// RETAIL port of sk83_na_m src/sk83_guest_replay.cpp (same cvars, same snapshot format; snapshots
// are per build -- never replay sk83 snapshots here). Capture points: native/skate3_native_render_game.cpp
// (render body 827FF7D0, SceneRenderView::Render 827FB158, Presentation::Update 8279E350,
// UpdateFrameBegin 8281E9B0, Update3DMixCtls 82951658), skate3_native_render.cpp (draw-list dispatch
// 827FAF50; replay at the guest Swap 82B82E08), native/skate3_audio_thread.cpp (AudioSystem::Update 82485190).
//
// Guest-state capture and replay: exact verification of decompiled render-thread code on real
// game state (REXGLUE/skate-physics-decomp docs/render_thread_plan.md).
//
// Capture (any run, e.g. the demo flythrough):
//   --sk83_capture_dir=E:\sk83_snapshots --sk83_capture_fn=827FAF50 --sk83_capture_count=6
//   [--sk83_capture_every=500 --sk83_capture_delay=60]
// At a capture point (a hooked guest function that calls sk83_replay::CapturePoint first) every
// other thread is suspended and the committed guest memory (virtual 0x00000000-0x9FFFFFFF and
// physical memory through its 0xA0000000 view) plus the caller's PPCContext is written to
// snap_<fn>_<n>.bin.
//
// Replay: --sk83_replay=E:\sk83_snapshots. At the first guest Swap the other threads are stopped
// for good; every snapshot is loaded and its function run twice from the same state -- the
// decomp toggle (sk83_native_render_game) off, then on -- with our render hooks off
// (skate3_native_render); all of guest memory is compared after the two runs and the
// differences logged to <dir>/replay_report.txt, then the process exits. --sk83_replay_selftest
// runs "off" twice instead (determinism check).
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(sk83_capture_dir, "", "Skate 3",
                      "DIAGNOSTIC: directory for guest-state snapshots (capture) ");
REXCVAR_DEFINE_STRING(sk83_capture_fn, "", "Skate 3",
                      "DIAGNOSTIC: hex address of the capture point to snapshot (e.g. 827FF7D0)");
REXCVAR_DEFINE_INT32(sk83_capture_count, 6, "Skate 3", "DIAGNOSTIC: snapshots to take");
REXCVAR_DEFINE_INT32(sk83_capture_every, 500, "Skate 3",
                     "DIAGNOSTIC: take a snapshot every this many calls of the capture point");
REXCVAR_DEFINE_DOUBLE(sk83_capture_delay, 60.0, "Skate 3",
                      "DIAGNOSTIC: seconds after start before the first snapshot");
REXCVAR_DEFINE_STRING(sk83_replay, "", "Skate 3",
                      "DIAGNOSTIC: replay the snapshots in this directory at the first Swap, "
                      "compare decomp off/on, write replay_report.txt and exit");
REXCVAR_DEFINE_BOOL(sk83_replay_selftest, false, "Skate 3",
                    "DIAGNOSTIC: replay runs the reference twice (determinism check)");
REXCVAR_DECLARE(bool, sk83_native_render_game);   // native/skate3_native_render_game.cpp
REXCVAR_DECLARE(bool, sk83_native_render_game_unproven);
REXCVAR_DECLARE(bool, skate3_native_render);

#if !defined(_WIN32)
// The harness suspends threads and maps snapshot memory with Win32 APIs; elsewhere it is off.
namespace sk83_replay {
bool Replaying() { return false; }
void CapturePoint(uint32_t, PPCContext&, uint8_t*) {}
void MaybeReplay(uint8_t*) {}
}  // namespace sk83_replay
#else

namespace {

constexpr uint32_t kMagic = 0x53383352;   // 'S83R'
constexpr uint32_t kVersion = 1;

struct Region { uint32_t addr, size; };

// The recompiler's indirect-call table (REX_LOOKUP_FUNC) lives inside guest space: host function
// pointers that differ per process (ASLR). Never captured, never restored, never compared.
constexpr uint64_t kTableLo = REX_IMAGE_BASE + REX_IMAGE_SIZE;
constexpr uint64_t kTableHi = kTableLo + (REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) * 2;

// The 0x90000000 view mirrors the 0x80000000 heap, so the table is excluded in both.
constexpr uint64_t kMirror = 0x10000000;
bool InTable(uint64_t a) { return (a >= kTableLo && a < kTableHi) || (a >= kTableLo + kMirror && a < kTableHi + kMirror); }

// the parts of [addr, addr + size) outside the table (both views): (offset in the region, length)
std::vector<std::pair<uint32_t, uint32_t>> Parts(const Region& r) {
  std::vector<std::pair<uint32_t, uint32_t>> v;
  const uint64_t a = r.addr, e = a + r.size;
  uint64_t cur = a;
  for (uint64_t lo : {kTableLo, kTableLo + kMirror}) {
    const uint64_t hi = lo + (kTableHi - kTableLo);
    if (e <= lo || cur >= hi) continue;
    if (cur < lo) v.push_back({uint32_t(cur - a), uint32_t(lo - cur)});
    cur = hi;
  }
  if (cur < e) v.push_back({uint32_t(cur - a), uint32_t(e - cur)});
  return v;
}

// committed, readable guest regions: virtual 0..0x9FFFFFFF, physical through the 0xA0000000 view
std::vector<Region> GuestRegions(uint8_t* base) {
  std::vector<Region> out;
  const uint64_t lo[2] = {0x00000000ull, 0xA0000000ull}, hi[2] = {0xA0000000ull, 0xC0000000ull};
  for (int r = 0; r < 2; ++r) {
    uint64_t a = lo[r];
    while (a < hi[r]) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
      const uint64_t start = uint64_t((uint8_t*)mbi.BaseAddress - base);
      uint64_t end = start + mbi.RegionSize;
      if (end > hi[r]) end = hi[r];
      const DWORD p = mbi.Protect & 0xff;
      if (mbi.State == MEM_COMMIT && p != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD)) {
        const uint64_t s = start < a ? a : start;
        // merge only within one 0x10000000 view (0x7F.. is the GPU/MMIO view)
        if (!out.empty() && out.back().addr + uint64_t(out.back().size) == s && (out.back().addr >> 28) == (s >> 28))
          out.back().size += uint32_t(end - s);
        else
          out.push_back({uint32_t(s), uint32_t(end - s)});
      }
      a = end;
    }
  }
  return out;
}

std::vector<HANDLE> SuspendOthers() {
  std::vector<HANDLE> hs;
  const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
    if (!h) continue;
    if (SuspendThread(h) == DWORD(-1)) { CloseHandle(h); continue; }
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL;
    GetThreadContext(h, &c);   // waits until the thread really stopped
    hs.push_back(h);
  }
  CloseHandle(snap);
  return hs;
}

void Resume(std::vector<HANDLE>& hs) {
  for (HANDLE h : hs) { ResumeThread(h); CloseHandle(h); }
  hs.clear();
}

bool WriteAll(HANDLE f, const void* p, uint64_t n) {
  const uint8_t* b = (const uint8_t*)p;
  while (n) {
    DWORD w = 0, k = DWORD(n > (1u << 30) ? (1u << 30) : n);
    if (!WriteFile(f, b, k, &w, nullptr) || !w) return false;
    b += w; n -= w;
  }
  return true;
}
bool ReadAll(HANDLE f, void* p, uint64_t n) {
  uint8_t* b = (uint8_t*)p;
  while (n) {
    DWORD r = 0, k = DWORD(n > (1u << 30) ? (1u << 30) : n);
    if (!ReadFile(f, b, k, &r, nullptr) || !r) return false;
    b += r; n -= r;
  }
  return true;
}

PPCFunc* Lookup(uint32_t addr) {
  for (const PPCFuncMapping* m = skate3_PPCFuncMappings; m->host; ++m)
    if (m->guest == addr) return m->host;
  return nullptr;
}

FILE* g_rep = nullptr;
uint8_t* g_base = nullptr;

// last-chance handler while replaying: where the replayed code faulted
LONG CALLBACK ReplayCrash(EXCEPTION_POINTERS* e) {
  if (!g_rep) return EXCEPTION_CONTINUE_SEARCH;
  const auto* r = e->ExceptionRecord;
  // only real faults: C++ throws and the like are first-chance here and get handled further up
  if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && r->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
      r->ExceptionCode != EXCEPTION_STACK_OVERFLOW && r->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
    return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t rip = e->ContextRecord->Rip;
  uint32_t guest_fn = 0;
  DWORD64 ib = 0;
  if (auto* fe = RtlLookupFunctionEntry(rip, &ib, nullptr)) {
    const uint64_t begin = ib + fe->BeginAddress;
    for (const PPCFuncMapping* m = skate3_PPCFuncMappings; m->host; ++m)
      if (uint64_t(m->host) == begin) { guest_fn = uint32_t(m->guest); break; }
  }
  const uint64_t fa = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
  std::fprintf(g_rep, "  EXCEPTION %08lX at host %llX (guest function %08X), %s address %llX (guest %08llX)\n",
               r->ExceptionCode, (unsigned long long)rip, guest_fn,
               r->NumberParameters && r->ExceptionInformation[0] ? "write" : "read", (unsigned long long)fa,
               (unsigned long long)(fa - uint64_t(g_base)));
  // a bad indirect jump: who called it (return address on the stack), and what the target is
  const uint64_t ret = *(const uint64_t*)e->ContextRecord->Rsp;
  uint32_t caller = 0;
  if (auto* fe = RtlLookupFunctionEntry(ret, &ib, nullptr)) {
    const uint64_t begin = ib + fe->BeginAddress;
    for (const PPCFuncMapping* m = skate3_PPCFuncMappings; m->host; ++m)
      if (uint64_t(m->host) == begin) { caller = uint32_t(m->guest); break; }
    std::fprintf(g_rep, "  return address %llX = %s+%llX (guest function %08X)\n", (unsigned long long)ret,
                 "image", (unsigned long long)(ret - ib), caller);
  }
  HMODULE mod = nullptr;
  char mname[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR)rip, &mod))
    GetModuleFileNameA(mod, mname, MAX_PATH);
  std::fprintf(g_rep, "  target %llX is in %s (base %p), exe base %p\n", (unsigned long long)rip, mname, (void*)mod,
               (void*)GetModuleHandleA(nullptr));
  std::fflush(g_rep);
  TerminateProcess(GetCurrentProcess(), 3);
  return EXCEPTION_CONTINUE_SEARCH;
}

std::atomic<int> g_taken{0};
std::atomic<uint64_t> g_calls{0};
LARGE_INTEGER g_t0{};

}  // namespace

namespace sk83_render_game { void ReportCoverage(FILE* f); }   // native/skate3_native_render_game.cpp

namespace sk83_replay {

static std::atomic<bool> g_replaying{false};
// true while MaybeReplay runs (hooks that hand work to other threads must run inline)
bool Replaying() { return g_replaying.load(std::memory_order_relaxed); }

// Called at the entry of a capture-point hook, on the calling guest thread.
void CapturePoint(uint32_t fn, PPCContext& ctx, uint8_t* base) {
  static const std::string dir = REXCVAR_GET(sk83_capture_dir);
  if (dir.empty()) return;
  // sk83_capture_fn: one or more hex addresses ("827FF7D0,8279E350"), counted separately
  constexpr int kMaxFns = 16;
  static uint32_t want[kMaxFns];
  static int nwant = [] {
    int k = 0;
    const std::string list = REXCVAR_GET(sk83_capture_fn);
    for (size_t p = 0; p < list.size() && k < kMaxFns;) {
      size_t q = list.find(',', p);
      if (q == std::string::npos) q = list.size();
      want[k++] = uint32_t(std::strtoul(list.substr(p, q - p).c_str(), nullptr, 16));
      p = q + 1;
    }
    return k;
  }();
  static std::atomic<int> taken[kMaxFns];
  static std::atomic<uint64_t> calls[kMaxFns];
  int slot = -1;
  for (int k = 0; k < nwant; ++k)
    if (want[k] == fn) slot = k;
  if (slot < 0 || taken[slot].load() >= REXCVAR_GET(sk83_capture_count)) return;
  LARGE_INTEGER now, fq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&fq);
  if (!g_t0.QuadPart) g_t0 = now;
  if (double(now.QuadPart - g_t0.QuadPart) / double(fq.QuadPart) < REXCVAR_GET(sk83_capture_delay)) return;
  if (calls[slot].fetch_add(1) % uint64_t(REXCVAR_GET(sk83_capture_every))) return;
  const int n = taken[slot].fetch_add(1);
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\snap_%08X_%03d.bin", dir.c_str(), fn, n);
  CreateDirectoryA(dir.c_str(), nullptr);
  // file handle and region list before anything is suspended (no heap use while stopped)
  HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  std::vector<Region> regs;
  regs.reserve(4096);
  std::vector<HANDLE> stopped;
  stopped.reserve(512);
  {
    auto tmp = SuspendOthers();
    stopped.swap(tmp);
  }
  {
    std::vector<Region> all = GuestRegions(base);
    for (auto& r : all)
      for (auto [po, pn] : Parts(r)) regs.push_back({r.addr + po, pn});
  }
  uint64_t total = 0;
  for (auto& r : regs) total += r.size;
  const uint32_t hdr[6] = {kMagic, kVersion, fn, uint32_t(sizeof(PPCContext)), uint32_t(regs.size()), 0};
  bool ok = WriteAll(f, hdr, sizeof(hdr)) && WriteAll(f, &ctx, sizeof(PPCContext)) &&
            WriteAll(f, regs.data(), regs.size() * sizeof(Region));
  for (auto& r : regs) ok = ok && WriteAll(f, base + r.addr, r.size);
  Resume(stopped);
  CloseHandle(f);
  REXLOG_INFO("sk83 capture: {} {} ({} regions, {:.1f} MB)", path, ok ? "written" : "FAILED",
              regs.size(), double(total) / 1048576.0);
}

// Called at the first guest Swap; returns only when no replay was requested.
void MaybeReplay(uint8_t* base) {
  static bool done = false;
  const std::string dir = REXCVAR_GET(sk83_replay);
  if (done || dir.empty()) return;
  done = true;
  const bool selftest = REXCVAR_GET(sk83_replay_selftest);
  std::vector<std::string> files;
  {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\snap_*.bin").c_str(), &fd);
    for (BOOL ok = h != INVALID_HANDLE_VALUE; ok; ok = FindNextFileA(h, &fd)) files.push_back(dir + "\\" + fd.cFileName);
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
  }
  FILE* rep = std::fopen((dir + "\\replay_report.txt").c_str(), "w");
  if (!rep) return;
  std::fprintf(rep, "replay %zu snapshots (%s)\n", files.size(), selftest ? "self-test: off vs off" : "decomp off vs on");
  std::fflush(rep);
  g_replaying = true;
  auto stopped = SuspendOthers();   // for good: the process exits after the replay
  g_rep = rep;
  g_base = base;
  AddVectoredExceptionHandler(0, ReplayCrash);   // after the SDK's own (MMIO / watch) handlers
  REXCVAR_SET(skate3_native_render, false);
  int bad_total = 0;
  for (const auto& path : files) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    uint32_t hdr[6] = {};
    if (f == INVALID_HANDLE_VALUE || !ReadAll(f, hdr, sizeof(hdr)) || hdr[0] != kMagic || hdr[1] != kVersion ||
        hdr[3] != sizeof(PPCContext)) {
      std::fprintf(rep, "%s: unreadable or other format\n", path.c_str());
      if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
      continue;
    }
    PPCContext snap_ctx;
    std::vector<Region> regs(hdr[4]);
    ReadAll(f, &snap_ctx, sizeof(PPCContext));
    ReadAll(f, regs.data(), regs.size() * sizeof(Region));
    uint64_t total = 0;
    for (auto& r : regs) total += r.size;
    std::vector<uint8_t> image(total), after(total);
    const bool complete = ReadAll(f, image.data(), total);
    CloseHandle(f);
    if (!complete) {   // a capture cut short (process stopped while writing)
      std::fprintf(rep, "%s: truncated snapshot, skipped\n", path.c_str());
      continue;
    }
    // make every snapshot region present and writable here
    int nfail = 0;
    std::vector<char> skip(regs.size(), 0);
    for (size_t ri = 0; ri < regs.size(); ++ri) {
      auto& r = regs[ri];
      void* c = VirtualAlloc(base + r.addr, r.size, MEM_COMMIT, PAGE_READWRITE);
      DWORD old;
      BOOL pr = VirtualProtect(base + r.addr, r.size, PAGE_READWRITE, &old);
      if (!c || !pr) {
        skip[ri] = 1;
        if (nfail++ < 10)
          std::fprintf(rep, "   region %08X+%X: commit %s protect %s (err %lu)\n", r.addr, r.size, c ? "ok" : "FAILED",
                       pr ? "ok" : "FAILED", GetLastError());
      }
    }
    std::fprintf(rep, "%s: %zu regions mapped (%d failed); running\n", path.c_str(), regs.size(), nfail);
    std::fflush(rep);
    const uint32_t fn = hdr[2];
    auto load = [&] {
      uint64_t o = 0;
      for (size_t ri = 0; ri < regs.size(); ++ri) {
        if (!skip[ri])
          for (auto [po, pn] : Parts(regs[ri])) std::memcpy(base + regs[ri].addr + po, image.data() + o + po, pn);
        o += regs[ri].size;
      }
    };
    double last_us = 0;
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    auto run = [&](bool native) {
      load();
      REXCVAR_SET(sk83_native_render_game, native);
      REXCVAR_SET(sk83_native_render_game_unproven, native);   // the decomp run includes unproven functions
      PPCContext c = snap_ctx;
      c.fpscr.loadFromHost();
      // the capture point's guest function (the hook, which honours the toggles itself)
      PPCFunc* fp = Lookup(fn);
      LARGE_INTEGER q0, q1;
      QueryPerformanceCounter(&q0);
      if (fp) fp(c, base);
      QueryPerformanceCounter(&q1);
      last_us = double(q1.QuadPart - q0.QuadPart) * 1e6 / double(qf.QuadPart);
      return fp != nullptr;
    };
    {
      auto sum = [&] {
        uint64_t h = 0;
        for (uint64_t a = kTableLo; a < kTableHi; a += 8) h = h * 31 + *(const uint64_t*)(base + a);
        return h;
      };
      // the dispatch table must survive the load (it is excluded in both views)
      const uint64_t h0 = sum();
      load();
      if (sum() != h0) std::fprintf(rep, "  ERROR: the snapshot load changed the dispatch table\n");
    }
    std::fprintf(rep, "  reference run\n");
    std::fflush(rep);
    if (!run(false)) { std::fprintf(rep, "%s: no function %08X\n", path.c_str(), fn); continue; }
    {
      uint64_t o = 0;
      for (size_t ri = 0; ri < regs.size(); ++ri) {
        if (!skip[ri])
          for (auto [po, pn] : Parts(regs[ri])) std::memcpy(after.data() + o + po, base + regs[ri].addr + po, pn);
        o += regs[ri].size;
      }
    }
    // a second reference run: words it leaves differently are nondeterministic (host kernel
    // objects, handles the first run created...) and are excluded from the comparison
    std::vector<uint8_t> after2(total);
    std::fprintf(rep, "  reference run 2\n");
    std::fflush(rep);
    run(false);
    {
      uint64_t o2 = 0;
      for (size_t ri = 0; ri < regs.size(); ++ri) {
        if (!skip[ri])
          for (auto [po, pn] : Parts(regs[ri])) std::memcpy(after2.data() + o2 + po, base + regs[ri].addr + po, pn);
        o2 += regs[ri].size;
      }
    }
    std::fprintf(rep, "  %s run\n", selftest ? "third reference" : "decomp");
    std::fflush(rep);
    run(!selftest);
    uint64_t o = 0, ndiff = 0, nondet = 0;
    int shown = 0;
    for (size_t ri = 0; ri < regs.size(); ++ri) {
      auto& r = regs[ri];
      if (skip[ri]) { o += r.size; continue; }
      const uint8_t* a = after.data() + o;
      const uint8_t* b = base + r.addr;
      for (uint32_t i = 0; i < r.size; i += 4) {
        if (InTable(r.addr + uint64_t(i))) continue;
        // dead stack below the entry r1: callee frames sit at different addresses in the
        // hand-written code (nothing reads that memory after the call returns)
        if (r.addr + uint64_t(i) < snap_ctx.r1.u32 && r.addr + uint64_t(i) >= snap_ctx.r1.u32 - 0x100000u) continue;
        uint32_t wa, wb, wn;
        std::memcpy(&wa, a + i, 4);
        std::memcpy(&wb, b + i, 4);
        std::memcpy(&wn, after2.data() + o + i, 4);
        if (wa != wn) {
          ++nondet;
          continue;
        }
        if (wa != wb) {
          if (shown++ < 40)
            std::fprintf(rep, "   %08X: off %08X  on %08X\n", r.addr + i, _byteswap_ulong(wa), _byteswap_ulong(wb));
          ++ndiff;
        }
      }
      o += r.size;
    }
    // how much the reference run changed (sanity: the function did something)
    uint64_t changed = 0;
    o = 0;
    for (auto& r : regs) {
      for (uint32_t i = 0; i < r.size; i += 4)
        changed += !InTable(r.addr + uint64_t(i)) &&
                   std::memcmp(image.data() + o + i, after.data() + o + i, 4) != 0;
      o += r.size;
    }
    std::fprintf(rep, "%s: fn %08X, %.1f MB, reference changed %llu words (%llu nondeterministic, excluded), %llu words differ%s\n",
                 path.c_str(), fn, double(total) / 1048576.0, (unsigned long long)changed, (unsigned long long)nondet,
                 (unsigned long long)ndiff, ndiff ? "" : "  EXACT");
    std::fflush(rep);
    bad_total += ndiff != 0;
    if (!selftest) {   // timing: the same function from the same memory, recompiled vs decomp (5 alternating pairs)
      std::vector<double> off, on;
      for (int k = 0; k < 5; ++k) {
        run(false); off.push_back(last_us);
        run(true); on.push_back(last_us);
      }
      std::sort(off.begin(), off.end());
      std::sort(on.begin(), on.end());
      std::fprintf(rep, "  timing (median of 5): recompiled %.1f us, decomp %.1f us (%+.1f%%)\n", off[2], on[2],
                   100.0 * (on[2] - off[2]) / off[2]);
    }
    sk83_render_game::ReportCoverage(rep);
  }
  std::fprintf(rep, "done: %d of %zu snapshots differ\n", bad_total, files.size());
  std::fclose(rep);
  TerminateProcess(GetCurrentProcess(), bad_total ? 1 : 0);
}

}  // namespace sk83_replay
#endif  // _WIN32
