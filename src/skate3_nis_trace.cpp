// NIS (cutscene) ground snap, logged.
//
// A cutscene played with a location override is snapped to the ground: the scene manager posts
// cMsgNISWorldCollisionQuery {fLocation @+0x10}, the world answers with
// cMsgNISWorldCollisionResponse {fPosition @+0x10, fNormal @+0x20}, and the scene manager makes
// the response's normal the scene's up (ReceiveNISMgrWorldCollisionResponseMsg re-orthogonalises
// its orientation around it - Skate 2 PDB/decomp). A downward or garbage normal plays the scene
// upside down. This logs the query location, who sent it, and the answer. The originals run
// unchanged.

#include "generated/skate3_init.h"

#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <rex/memory/utils.h>
#endif

#include <cstdint>
#include <cstring>
#include <string>

namespace {

bool Ok(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000u; }

// a stack back-chain can step into the unmapped gap between guest stacks: only read committed pages
bool Readable(const void* p) {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi;
  return VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
         (mbi.Protect & 0xff) != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
#else
  size_t length = 4;
  rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
  return rex::memory::QueryProtect(const_cast<void*>(p), length, access) &&
         access != rex::memory::PageAccess::kNoAccess;
#endif
}

uint32_t U32(uint8_t* base, uint32_t a) {
  if (!Ok(a) || (a & 3) || !Readable(base + a)) return 0;
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

float F32(uint8_t* base, uint32_t a) {
  const uint32_t b = U32(base, a);
  float f;
  memcpy(&f, &b, 4);
  return f;
}

std::string Vec(uint8_t* base, uint32_t a) {
  return fmt::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", F32(base, a), F32(base, a + 4), F32(base, a + 8),
                     F32(base, a + 12));
}

std::string Stack(uint8_t* base, uint32_t sp) {
  std::string chain;
  for (int n = 0; n < 24; ++n) {
    const uint32_t prev = U32(base, sp);
    if (!Ok(prev) || prev <= sp || prev - sp > 0x100000) break;
    chain += " " + fmt::format("{:08X}", U32(base, prev - 8));
    sp = prev;
  }
  return chain;
}

}  // namespace

extern "C" REX_FUNC(rex_cMsgNISWorldCollisionQuery_Deliver_SAXPBUStaticData_Hermes_N_Z_823890A0) {
  REXLOG_INFO("[nis-snap] query location {} lr={:08X} stack:{}", Vec(base, ctx.r3.u32 + 0x10),
              uint32_t(ctx.lr), Stack(base, ctx.r1.u32));
  __imp__rex_cMsgNISWorldCollisionQuery_Deliver_SAXPBUStaticData_Hermes_N_Z_823890A0(ctx, base);
}

extern "C" REX_FUNC(rex_cMsgNISWorldCollisionResponse_Deliver_SAXPBUStaticData_Hermes_N_Z_82389128) {
  REXLOG_INFO("[nis-snap] response posted: position {} normal {} lr={:08X} stack:{}",
              Vec(base, ctx.r3.u32 + 0x10), Vec(base, ctx.r3.u32 + 0x20), uint32_t(ctx.lr),
              Stack(base, ctx.r1.u32));
  __imp__rex_cMsgNISWorldCollisionResponse_Deliver_SAXPBUStaticData_Hermes_N_Z_82389128(ctx, base);
}

// The world side: sub_8285FF20 handles the query (r4 = message, fLocation @+0x10) and answers
// with sub_828600C0(location, &position, &normal) - a ray from location + 1 m straight down
// 500 m; a miss answers position 0, normal (0, -1, 0).
extern "C" REX_FUNC(sub_8285FF20) {
  REXLOG_INFO("[nis-snap] world got query: location {}", Vec(base, ctx.r4.u32 + 0x10));
  __imp__sub_8285FF20(ctx, base);
}

extern "C" REX_FUNC(sub_828600C0) {
  const uint32_t loc = ctx.r3.u32, pos = ctx.r4.u32, nrm = ctx.r5.u32;
  const std::string at = Vec(base, loc);
  __imp__sub_828600C0(ctx, base);
  REXLOG_INFO("[nis-snap] ground probe at {} -> position {} normal {}", at, Vec(base, pos), Vec(base, nrm));
}

extern "C" REX_FUNC(
    rex_User_ICESceneManager_ReceiveNISMgrWorldCollisionResponseMsg_QAAXABVcMsgNISWorldCollisionResponse_Z_82674490) {
  REXLOG_INFO("[nis-snap] scene manager {:08X} received: position {} normal {}", ctx.r3.u32,
              Vec(base, ctx.r4.u32 + 0x10), Vec(base, ctx.r4.u32 + 0x20));
  __imp__rex_User_ICESceneManager_ReceiveNISMgrWorldCollisionResponseMsg_QAAXABVcMsgNISWorldCollisionResponse_Z_82674490(
      ctx, base);
}

// Player teleports around a challenge start (cMsgTeleport {matrix rows @+0x10..+0x4F, flags
// @+0x50/+0x51}; the teleport manager entry points), with their guest callers.
extern "C" REX_FUNC(rex_cMsgTeleport_Deliver_SAXPBUStaticData_Hermes_N_Z_8238E7A8) {
  const uint32_t m = ctx.r3.u32;
  REXLOG_INFO("[nis-snap] teleport msg rows {} {} {} pos {} flags {:02X} {:02X} stack:{}", Vec(base, m + 0x10),
              Vec(base, m + 0x20), Vec(base, m + 0x30), Vec(base, m + 0x40), U32(base, m + 0x50) >> 24, (U32(base, m + 0x50) >> 16) & 0xFF,
              Stack(base, ctx.r1.u32));
  __imp__rex_cMsgTeleport_Deliver_SAXPBUStaticData_Hermes_N_Z_8238E7A8(ctx, base);
}

#define NIS_TRACE_CALL(fn, label)                                                                       \
  extern "C" REX_FUNC(fn) {                                                                             \
    REXLOG_INFO("[nis-snap] " label " r4={:08X} r5={:08X} r6={:08X} *r4 {} +40 {} stack:{}", ctx.r4.u32, \
                ctx.r5.u32, ctx.r6.u32, Vec(base, ctx.r4.u32), Vec(base, ctx.r4.u32 + 0x40),              \
                Stack(base, ctx.r1.u32));                                                               \
    __imp__##fn(ctx, base);                                                                             \
  }

NIS_TRACE_CALL(rex_Sk8_cTeleportManager_TeleportPlayer_UBAXHIH_N0_Z_82864330, "TeleportPlayer")
NIS_TRACE_CALL(rex_Sk8_cTeleportManager_TeleportToLastBreadcrumb_UBAXH_Z_82864470, "TeleportToLastBreadcrumb")
NIS_TRACE_CALL(rex_Sk8_cTeleportManager_TeleportLocalPlayer_UBAXABUtLocationID_Challenge_2_N1_Z_82864AE8,
               "TeleportLocalPlayer")
NIS_TRACE_CALL(rex_Sk8_cTeleportManager_TeleportLocalToLastBreadcrumb_UBAXXZ_82864B90, "TeleportLocalToLastBreadcrumb")
NIS_TRACE_CALL(rex_Sk8_Challenge_cChallengeTeleportManager_TeleportPlayerToLastBreadcrumb_QAAXH_Z_82822D60,
               "ChallengeTeleportPlayerToLastBreadcrumb")
NIS_TRACE_CALL(rex_Sk8_SimController_ReceiveTeleportMsg_AAAXABVcMsgTeleport_Z_8285C660, "SimController teleport")
NIS_TRACE_CALL(rex_cMsgSkaterTeleportComplete_Deliver_SAXPBUStaticData_Hermes_N_Z_8238DE78, "SkaterTeleportComplete")
NIS_TRACE_CALL(rex_cMsgTeleportTransitionFinished_Deliver_SAXPBUStaticData_Hermes_N_Z_8238E9D8,
               "TeleportTransitionFinished")
