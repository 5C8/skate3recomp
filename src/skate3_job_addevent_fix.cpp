// EA::Jobs lost-release race, fixed at its source (port of sk83_na_m's skate3_job_watch.cpp fix).
//
// The job garbage collector (LocalBackend::TryToRunJobInstanceGarbageCollector, 0x8290A3B8) CASes a
// finished job's state word 3 ("done") -> 4 ("being collected"), keeping its id, then clears the job's
// event lists. LocalBackend::AddEvent (0x8290AAD0) treats only state 3 as finished. When it races the
// collector it sees state 4, appends the waiter's release event to the dying list, and the event is
// never played: the waiting job's enabler counter stays at 1 forever and whoever waits on it (e.g.
// AnimJobScheduler::WaitForJob -> RunSingleJob) spins for good. Seen live in retail 2026-11-24: job
// slot 435 in state 2 with counter 1 while every other slot was free.
//
// This is AddEvent step for step, except that states 3 and 4 both count as finished (then the event
// runs at once, as the original does for state 3). cvar skate3_job_addevent_fix (default on).

#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define SKATE3_SPIN_PAUSE() _mm_pause()
#else
#define SKATE3_SPIN_PAUSE() ((void)0)
#endif

REXCVAR_DEFINE_BOOL(skate3_job_addevent_fix, true, "Skate 3",
                    "Fix the EA::Jobs lost-release race (job-system freeze): LocalBackend::AddEvent "
                    "treats a dependency the garbage collector is clearing (state 4) as finished, "
                    "like state 3, instead of appending to its dying event list");

namespace {
std::atomic<uint64_t> g_saves{0};

uint32_t Load32(uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a));
}

uint32_t AtomicCas32(uint8_t* base, uint32_t addr, uint32_t expect, uint32_t desired) {
  auto* p = reinterpret_cast<uint32_t*>(base + addr);
  return __builtin_bswap32(__sync_val_compare_and_swap(p, __builtin_bswap32(expect), __builtin_bswap32(desired)));
}

// r3 = backend, r4 = dependency id (64-bit), r5 = its handle (slot = bits 16..31), r6 = Event*,
// r7 = When. Layout: backend +4 JobInstance records (656 B; list lock +644, played flags +648,
// end lists +80 + when*272), +8 u64 state words (top 3 bits state, id = (hi & 0x7FFF) << 32 | lo).
void AddEventFixed(PPCContext& ctx, uint8_t* base) {
  const uint32_t be = ctx.r3.u32;
  const uint64_t dep_id = ctx.r4.u64;
  const uint32_t slot = uint32_t(ctx.r5.u64 >> 16) & 0xFFFFu;
  const uint32_t ev = ctx.r6.u32;
  const uint32_t when = ctx.r7.u32;
  const uint32_t sw = Load32(base, be + 8) + slot * 8;
  const auto id_of = [&](uint32_t hi) { return (uint64_t(hi & 0x7FFFu) << 32) | Load32(base, sw + 4); };
  bool saved = false;  // state 4 seen where the original tested only 3
  const auto finished = [&](uint32_t hi) {
    const uint32_t st = hi >> 29;
    if (st == 4) saved = true;
    return st == 3 || st == 4;
  };
  const auto run_now = [&]() {
    ctx.r3.u64 = ev;
    rex_EA_Jobs_Event_Run_QBAXXZ_82908DF8(ctx, base);
  };
  const uint32_t rec_base = Load32(base, be + 4);
  const uint32_t rec = rec_base + slot * 656;
  uint32_t hi = Load32(base, sw);
  if (id_of(hi) != dep_id || rec == 0 || finished(hi)) {
    run_now();
  } else {
    for (;;) {
      if (id_of(hi) != dep_id || base[rec + 648 + when] != 0) {
        run_now();
        break;
      }
      if (AtomicCas32(base, rec + 644, 0, 1) == 0) {
        // Locked: re-check under the lock.
        hi = Load32(base, sw);
        if (finished(hi) || id_of(hi) != dep_id || base[rec + 648 + when] != 0) {
          run_now();
        } else {
          ctx.r3.u64 = rec + 80 + when * 272;
          ctx.r4.u64 = ev;
          rex_EA_Jobs_VEvent_BucketListNode_Add_0BA_Detail_Jobs_EA_QAAXABVEvent_34_Z_8290BD20(ctx, base);
        }
        AtomicCas32(base, rec + 644, 1, 0);  // AutoTryLockEventList's release
        break;
      }
      SKATE3_SPIN_PAUSE();  // db16cyc
      hi = Load32(base, sw);
      if (finished(hi)) {
        run_now();
        break;
      }
    }
  }
  if (saved) {
    const uint64_t n = ++g_saves;
    if ((n & (n - 1)) == 0) REXLOG_INFO("[jobs] AddEvent fix: {} lost releases prevented", n);
  }
}
}  // namespace

extern "C" REX_FUNC(rex_EA_1Jobs_LocalBackend_AddEvent_UAAXVJobInstanceHandle_23_ABVEvent_23_W4When_523_Z_8290AAD0) {
  if (REXCVAR_GET(skate3_job_addevent_fix)) {
    AddEventFixed(ctx, base);
  } else {
    __imp__rex_EA_1Jobs_LocalBackend_AddEvent_UAAXVJobInstanceHandle_23_ABVEvent_23_W4When_523_Z_8290AAD0(ctx, base);
  }
}
