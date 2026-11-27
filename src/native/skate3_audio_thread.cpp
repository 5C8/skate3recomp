// The render loop's AudioSystem::Update on its own guest thread (cvar sk83_audio_thread).
//
// RETAIL PORT (from sk83_na_m src/native/sk83_audio_thread.cpp). Retail render loop body =
// sub_826D7E20 (dev sub_8283B3B0): it calls the audio system singleton ([0x830CFDC4]) through
// vtable +148 with f1 = dt, r5 = byte [renderman+337] (dev +1121), return address 0x826D8188
// (dev 0x8283B7F4), then sub_827FF1D0 (dev sub_829AD058). Same interface as dev
// AudioSystem::Update (r3 this, f1, r5); retail AudioSystem::Update = sub_82485190 (found by
// reading [[0x830CFDC4]]+148 at runtime; its call tree has the same kinds of shared access as
// dev: its own EventSys pipe flush, the camera matrix read, mutexes). The text below describes
// the dev build.
//
// sub_8283B3B0 (the render loop body) runs Presentation::Update (which starts the living-world
// job), scene culling, AudioSystem::Update (~250 us in busy Downtown) and then sub_829AD058,
// whose first step waits for the living-world job. Audio and the job ran side by side and took
// about the same time, so neither could get faster on its own. Here the audio update is handed
// to a worker and overlaps the rest of the frame; the render thread joins it when
// sub_829AD058 returns, so it still finishes inside the same loop pass (before the next
// RestoreFrame / EventSys flush / Presentation::Update) as before.
//
// The worker is a real guest thread (XHostThread: own KTHREAD, PCR, TLS, stack): critical
// sections and EA recursive mutexes see a different owner, never the render thread's. Its
// fake CPU is 4, which no game thread uses (they run on 0,1,2,3,5); the audio update never
// touches the job scheduler's per-processor queues or the animation codec.
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <immintrin.h>
#include <immintrin.h>

REXCVAR_DEFINE_BOOL(sk83_audio_thread, true, "Skate 3",
                    "Run the render loop's AudioSystem::Update on its own guest thread, "
                    "overlapping the frame's rendering (joined at the end of the frame).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);


namespace {

constexpr uint32_t kAudioUpdate = 0x82485190u;  // retail AudioSystem::Update (dev 0x826068B8)
constexpr uint32_t kRenderLoopReturn = 0x826D8188u;  // lr of the call in sub_826D7E20

struct Request {
  PPCRegister r[8];  // r3..r10
  PPCRegister f[8];  // f1..f8
  PPCFPSCRRegister fpscr;
  uint32_t msr;
};

Request g_req;
uint8_t* g_base = nullptr;
std::atomic<uint32_t> g_seq{0};   // requests posted (render thread)
std::atomic<uint32_t> g_done{0};  // requests finished (worker)
std::atomic<int> g_state{0};      // 0 none, 1 starting, 2 running, 3 failed
bool g_pending = false;           // render thread only
rex::system::object_ref<rex::system::XHostThread> g_thread;

// stats (render thread), logged every ~10 s
uint64_t g_n = 0, g_join_us = 0, g_join_waits = 0;
std::atomic<uint64_t> g_work_us{0};
std::chrono::steady_clock::time_point g_last_log = std::chrono::steady_clock::now();

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int WorkerMain() {
  auto* self = rex::system::XThread::GetCurrentThread();
  PPCContext& c = *self->thread_state()->context();
  g_state.store(2, std::memory_order_release);
  REXLOG_INFO("sk83_audio_thread: worker running (guest thread {:08X}, r1 {:08X}, r13 {:08X})",
              self->thread_id(), c.r1.u32, c.r13.u32);
  uint32_t seen = 0;
  for (;;) {
    // No spinning: one request per frame, and a spinning worker slows the render thread
    // (it can share a physical core with it).
    uint32_t s = g_seq.load(std::memory_order_acquire);
    while (s == seen) {
      g_seq.wait(seen, std::memory_order_acquire);
      s = g_seq.load(std::memory_order_acquire);
    }
    const uint64_t r1 = c.r1.u64, r13 = c.r13.u64;
    PPCRegister* const gr[8] = {&c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10};
    PPCRegister* const fr[8] = {&c.f1, &c.f2, &c.f3, &c.f4, &c.f5, &c.f6, &c.f7, &c.f8};
    for (int i = 0; i < 8; ++i) {
      *gr[i] = g_req.r[i];
      *fr[i] = g_req.f[i];
    }
    c.fpscr = g_req.fpscr;
    c.msr = g_req.msr;
    c.lr = kRenderLoopReturn;
    const int64_t t0 = NowUs();
    __imp__sub_82485190(c, g_base);
    g_work_us.fetch_add(uint64_t(NowUs() - t0), std::memory_order_relaxed);
    c.r1.u64 = r1;
    c.r13.u64 = r13;
    seen = s;
    g_done.store(s, std::memory_order_release);
    g_done.notify_one();
  }
}

bool EnsureWorker(uint8_t* base) {
  int st = g_state.load(std::memory_order_acquire);
  if (st == 2) return true;
  if (st != 0) return false;  // starting (fall back this frame) or failed
  g_base = base;
  g_state.store(1, std::memory_order_release);
  auto* ks = rex::system::kernel_state();
  // creation flags: processor mask in the top byte -> fake CPU 4
  g_thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(ks, 256 * 1024, 0x10000000u, [] { return WorkerMain(); }));
  g_thread->set_name("sk83 Audio Update");
  if (g_thread->Create() != 0) {
    REXLOG_ERROR("sk83_audio_thread: worker thread creation failed; audio stays on the render thread");
    g_state.store(3, std::memory_order_release);
    return false;
  }
  if (g_thread->thread()) {
    g_thread->thread()->set_priority(rex::thread::ThreadPriority::kAboveNormal);
  }
  return false;  // this frame runs inline; the worker takes over once it is up
}

void Join() {
  if (!g_pending) return;
  const uint32_t want = g_seq.load(std::memory_order_relaxed);
  uint32_t d = g_done.load(std::memory_order_acquire);
  if (d != want) {
    const int64_t t0 = NowUs();
    for (int spin = 0; d != want && spin < 20000; ++spin) {
      _mm_pause();
      d = g_done.load(std::memory_order_acquire);
    }
    while (d != want) {
      g_done.wait(d, std::memory_order_acquire);
      d = g_done.load(std::memory_order_acquire);
    }
    g_join_us += uint64_t(NowUs() - t0);
    ++g_join_waits;
  }
  g_pending = false;
  ++g_n;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_log > std::chrono::seconds(10)) {
    g_last_log = now;
    REXLOG_INFO("sk83_audio_thread: {} updates offloaded, worker avg {:.0f} us, render-thread "
                "join waited in {} ({:.1f} us avg over all)",
                g_n, double(g_work_us.exchange(0)) / double(g_n), g_join_waits,
                double(g_join_us) / double(g_n));
    g_n = g_join_us = g_join_waits = 0;
  }
}

}  // namespace

namespace sk83_replay { void CapturePoint(uint32_t fn, PPCContext& ctx, uint8_t* base); bool Replaying(); }
extern "C" REX_FUNC(sub_82485190) {
  sk83_replay::CapturePoint(0x82485190, ctx, base);
  Join();  // never expected: an earlier offloaded update still running
  // (never offloaded while --sk83_replay runs: the other threads, the worker too, are stopped)
  if (ctx.lr == kRenderLoopReturn && REXCVAR_GET(sk83_audio_thread) && !sk83_replay::Replaying() && EnsureWorker(base)) {
    PPCRegister* const gr[8] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    PPCRegister* const fr[8] = {&ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8};
    for (int i = 0; i < 8; ++i) {
      g_req.r[i] = *gr[i];
      g_req.f[i] = *fr[i];
    }
    g_req.fpscr = ctx.fpscr;
    g_req.msr = ctx.msr;
    g_pending = true;
    g_seq.fetch_add(1, std::memory_order_release);
    g_seq.notify_one();
    return;
  }
  __imp__sub_82485190(ctx, base);
}

// The rest of the render loop pass (LW wait, crowd sync, rendering, present): the offloaded
// audio update overlaps it and is joined right after.
extern "C" REX_FUNC(sub_827FF1D0) {
  __imp__sub_827FF1D0(ctx, base);
  Join();
}
