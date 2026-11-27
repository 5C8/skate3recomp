// Ported from sk83_na_m (src/sk83_guest_profiler.cpp, 2026-11-14).
// Guest render thread sampling profiler (DIAGNOSTIC, off until armed).
//
// Purpose: explain the guest render thread's SLOW frames (the smoothness
// monitor charges ~all remaining drops/bad steps to them). Measured: a normal
// frame is pure CPU (the thread runs ~99% of it); a slow frame adds both
// running time (more game work) and 0.5-4.5 ms of not running (blocked or
// preempted). This names both.
//
// A host thread samples the guest render thread at ~2 kHz: suspend, read its
// context, unwind the host stack with the image's .pdata
// (RtlLookupFunctionEntry/RtlVirtualUnwind; no allocation, nothing touched
// that the suspended thread could hold except the function-table lookup),
// resume. Each frame's function START is mapped back to a guest function via
// the recompiler's own {guest, host} table (skate3_PPCFuncMappings, which
// also maps the kernel import thunks __imp__Ke*/Nt*: a blocked sample shows
// exactly which kernel wait it is in). Running vs not: the thread's own
// cycle counter advanced since the previous sample or not.
//
// Samples are classified by the guest frame they fall in (frame entry times
// from BuildFrameScene; slow = > 1.5x the median frame) and the report lists
// the guest functions with the most EXTRA time per slow frame vs a normal
// one, leaf and inclusive, running and not running, as guest addresses
// (resolve names in generated/: rex_<name>_<ADDR>). Written to
// logs/guestprof_<ts>.txt when the window closes.

#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread/mutex.h>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_DOUBLE(skate3_guest_profile, 0.0, "Skate 3",
                      "DIAGNOSTIC: sample the guest render thread for this many seconds "
                      "(re-arms when the value changes); report in logs/guestprof_<ts>.txt");
REXCVAR_DEFINE_DOUBLE(skate3_guest_profile_delay, 0.0, "Skate 3",
                      "DIAGNOSTIC: start the armed guest profile this many seconds later "
                      "(e.g. arm it from the command line, sample once gameplay runs)");
REXCVAR_DEFINE_STRING(skate3_guest_profile_thread, "", "Skate 3",
                      "DIAGNOSTIC: sample the thread whose name contains this instead (frames "
                      "still come from the render/main thread), e.g. 'Job Thread'");
REXCVAR_DEFINE_INT32(skate3_guest_profile_thread_index, 0, "Skate 3",
                     "DIAGNOSTIC: which match of skate3_guest_profile_thread (0 = first)");
REXCVAR_DEFINE_BOOL(skate3_guest_profile_main, false, "Skate 3",
                    "DIAGNOSTIC: the next guest profile samples the main (sim) thread instead "
                    "(frames = sim ticks; report in logs/mainprof_<ts>.txt)");

#if !defined(_WIN32)
// The sampler suspends and unwinds threads with Win32 APIs; elsewhere the
// hooks are no-ops.
namespace skate3_guest_profiler {
void NoteFrame() {}
void NoteSimTick() {}
}  // namespace skate3_guest_profiler
#else

#include <windows.h>
#include <tlhelp32.h>

namespace {
constexpr int kDepth = 32;
constexpr size_t kMaxFrames = 1 << 16;

constexpr int kHolderDepth = 24;
struct Sample {
  int64_t qpc;
  uint64_t cycles;
  uint16_t depth;
  uint64_t begin[kDepth];  // host function starts, innermost first
  uint64_t leaf_pc;        // the innermost PC (host)
  // The SDK global critical region's owner at the sample (another thread),
  // and its stack: who the render thread waits for when blocked in it.
  uint32_t holder_tid;
  uint16_t holder_depth;
  uint64_t holder_cycles;
  uint64_t holder_begin[kHolderDepth];
  // The SDK's "GPU Commands" thread (command processor) at the same moment:
  // what it is busy with while the game waits on a GPU fence.
  uint16_t gpu_depth;
  uint64_t gpu_cycles;
  uint64_t gpu_begin[kHolderDepth];
};

// Unwind a SUSPENDED thread's host stack into function start addresses.
// No allocation (the suspended thread may hold the heap lock).
bool WalkThread(HANDLE th, uint64_t* begin, int max_depth, uint16_t* depth, uint64_t* leaf_pc) {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  *depth = 0;
  if (!GetThreadContext(th, &ctx)) return false;
  if (leaf_pc) *leaf_pc = ctx.Rip;
  for (int d = 0; d < max_depth && ctx.Rip != 0; ++d) {
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!fn) {
      begin[(*depth)++] = ctx.Rip;  // unknown start: keep the PC
      // Only the innermost frame may be a leaf without unwind data (return
      // address at [rsp]); deeper, the walk is lost: stop rather than chase
      // a garbage stack pointer.
      if (d != 0) break;
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    begin[(*depth)++] = image_base + fn->BeginAddress;
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                     &establisher, nullptr);
  }
  return true;
}

// Owner thread id of the SDK global critical region (MSVC recursive_mutex
// internals: _Mtx_internal_imp_t::_Thread_id), 0 when free.
uint32_t GlobalLockOwner() {
  // native_handle() was removed from the STL (GH-3820); _Mutex_base's only
  // member is the _Mtx_internal_imp_t (no vtable), so the object IS it.
  static_assert(sizeof(std::recursive_mutex) == sizeof(::_Mtx_internal_imp_t));
  auto* imp = reinterpret_cast<::_Mtx_internal_imp_t*>(
      &rex::thread::global_critical_region::mutex());
  const long id = reinterpret_cast<volatile const long&>(imp->_Thread_id);
  return id > 0 ? uint32_t(id) : 0;
}

std::atomic<uint32_t> g_render_tid{0};
std::atomic<uint32_t> g_main_tid{0};
// Thread being profiled: 0 = guest render thread (frames = BuildFrameScene
// entries), 1 = main/sim thread (frames = sim ticks, GenerateSyncCommand).
std::atomic<int> g_target{0};
std::atomic<bool> g_active{false};
// Frame entry times (QPC), written by the guest render thread only.
std::vector<int64_t> g_frames;
std::atomic<size_t> g_frame_n{0};

double g_armed_for = 0.0;

int64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

struct ModRange {
  uint64_t lo, hi;
};
ModRange ModuleRange(const char* name) {
  HMODULE m = GetModuleHandleA(name);
  if (!m) return {0, 0};
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
  return {uint64_t(m), uint64_t(m) + nt->OptionalHeader.SizeOfImage};
}

void RunProfile(double seconds) {
  const int target = g_target.load();
  const char* const label = target == 1 ? "main (sim) thread" : "guest render thread";
  const DWORD tid = target == 1 ? g_main_tid.load() : g_render_tid.load();
  if (tid == 0) {
    REXLOG_WARN("guest-profile: {} not seen yet", label);
    return;
  }
  HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, tid);
  // Optionally sample another thread (found by name; the n-th match) on the same frames,
  // e.g. a job thread against sim ticks.
  const std::string other = REXCVAR_GET(skate3_guest_profile_thread);
  if (th && !other.empty()) {
    const std::wstring want(other.begin(), other.end());
    int skip = REXCVAR_GET(skate3_guest_profile_thread_index);
    HANDLE found = nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
      do {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR d = nullptr;
        bool match = false;
        if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
          match = std::wstring(d).find(want) != std::wstring::npos;
          LocalFree(d);
        }
        if (match && skip-- == 0) {
          found = h;
          REXLOG_INFO("guest-profile: sampling thread {} ('{}') against the frames", te.th32ThreadID, other);
          break;
        }
        CloseHandle(h);
      } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    if (found) {
      CloseHandle(th);
      th = found;
    } else {
      REXLOG_WARN("guest-profile: no thread named '{}'", other);
    }
  }
  if (!th) {
    REXLOG_WARN("guest-profile: OpenThread failed ({})", GetLastError());
    return;
  }
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const size_t max_samples = size_t(seconds * 2200.0) + 64;
  std::vector<Sample> samples(max_samples);
  size_t ns = 0;
  g_frames.assign(kMaxFrames, 0);
  g_frame_n.store(0);
  g_active.store(true);

  struct HolderInfo {
    HANDLE h = nullptr;
    std::string name;
  };
  std::unordered_map<uint32_t, HolderInfo> holders;
  holders.reserve(64);

  // The command processor thread, found by name (render-thread profiles).
  HANDLE gpu_th = nullptr;
  if (target == 0) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
      do {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                  THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR d = nullptr;
        bool match = false;
        if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
          match = wcsncmp(d, L"GPU Commands", 12) == 0;
          LocalFree(d);
        }
        if (match) {
          gpu_th = h;
          break;
        }
        CloseHandle(h);
      } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    if (!gpu_th) REXLOG_WARN("guest-profile: GPU Commands thread not found");
  }

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  const int64_t end = Qpc() + int64_t(seconds * double(freq.QuadPart));
  REXLOG_INFO("guest-profile: sampling {} {} for {:.0f} s", label, tid, seconds);
  while (Qpc() < end && ns < max_samples) {
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -5000;  // 0.5 ms
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 5);
    } else {
      Sleep(1);
    }
    Sample& s = samples[ns];
    if (SuspendThread(th) == DWORD(-1)) break;
    const bool ok = WalkThread(th, s.begin, kDepth, &s.depth, &s.leaf_pc);
    const uint32_t owner = GlobalLockOwner();
    ResumeThread(th);
    ULONG64 cyc = 0;
    QueryThreadCycleTime(th, &cyc);
    s.cycles = cyc;
    s.qpc = Qpc();
    s.holder_tid = 0;
    s.holder_depth = 0;
    if (owner != 0 && owner != tid) {
      // Handle + name looked up BEFORE suspending the holder (allocates).
      auto it = holders.find(owner);
      if (it == holders.end()) {
        HolderInfo hi;
        hi.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_QUERY_INFORMATION, FALSE, owner);
        if (hi.h) {
          PWSTR desc = nullptr;
          if (SUCCEEDED(GetThreadDescription(hi.h, &desc)) && desc) {
            char buf[128];
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf), nullptr, nullptr);
            hi.name = buf;
            LocalFree(desc);
          }
        }
        if (hi.name.empty()) hi.name = "tid" + std::to_string(owner);
        it = holders.emplace(owner, std::move(hi)).first;
      }
      if (it->second.h && SuspendThread(it->second.h) != DWORD(-1)) {
        WalkThread(it->second.h, s.holder_begin, kHolderDepth, &s.holder_depth, nullptr);
        ResumeThread(it->second.h);
        ULONG64 hc = 0;
        QueryThreadCycleTime(it->second.h, &hc);
        s.holder_cycles = hc;
        s.holder_tid = owner;
      }
    }
    s.gpu_depth = 0;
    s.gpu_cycles = 0;
    if (gpu_th && SuspendThread(gpu_th) != DWORD(-1)) {
      WalkThread(gpu_th, s.gpu_begin, kHolderDepth, &s.gpu_depth, nullptr);
      ResumeThread(gpu_th);
      ULONG64 gc = 0;
      QueryThreadCycleTime(gpu_th, &gc);
      s.gpu_cycles = gc;
    }
    if (ok) ++ns;
  }
  g_active.store(false);
  if (gpu_th) CloseHandle(gpu_th);
  if (timer) CloseHandle(timer);
  CloseHandle(th);
  for (auto& [k, v] : holders) {
    if (v.h) CloseHandle(v.h);
  }

  // ---- Post-process (target running again; allocation is fine now). ----
  std::unordered_map<uint64_t, uint32_t> host_to_guest;
  for (const PPCFuncMapping* m = skate3_PPCFuncMappings; m->host != nullptr; ++m) {
    host_to_guest.emplace(uint64_t(reinterpret_cast<uintptr_t>(m->host)), uint32_t(m->guest));
  }
  const size_t nf = std::min(g_frame_n.load(), kMaxFrames);
  if (nf < 50 || ns < 100) {
    REXLOG_WARN("guest-profile: too little data ({} frames, {} samples)", nf, ns);
    return;
  }
  std::vector<double> dts;
  for (size_t i = 1; i < nf; ++i) dts.push_back(double(g_frames[i] - g_frames[i - 1]));
  std::vector<double> sorted = dts;
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  const double med = sorted[sorted.size() / 2];
  size_t n_slow = 0, n_norm = 0;
  for (double d : dts) (d > 1.5 * med ? n_slow : n_norm) += 1;
  if (n_slow == 0) {
    REXLOG_WARN("guest-profile: no slow frames in the window (slow columns are 0)");
  }

  const ModRange ntdll = ModuleRange("ntdll.dll");
  const ModRange kbase = ModuleRange("kernelbase.dll");
  const ModRange k32 = ModuleRange("kernel32.dll");
  const auto in_os = [&](uint64_t pc) {
    return (pc >= ntdll.lo && pc < ntdll.hi) || (pc >= kbase.lo && pc < kbase.hi) ||
           (pc >= k32.lo && pc < k32.hi);
  };

  // key: guest addr (or host addr | 1<<63 for non-guest leaves)
  struct Acc {
    double slow_run = 0, slow_idle = 0, norm_run = 0, norm_idle = 0;
  };
  std::unordered_map<uint64_t, Acc> leaf, incl, wait_site;
  const double tsc_per_qpc = [&] {
    // Thread cycles are TSC-rate; estimate TSC per QPC tick from rdtsc.
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const uint64_t t0 = __rdtsc();
    Sleep(20);
    QueryPerformanceCounter(&b);
    const uint64_t t1 = __rdtsc();
    return double(t1 - t0) / double(b.QuadPart - a.QuadPart);
  }();
  size_t fi = 0;
  size_t n_in_frames = 0, n_guest = 0, n_depth0 = 0;
  double sample_ms = 1000.0 * double(seconds) / double(ns);
  for (size_t i = 1; i < ns; ++i) {
    const Sample& s = samples[i];
    while (fi + 1 < nf && g_frames[fi + 1] <= s.qpc) ++fi;
    if (fi + 1 >= nf || g_frames[fi] > s.qpc) continue;  // outside recorded frames
    ++n_in_frames;
    if (s.depth == 0) ++n_depth0;
    const double dt = double(g_frames[fi + 1] - g_frames[fi]);
    const bool slow = dt > 1.5 * med;
    const double interval = double(s.qpc - samples[i - 1].qpc) * tsc_per_qpc;
    const bool running = double(s.cycles - samples[i - 1].cycles) > 0.3 * interval;
    const auto add = [&](Acc& a) {
      (slow ? (running ? a.slow_run : a.slow_idle) : (running ? a.norm_run : a.norm_idle)) += 1;
    };
    // Leaf guest function (innermost frame that is a guest function).
    uint64_t lg = 0;
    for (int d = 0; d < s.depth; ++d) {
      const auto it = host_to_guest.find(s.begin[d]);
      if (it != host_to_guest.end()) {
        lg = it->second;
        break;
      }
    }
    if (lg) ++n_guest;
    add(leaf[lg ? lg : (s.begin[0] | (1ull << 63))]);
    // Inclusive: every distinct guest function on the stack.
    uint64_t seen[kDepth];
    int nseen = 0;
    for (int d = 0; d < s.depth; ++d) {
      const auto it = host_to_guest.find(s.begin[d]);
      if (it == host_to_guest.end()) continue;
      bool dup = false;
      for (int k = 0; k < nseen; ++k) dup |= seen[k] == it->second;
      if (dup) continue;
      seen[nseen++] = it->second;
      add(incl[it->second]);
    }
    if (!running) {
      // Where it is blocked: the innermost guest function + whether the
      // host leaf is in the OS (a real wait/syscall) or our/SDK code.
      add(wait_site[(lg ? lg : 0) | (in_os(s.leaf_pc) ? (1ull << 62) : 0)]);
    }
  }

  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  const std::string path = std::string(target == 1 ? "logs/mainprof_" : "logs/guestprof_") + std::to_string(stamp) + ".txt";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return;
  std::fprintf(f,
               "%s profile: %zu samples (%.2f ms each), %zu frames, median "
               "%.2f ms, slow (>1.5x) %zu\n"
               "columns: extra ms per slow frame vs a normal frame (total, running, not "
               "running) | slow-frame ms run/idle | normal-frame ms run/idle | key\n"
               "key: guest address (resolve in generated/), H:host address = non-guest leaf; "
               "wait sites: +OS = host leaf inside ntdll/kernelbase (blocked in a syscall)\n",
               label, ns, sample_ms, nf, 1000.0 * med / double(freq.QuadPart), n_slow);
  {
    double max_incl = 0, sum_depth = 0;
    for (auto& [k, a] : incl) max_incl = std::max(max_incl, a.norm_run + a.norm_idle + a.slow_run + a.slow_idle);
    for (size_t i = 1; i < ns; ++i) sum_depth += samples[i].depth;
    std::fprintf(f, "samples in frames %zu, with a guest frame %zu, empty stacks %zu; frames %zu normal; "
                 "incl keys %zu leaf keys %zu, max incl count %.0f, mean depth %.1f\n",
                 n_in_frames, n_guest, n_depth0, n_norm, incl.size(), leaf.size(), max_incl,
                 sum_depth / double(std::max<size_t>(ns, 1)));
  }
  const auto dump = [&](const char* title, std::unordered_map<uint64_t, Acc>& m, size_t top,
                        bool by_normal = false) {
    struct Row {
      uint64_t k;
      double extra, extra_run, extra_idle;
      Acc a;
    };
    std::vector<Row> rows;
    for (auto& [k, a] : m) {
      const double sr = a.slow_run * sample_ms / double(std::max<size_t>(n_slow, 1));
      const double si = a.slow_idle * sample_ms / double(std::max<size_t>(n_slow, 1));
      const double nr = a.norm_run * sample_ms / double(std::max<size_t>(n_norm, 1));
      const double ni = a.norm_idle * sample_ms / double(std::max<size_t>(n_norm, 1));
      rows.push_back(Row{k, (sr + si) - (nr + ni), sr - nr, si - ni, a});
    }
    if (by_normal) {
      // where a typical frame's running time goes (ignores the slow/normal split)
      std::sort(rows.begin(), rows.end(),
                [](const Row& x, const Row& y) { return x.a.norm_run > y.a.norm_run; });
    } else {
      std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.extra > y.extra; });
    }
    std::fprintf(f, "\n== %s ==\n", title);
    for (size_t i = 0; i < rows.size() && i < top; ++i) {
      const Row& r = rows[i];
      const double sr = r.a.slow_run * sample_ms / double(std::max<size_t>(n_slow, 1));
      const double si = r.a.slow_idle * sample_ms / double(std::max<size_t>(n_slow, 1));
      const double nr = r.a.norm_run * sample_ms / double(std::max<size_t>(n_norm, 1));
      const double ni = r.a.norm_idle * sample_ms / double(std::max<size_t>(n_norm, 1));
      char key[64];
      if (r.k & (1ull << 63)) {
        std::snprintf(key, sizeof(key), "H:%016llx", (unsigned long long)(r.k & ~(1ull << 63)));
      } else {
        std::snprintf(key, sizeof(key), "%08X%s", uint32_t(r.k),
                      (r.k & (1ull << 62)) ? " +OS" : "");
      }
      std::fprintf(f, "%+7.3f %+7.3f %+7.3f | %6.3f %6.3f | %6.3f %6.3f | %s\n", r.extra,
                   r.extra_run, r.extra_idle, sr, si, nr, ni, key);
    }
  };
  // Global-lock holders while the render thread was NOT running: thread
  // name | holder's innermost guest function | its innermost host frames in
  // the exe that are not guest functions (SDK code; exe RVAs, symbolize
  // offline) | holder running or itself switched out.
  {
    const uint64_t exe_lo = uint64_t(GetModuleHandleW(nullptr));
    const ModRange exe = [&] {
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_lo);
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe_lo + dos->e_lfanew);
      return ModRange{exe_lo, exe_lo + nt->OptionalHeader.SizeOfImage};
    }();
    std::unordered_map<uint32_t, std::pair<int64_t, uint64_t>> last_seen;  // tid -> qpc, cycles
    struct HAcc {
      double slow = 0, norm = 0;
    };
    std::unordered_map<std::string, HAcc> by_holder;
    double blocked_slow = 0, blocked_norm = 0, held_slow = 0, held_norm = 0;
    size_t fj = 0;
    for (size_t i = 1; i < ns; ++i) {
      const Sample& s = samples[i];
      bool holder_running = false;
      if (s.holder_tid) {
        auto it = last_seen.find(s.holder_tid);
        if (it != last_seen.end() && s.qpc > it->second.first) {
          const double iv = double(s.qpc - it->second.first) * tsc_per_qpc;
          holder_running = double(s.holder_cycles - it->second.second) > 0.3 * iv;
        }
        last_seen[s.holder_tid] = {s.qpc, s.holder_cycles};
      }
      while (fj + 1 < nf && g_frames[fj + 1] <= s.qpc) ++fj;
      if (fj + 1 >= nf || g_frames[fj] > s.qpc) continue;
      const bool slow = double(g_frames[fj + 1] - g_frames[fj]) > 1.5 * med;
      const double interval = double(s.qpc - samples[i - 1].qpc) * tsc_per_qpc;
      const bool running = double(s.cycles - samples[i - 1].cycles) > 0.3 * interval;
      if (running) continue;
      (slow ? blocked_slow : blocked_norm) += 1;
      if (!s.holder_tid) continue;
      (slow ? held_slow : held_norm) += 1;
      std::string key = holders.count(s.holder_tid) ? holders[s.holder_tid].name : "?";
      uint32_t hg = 0;
      for (int d = 0; d < s.holder_depth; ++d) {
        const auto it = host_to_guest.find(s.holder_begin[d]);
        if (it != host_to_guest.end()) {
          hg = it->second;
          break;
        }
      }
      char buf[64];
      std::snprintf(buf, sizeof(buf), " | guest %08X | host", hg);
      key += buf;
      int shown = 0;
      for (int d = 0; d < s.holder_depth && shown < 3; ++d) {
        const uint64_t b = s.holder_begin[d];
        if (host_to_guest.count(b)) break;  // reached guest code
        if (b >= exe.lo && b < exe.hi) {
          std::snprintf(buf, sizeof(buf), " exe+%llX", (unsigned long long)(b - exe.lo));
        } else {
          std::snprintf(buf, sizeof(buf), " %s", in_os(b) ? "os" : "ext");
        }
        key += buf;
        ++shown;
      }
      key += holder_running ? " | holder RUNNING" : " | holder switched out";
      auto& a = by_holder[key];
      (slow ? a.slow : a.norm) += 1;
    }
    std::fprintf(f,
                 "\n== global lock holder while the profiled thread was blocked ==\n"
                 "blocked ms per slow frame %.3f (held by another thread %.3f) | per normal "
                 "frame %.3f (held %.3f)\n",
                 blocked_slow * sample_ms / double(std::max<size_t>(n_slow, 1)), held_slow * sample_ms / double(std::max<size_t>(n_slow, 1)),
                 blocked_norm * sample_ms / double(std::max<size_t>(n_norm, 1)), held_norm * sample_ms / double(std::max<size_t>(n_norm, 1)));
    std::vector<std::pair<double, std::string>> hrows;
    for (auto& [k, a] : by_holder) {
      hrows.push_back({a.slow * sample_ms / double(std::max<size_t>(n_slow, 1)) - a.norm * sample_ms / double(std::max<size_t>(n_norm, 1)),
                       k});
    }
    std::sort(hrows.begin(), hrows.end(), [](auto& x, auto& y) { return x.first > y.first; });
    for (size_t i = 0; i < hrows.size() && i < 40; ++i) {
      std::fprintf(f, "%+7.3f | %s\n", hrows[i].first, hrows[i].second.c_str());
    }
  }
  // GPU command processor while the render thread waits on a GPU fence
  // (D3D CBlocker::Check / CDevice::BlockOnFence spin). Keys are host
  // functions as module+RVA (symbolize offline: exe via its PDB, the SDK
  // runtime DLL via its export table).
  if (target == 0) {
    std::unordered_map<uint64_t, std::string> mod_name;  // module base -> name
    const auto key_of = [&](uint64_t addr) {
      HMODULE m = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(addr), &m) ||
          !m) {
        char b[32];
        std::snprintf(b, sizeof(b), "?%llX", (unsigned long long)addr);
        return std::string(b);
      }
      auto it = mod_name.find(uint64_t(m));
      if (it == mod_name.end()) {
        wchar_t w[MAX_PATH];
        GetModuleFileNameW(m, w, MAX_PATH);
        std::wstring ws(w);
        ws = ws.substr(ws.find_last_of(L"\\/") + 1);
        char b[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, b, sizeof(b), nullptr, nullptr);
        it = mod_name.emplace(uint64_t(m), std::string(b)).first;
      }
      char b[64];
      std::snprintf(b, sizeof(b), "+%llX", (unsigned long long)(addr - uint64_t(m)));
      return it->second + b;
    };
    std::unordered_map<std::string, double> g_incl, g_leaf;
    double wait_n = 0, wait_gpu_run = 0, other_n = 0, other_gpu_run = 0;
    for (size_t i = 1; i < ns; ++i) {
      const Sample& s = samples[i];
      if (s.gpu_depth == 0 || samples[i - 1].gpu_depth == 0) continue;
      uint32_t lg = 0;
      for (int d = 0; d < s.depth; ++d) {
        const auto it = host_to_guest.find(s.begin[d]);
        if (it != host_to_guest.end()) {
          lg = it->second;
          break;
        }
      }
      const double interval = double(s.qpc - samples[i - 1].qpc) * tsc_per_qpc;
      const bool gpu_running = double(s.gpu_cycles - samples[i - 1].gpu_cycles) > 0.3 * interval;
      const bool waiting = lg == 0x82B76080 || lg == 0x82B755C0;  // retail CBlocker::Check, CDevice::BlockOnFence
      if (!waiting) {
        other_n += 1;
        other_gpu_run += gpu_running ? 1 : 0;
        continue;
      }
      wait_n += 1;
      wait_gpu_run += gpu_running ? 1 : 0;
      g_leaf[key_of(s.gpu_begin[0]) + (gpu_running ? "" : " [idle]")] += 1;
      uint64_t seen[kHolderDepth];
      int nseen = 0;
      for (int d = 0; d < s.gpu_depth; ++d) {
        bool dup = false;
        for (int k = 0; k < nseen; ++k) dup |= seen[k] == s.gpu_begin[d];
        if (dup) continue;
        seen[nseen++] = s.gpu_begin[d];
        g_incl[key_of(s.gpu_begin[d])] += 1;
      }
    }
    std::fprintf(f,
                 "\n== GPU Commands thread while the render thread waits on a GPU fence ==\n"
                 "fence-wait samples %.0f (%.1f%% of all; %.2f ms per frame) | GPU thread "
                 "running %.1f%% of them (vs %.1f%% otherwise)\n",
                 wait_n, 100.0 * wait_n / std::max(1.0, wait_n + other_n),
                 wait_n * sample_ms / double(n_slow + n_norm),
                 100.0 * wait_gpu_run / std::max(1.0, wait_n),
                 100.0 * other_gpu_run / std::max(1.0, other_n));
    const auto dump_s = [&](const char* title, std::unordered_map<std::string, double>& m,
                            size_t top) {
      std::vector<std::pair<double, std::string>> rows;
      for (auto& [k, v] : m) rows.push_back({v, k});
      std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.first > y.first; });
      std::fprintf(f, "-- %s (%% of fence-wait samples) --\n", title);
      for (size_t i = 0; i < rows.size() && i < top; ++i) {
        std::fprintf(f, "%6.1f%% | %s\n", 100.0 * rows[i].first / std::max(1.0, wait_n),
                     rows[i].second.c_str());
      }
    };
    dump_s("inclusive", g_incl, 50);
    dump_s("leaf", g_leaf, 25);
  }
  dump("leaf guest function", leaf, 40);
  dump("inclusive guest function", incl, 60);
  dump("not-running sites", wait_site, 30);
  dump("normal frame: leaf guest function (sorted by normal-frame running ms)", leaf, 120, true);
  dump("normal frame: inclusive guest function (sorted by normal-frame running ms)", incl, 400, true);
  std::fclose(f);
  REXLOG_INFO("guest-profile: wrote {} ({} samples, {} frames, {} slow)", path, ns, nf, n_slow);
}

}  // namespace

namespace skate3_guest_profiler {
// Called by the guest render thread at every frame entry (BuildFrameScene).
void NoteFrame() {
  g_render_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  if (g_active.load(std::memory_order_relaxed) && g_target.load(std::memory_order_relaxed) == 0) {
    const size_t i = g_frame_n.load(std::memory_order_relaxed);
    if (i < kMaxFrames) {
      g_frames[i] = Qpc();
      g_frame_n.store(i + 1, std::memory_order_release);
    }
  }
  // Arming (cheap cvar poll on the guest thread; the work runs elsewhere).
  const double secs = REXCVAR_GET(skate3_guest_profile);
  if (secs > 0.0 && secs != g_armed_for) {
    g_armed_for = secs;
    g_target.store(REXCVAR_GET(skate3_guest_profile_main) ? 1 : 0);
    static std::atomic<bool> s_running{false};
    if (!s_running.exchange(true)) {
      const double delay = REXCVAR_GET(skate3_guest_profile_delay);
      std::thread([secs, delay] {
        if (delay > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(delay));
        RunProfile(std::min(secs, 120.0));
        s_running.store(false);
      }).detach();
    }
  } else if (secs <= 0.0) {
    g_armed_for = 0.0;
  }
}
// Called by the main thread when the sim stores a frame (retail:
// DataTracker::StoreFrame, see skate3_present_clock.cpp). Several trackers
// store per sim tick, within well under 2 ms: only the first counts.
void NoteSimTick() {
  g_main_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  static int64_t s_last = 0;
  const int64_t now_qpc = Qpc();
  LARGE_INTEGER fq;
  QueryPerformanceFrequency(&fq);
  const bool same_tick = now_qpc - s_last < fq.QuadPart / 500;
  s_last = now_qpc;
  if (same_tick) return;
  if (g_active.load(std::memory_order_relaxed) && g_target.load(std::memory_order_relaxed) == 1) {
    const size_t i = g_frame_n.load(std::memory_order_relaxed);
    if (i < kMaxFrames) {
      g_frames[i] = Qpc();
      g_frame_n.store(i + 1, std::memory_order_release);
    }
  }
}
}  // namespace skate3_guest_profiler
#endif  // _WIN32
