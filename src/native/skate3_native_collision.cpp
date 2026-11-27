// Native collision function for retail (ported from sk83_na_m src/native/sk83_native_collision.cpp):
// rw::collision UnitGetOverlappingGPInstances -- decode one ClusteredMesh unit into 1-2 GPTriangle
// instances; the RipTriangles functors of the collision jobs call it per unit.
//
// Retail = sub_82778238 (dev 0x828FDB48). Same argument registers (r3..r9, f1) and the same
// caller stack slot for the flags (sp+0x56). Retail never reads the dev volumeTag argument (sp+0x5c)
// and stores 0 into mVolumeTag (+0x84); the triangle VolumeMethods table it copies is 0x82FD58A0
// (dev 0x832E2D20, patched in the decomp copy). No asserts in retail; the decomp's assert paths
// (return false) fall back to retail's function. sk83_native_collision_verify runs retail's
// function and the native one from the same memory and compares both instances and the result.
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#define RWP_GUEST 1
#include "native/decomp/game/source/spu/clusteredmesh/clusteredmeshspu.cpp"

// Proven 2026-11-17 against retail's function: 1.77M calls, instances + result identical, 0 fallbacks.
// 0.18 us/call native vs 0.29-0.37 recompiled.
REXCVAR_DEFINE_BOOL(sk83_native_collision, true, "Skate 3",
                    "Native UnitGetOverlappingGPInstances (sk83_native_collision_verify re-checks it).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_native_collision_verify, false, "Skate 3",
                    "DIAGNOSTIC: run retail's UnitGetOverlappingGPInstances and the native one from the "
                    "same memory and compare the instances and the result (keeps retail's).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
std::atomic<uint64_t> g_ns{0}, g_calls{0}, g_native{0}, g_v_calls{0}, g_v_bad{0}, g_v_fallback{0};
std::atomic<int64_t> g_next_log{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Account(int64_t t0, bool native) {
  const int64_t t1 = NowNs();
  g_ns.fetch_add(uint64_t(t1 - t0), std::memory_order_relaxed);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (native) g_native.fetch_add(1, std::memory_order_relaxed);
  int64_t due = g_next_log.load(std::memory_order_relaxed);
  if (due == 0) {
    g_next_log.compare_exchange_strong(due, t1 + 10'000'000'000LL);
    return;
  }
  if (t1 < due || !g_next_log.compare_exchange_strong(due, t1 + 10'000'000'000LL)) return;
  const uint64_t ns = g_ns.exchange(0), c = g_calls.exchange(0), n = g_native.exchange(0);
  if (c)
    REXLOG_INFO("collision: UnitGetOverlappingGPInstances {} | {:.3f} us/call, {} calls/s, {:.2f} ms/s",
                n == c ? "NATIVE" : n ? "mixed" : "recompiled", double(ns) / double(c) / 1000.0, c / 10,
                double(ns) / 1e7);
}

bool RunNative(PPCContext& ctx, uint8_t* base) {
  rwp::g_base = base;
  const uint32_t sp = ctx.r1.u32;  // the caller's parameter slot: flags (u16)
  const uint32_t flags = __builtin_bswap16(*(const uint16_t*)(base + sp + 0x56));
  uint32_t size = 0;
  if (rw::collision::UnitGetOverlappingGPInstances(&size, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                                   ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, float(ctx.f1.f64),
                                                   flags, /*volumeTag: retail stores 0*/ 0u)) {
    ctx.r3.u64 = size;
    return true;
  }
  return false;
}
}  // namespace

extern "C" REX_FUNC(sub_82778238) {
  const int64_t t0 = NowNs();
  if (REXCVAR_GET(sk83_native_collision_verify)) {
    // Output: up to two GPTriangle instances (0xC0 each) at r6, and the return value.
    const uint32_t inst = ctx.r6.u32;
    if (inst >= 0x10000 && inst < 0xE0000000u) {
      uint8_t before[0x180], g[0x180], m[0x180];
      std::memcpy(before, base + inst, sizeof(before));
      PPCContext gc = ctx;
      __imp__sub_82778238(gc, base);
      std::memcpy(g, base + inst, sizeof(g));
      std::memcpy(base + inst, before, sizeof(before));
      PPCContext mc = ctx;
      if (!RunNative(mc, base)) {
        g_v_fallback.fetch_add(1);
      } else {
        const uint64_t c = g_v_calls.fetch_add(1) + 1;
        std::memcpy(m, base + inst, sizeof(m));
        if (mc.r3.u32 != gc.r3.u32 || std::memcmp(m, g, sizeof(g)) != 0) {
          if (g_v_bad.fetch_add(1) < 10) {
            size_t k = 0;
            while (k < sizeof(g) && m[k] == g[k]) ++k;
            REXLOG_WARN("collision verify: DIFF result {} vs {}, first byte +0x{:X} (native {:02X} retail {:02X})",
                        mc.r3.u32, gc.r3.u32, k, k < sizeof(g) ? m[k] : 0, k < sizeof(g) ? g[k] : 0);
          }
        }
        if ((c & 0xFFFF) == 0)
          REXLOG_INFO("collision verify: {} calls, {} differ, {} fell back", c, g_v_bad.load(),
                      g_v_fallback.load());
      }
      std::memcpy(base + inst, g, sizeof(g));
      ctx = gc;
      Account(t0, false);
      return;
    }
  }
  if (REXCVAR_GET(sk83_native_collision) && RunNative(ctx, base)) {
    Account(t0, true);
    return;
  }
  __imp__sub_82778238(ctx, base);
  Account(t0, false);
}
