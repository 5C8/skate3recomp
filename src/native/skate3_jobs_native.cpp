// EA::Jobs / job-scheduling natives for retail (ported from sk83_na_m src/native/sk83_lw_jobs.cpp).
//
// EA::Jobs::LocalBackend::RunSingleJob(JobAffinity), retail 0x8290AFA0 (dev 0x82ABA558).
// The job pick of every job thread and every waiter (AnimJobScheduler::WaitForJob ->
// JobScheduler::JoinLocalQueueForOneJob, vtable +96). It scans the whole 64-bit slot array per
// call; recompiled, that scan was the dominant EA::Jobs cost and sits between every pair of
// dependent animation jobs on the living-world job's critical path.
//
// Retail vs dev listing: the scan / claim / release code is instruction-identical (only the stack
// temp slots 80/88 swap); retail LTCG INLINED JobInstance::Run (dev 0x82ABA3B8) into it:
//   PlayEventList(rec, 0) [sub_8290B460]; rec[40] != 0 -> DbgBreakPoint [sub_82EE1EC8];
//   QueryPerformanceCounter(r1+80); entry = [rec+44]: entry([rec+56],[rec+60],[rec+64],[rec+68]);
//   [rec+56] = (u64)GetThreadId(), [rec+64] = start; QueryPerformanceCounter(r1+80);
//   [rec+72] = end; PlayEventList(rec, 1).
// so the native version does that sequence inline, with retail's own callees and return
// addresses, inside a guest frame of the original's size (1264: QPC writes r1+80).
//   slots: u64 array at [be+8], count [be+16]; JobInstance records [be+4] + i*656.
//   slot word hi: state = hi >> 29 (0 ready, 1 running, 3 done, ...), exclusion mask
//   (hi >> 23) & 0x3F; a slot is eligible for affinity a when a & ~mask.
//   scan: found = any slot ready && eligible; best = argmin over ALL slots of
//         (word | notaff), notaff = ((~a << 23) & 0x1F800000) << 32 (unsigned 64-bit).
//   none -> return 0. Re-check best (ready + eligible, else rescan), CAS 0 -> state 1 (fail ->
//   rescan), run (above), then rec[41] != 0: Increment(rec+624); CAS running -> done (state 3);
//   rec[41] != 0 -> Semaphore::Post(rec+628, 1); Decrement(rec+624); else CAS running ->
//   (debug byte [[be+1588]] ? done : ~0). Then the backend's vtable +28 (r3 = be); return 1.
// The debug trace path ([[be+1588]] set: snprintf of the job first) stays recompiled.
// 64-bit CASes are host compare-exchanges on guest memory, exactly how the recompiler lowers
// ldarx/stdcx., so this interoperates with the recompiled job code on other threads.
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

REXCVAR_DEFINE_BOOL(sk83_native_runsinglejob, true, "Skate 3",
                    "Native EA::Jobs LocalBackend::RunSingleJob (the job pick of every job thread "
                    "and job waiter).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_native_runsinglejob_verify, false, "Skate 3",
                    "DIAGNOSTIC: every 64th call, run the recompiled and the native RunSingleJob on "
                    "two private copies of the live slot array and compare.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_native_runsinglejob_selftest, false, "Skate 3",
                    "DIAGNOSTIC: the verify fixture runs the recompiled function on both copies.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
constexpr uint32_t kFrame = 1264;  // the original's stwu

inline bool Cas64(uint8_t* base, uint32_t a, uint64_t expect, uint64_t desired) {
  return __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(REX_RAW_ADDR(a)),
                                      __builtin_bswap64(expect), __builtin_bswap64(desired));
}
struct Pick {
  int32_t best;  // -1 none
  bool found;
};
inline Pick Scan(uint8_t* base, uint32_t be, uint32_t aff) {
  const uint64_t notaff = uint64_t((~aff << 23) & 0x1F800000u) << 32;
  const uint32_t n = REX_LOAD_U32(be + 16);
  const uint32_t arr = REX_LOAD_U32(be + 8);
  Pick p{-1, false};
  uint64_t bestv = ~0ull;
  const volatile uint64_t* w = reinterpret_cast<const volatile uint64_t*>(REX_RAW_ADDR(arr));
  for (int32_t i = 0; i < int32_t(n); ++i) {
    const uint64_t v = __builtin_bswap64(w[i]);
    const uint32_t hi = uint32_t(v >> 32);
    if ((aff & ~((hi >> 23) & 0x3Fu) & 0xFFu) != 0 && (hi & 0xE0000000u) == 0) p.found = true;
    const uint64_t o = v | notaff;
    if (o < bestv) {
      bestv = o;
      p.best = i;
    }
  }
  return p;
}

void NativeRunSingleJob(PPCContext& ctx, uint8_t* base, uint32_t be, uint32_t aff) {
  uint32_t slot = 0, rec = 0;
  uint64_t running = 0, done = 0;
  for (;;) {
    const Pick p = Scan(base, be, aff);
    if (!p.found || p.best < 0) {
      ctx.r3.u64 = 0;
      return;
    }
    slot = REX_LOAD_U32(be + 8) + uint32_t(p.best) * 8u;
    const uint64_t w = REX_LOAD_U64(slot);
    const uint32_t hi = uint32_t(w >> 32), lo = uint32_t(w);
    if ((hi & 0xE0000000u) != 0) continue;
    if ((aff & ~((hi >> 23) & 0x3Fu) & 0xFFu) == 0) continue;
    const uint32_t keep = hi & 0x1FFFFFFFu;
    const uint64_t expect = (uint64_t(keep) << 32) | lo;
    running = (uint64_t(keep | 0x20000000u) << 32) | lo;
    if (!Cas64(base, slot, expect, running)) continue;
    done = (uint64_t(keep | 0x60000000u) << 32) | lo;
    rec = REX_LOAD_U32(be + 4) + uint32_t(p.best) * 656u;
    break;
  }
  const uint64_t r1 = ctx.r1.u64;
  ctx.r1.u64 = uint32_t(r1 - kFrame);
  const uint32_t qpc = ctx.r1.u32 + 80;
  // inlined JobInstance::Run
  ctx.r3.u64 = rec;
  ctx.r4.u64 = 0;
  ctx.lr = 0x8290B19C;
  sub_8290B460(ctx, base);
  if (REX_LOAD_U8(rec + 40) != 0) {
    ctx.lr = 0x8290B1AC;
    sub_82EE1EC8(ctx, base);
  }
  ctx.r3.u64 = qpc;
  ctx.lr = 0x8290B1B4;
  rex_QueryPerformanceCounter_82EDE320(ctx, base);
  const uint32_t entry = REX_LOAD_U32(rec + 44);
  if (entry != 0) {
    ctx.r6.u64 = REX_LOAD_U32(rec + 68);
    ctx.r5.u64 = REX_LOAD_U32(rec + 64);
    ctx.r4.u64 = REX_LOAD_U32(rec + 60);
    ctx.r3.u64 = REX_LOAD_U32(rec + 56);
    ctx.ctr.u64 = entry;
    ctx.lr = 0x8290B1D8;
    REX_CALL_INDIRECT_FUNC(entry);
  }
  ctx.lr = 0x8290B1DC;
  rex_EA_Thread_GetThreadId_YAPAXXZ_82A3FD78(ctx, base);
  const uint64_t start = REX_LOAD_U64(qpc);
  REX_STORE_U64(rec + 56, uint64_t(ctx.r3.u32));
  REX_STORE_U64(rec + 64, start);
  ctx.r3.u64 = qpc;
  ctx.lr = 0x8290B1F4;
  rex_QueryPerformanceCounter_82EDE320(ctx, base);
  REX_STORE_U64(rec + 72, REX_LOAD_U64(qpc));
  ctx.r3.u64 = rec;
  ctx.r4.u64 = 1;
  ctx.lr = 0x8290B208;
  sub_8290B460(ctx, base);
  // release
  if (REX_LOAD_U8(rec + 41) != 0) {
    ctx.r3.u64 = rec + 624;
    ctx.lr = 0x8290B22C;
    rex_EA_Jobs_Detail_ReferenceCount_Increment_QAA_NXZ_82909648(ctx, base);
    Cas64(base, slot, running, done);
    if (REX_LOAD_U8(rec + 41) != 0) {
      ctx.r3.u64 = rec + 628;
      ctx.r4.u64 = 1;
      ctx.lr = 0x8290B280;
      rex_EA_Thread_Semaphore_Post_QAAHH_Z_82A40A60(ctx, base);
    }
    ctx.r3.u64 = rec + 624;
    ctx.lr = 0x8290B288;
    rex_EA_Jobs_Detail_ReferenceCount_Decrement_QAA_NXZ_829096B0(ctx, base);
  } else if (REX_LOAD_U8(REX_LOAD_U32(be + 1588)) != 0) {
    Cas64(base, slot, running, done);
  } else {
    Cas64(base, slot, running, ~0ull);
  }
  ctx.r3.u64 = be;
  const uint32_t fn = REX_LOAD_U32(REX_LOAD_U32(be + 0) + 28);
  ctx.ctr.u64 = fn;
  ctx.lr = 0x8290B314;
  REX_CALL_INDIRECT_FUNC(fn);
  ctx.r1.u64 = r1;
  ctx.r3.u64 = 1;
}

// Verify fixture: a private fake backend in guest memory. Every 64th call snapshots the live
// slot array ONCE into a host buffer (copying one guest copy from the other lets the compiler
// forward the copy to the live, changing array) and runs the recompiled function on one guest
// copy and the native one on the other, then compares return values and both arrays (equal
// arrays also mean the same slot was picked). Fake records are zero: no entry function runs,
// their event lists are empty. The fake vtable's +28 is EA::Thread::GetThreadId (harmless).
constexpr uint32_t kFakeMaxSlots = 4096;
struct Fake {
  uint32_t be = 0, arr = 0, recs = 0;
};
Fake g_fake;
std::mutex g_fake_mu;
std::atomic<uint64_t> g_fx_runs{0}, g_fx_bad{0}, g_fx_picks{0}, g_rsj_calls{0};

bool FakeInit(uint8_t* base) {
  if (g_fake.be) return true;
  auto* mem = rex::system::kernel_memory();
  const uint32_t be = mem->SystemHeapAlloc(0x1000, 0x80);
  const uint32_t arr = mem->SystemHeapAlloc(kFakeMaxSlots * 8 * 2, 0x80);
  const uint32_t recs = mem->SystemHeapAlloc(kFakeMaxSlots * 656, 0x80);
  if (!be || !arr || !recs) return false;
  std::memset(REX_RAW_ADDR(be), 0, 0x1000);
  std::memset(REX_RAW_ADDR(recs), 0, kFakeMaxSlots * 656);
  REX_STORE_U32(be + 0, be + 0x800);
  REX_STORE_U32(be + 0x800 + 28, 0x82A3FD78u);
  REX_STORE_U32(be + 4, recs);
  REX_STORE_U32(be + 1588, be + 0x7F0);
  g_fake = {be, arr, recs};
  return true;
}

void FixtureCheck(PPCContext& ctx, uint8_t* base, uint32_t be, uint32_t aff) {
  std::lock_guard<std::mutex> lock(g_fake_mu);
  if (!FakeInit(base)) return;
  const uint32_t n = REX_LOAD_U32(be + 16);
  if (n == 0 || n > kFakeMaxSlots) return;
  const uint32_t live = REX_LOAD_U32(be + 8);
  const uint32_t a0 = g_fake.arr, a1 = g_fake.arr + kFakeMaxSlots * 8;
  static thread_local std::vector<uint8_t> snap;
  snap.resize(n * 8);
  std::memcpy(snap.data(), REX_RAW_ADDR(live), n * 8);
  asm volatile("" ::: "memory");
  std::memcpy(REX_RAW_ADDR(a0), snap.data(), n * 8);
  std::memcpy(REX_RAW_ADDR(a1), snap.data(), n * 8);
  REX_STORE_U32(g_fake.be + 16, n);
  PPCContext g = ctx;
  REX_STORE_U32(g_fake.be + 8, a0);
  g.r3.u64 = g_fake.be;
  g.r4.u64 = aff;
  __imp__rex_EA_1Jobs_LocalBackend_RunSingleJob_UAA_NW4JobAffinity_23_Z_8290AFA0(g, base);
  PPCContext m = ctx;
  REX_STORE_U32(g_fake.be + 8, a1);
  if (REXCVAR_GET(sk83_native_runsinglejob_selftest)) {
    m.r3.u64 = g_fake.be;
    m.r4.u64 = aff;
    __imp__rex_EA_1Jobs_LocalBackend_RunSingleJob_UAA_NW4JobAffinity_23_Z_8290AFA0(m, base);
  } else {
    NativeRunSingleJob(m, base, g_fake.be, aff);
  }
  const bool same = g.r3.u32 == m.r3.u32 && g.r1.u64 == m.r1.u64 &&
                    std::memcmp(REX_RAW_ADDR(a0), REX_RAW_ADDR(a1), n * 8) == 0;
  const uint64_t c = g_fx_runs.fetch_add(1) + 1;
  if (g.r3.u32) g_fx_picks.fetch_add(1);
  if (!same && g_fx_bad.fetch_add(1) < 10) {
    uint32_t k = 0;
    while (k < n && REX_LOAD_U64(a0 + k * 8) == REX_LOAD_U64(a1 + k * 8)) ++k;
    REXLOG_WARN("runsinglejob fixture: DIFF n {} aff {:02X}: guest ret {} | native ret {} | r1 {:X}/{:X} | slot {} {:016X} vs {:016X}",
                n, aff, g.r3.u32, m.r3.u32, g.r1.u32, m.r1.u32, k, k < n ? REX_LOAD_U64(a0 + k * 8) : 0,
                k < n ? REX_LOAD_U64(a1 + k * 8) : 0);
  }
  if ((c & 0x3FFF) == 0)
    REXLOG_INFO("runsinglejob fixture: {} runs ({} picked a job), {} differ", c, g_fx_picks.load(),
                g_fx_bad.load());
}
}  // namespace

extern "C" REX_FUNC(rex_EA_1Jobs_LocalBackend_RunSingleJob_UAA_NW4JobAffinity_23_Z_8290AFA0) {
  const uint32_t be = ctx.r3.u32;
  const uint32_t aff = ctx.r4.u32 & 0xFFu;
  if (REXCVAR_GET(sk83_native_runsinglejob_verify) && be != g_fake.be &&
      (g_rsj_calls.fetch_add(1, std::memory_order_relaxed) & 63) == 0) {
    FixtureCheck(ctx, base, be, aff);
  }
  if (!REXCVAR_GET(sk83_native_runsinglejob) || REX_LOAD_U8(REX_LOAD_U32(be + 1588)) != 0) {
    __imp__rex_EA_1Jobs_LocalBackend_RunSingleJob_UAA_NW4JobAffinity_23_Z_8290AFA0(ctx, base);
    return;
  }
  NativeRunSingleJob(ctx, base, be, aff);
}

// ---- scene culling jobs: job threads only ---------------------------------------------------
// Retail builds the VectorizedCullingJob description (BufferedIterativeJobEntryPoint 0x82812A20)
// inline on the stack in three render-view functions (sub_827FA190, sub_827FB908,
// RenderView::Render 0x827FCA10) with affinity 63 at description +20, then calls
// EntryPoint::SetName (0x82908D38) on it. Affinity 63 lets ANY joining thread run a culling batch
// -- including an animation waiter (AnimJobScheduler::WaitForJob joins with affinity 4), which then
// runs culling for up to several hundred us inside the living-world job (sk83 measurement; sk83
// used affinity 4 for the same jobs). With affinity 8 only the job threads (affinity 0x3F) take
// them. Applied at SetName, before the description is copied into the job; only these three
// call sites (by return address), only when the field still holds the original 63.
REXCVAR_DEFINE_BOOL(sk83_cull_jobs_job_threads_only, true, "Skate 3",
                    "Scene culling jobs run on the job threads only, never inside an animation "
                    "wait (keeps them off the living-world job's critical path).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" REX_FUNC(rex_EA_Jobs_EntryPoint_SetName_QAAXPBD_Z_82908D38) {
  if ((ctx.lr == 0x827FA380u || ctx.lr == 0x827FBA74u || ctx.lr == 0x827FCAE4u) &&
      REXCVAR_GET(sk83_cull_jobs_job_threads_only) && REX_LOAD_U32(ctx.r3.u32 + 20) == 63u) {
    REX_STORE_U32(ctx.r3.u32 + 20, 8u);
  }
  __imp__rex_EA_Jobs_EntryPoint_SetName_QAAXPBD_Z_82908D38(ctx, base);
}

