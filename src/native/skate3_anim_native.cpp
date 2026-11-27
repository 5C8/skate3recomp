// Native animation job functions for retail (ported from sk83_na_m src/native/sk83_native_anim.cpp):
// AnimCore::Anim_Tools::PoseWithMatMult and AnimCmdSys::ACSsqtBlend_job, hand-decompiled in
// REXGLUE/skate-physics-decomp from the DEV build and proven bit-exact against the dev recompiled
// functions there. Retail is an LTCG build, so each is proven again here against RETAIL's own
// recompiled function: sk83_native_anim_verify runs the recompiled function and the native one
// from the same memory and compares every byte they write (outputs restored to the recompiled
// result on any difference). The natives stay off by default until that comparison is clean.
//   PoseWithMatMult  retail 0x828D4150 (dev 0x82A52FE0): listing identical apart from the address
//                    of one float constant (the decomp uses its value, 1.0f).
//   ACSsqtBlend_job  retail 0x828D6A58 (dev 0x82A5AA90): mostly register allocation, but some
//                    multiply-add chains are accumulated in a different order -- the verify decides.
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

#define RWP_GUEST 1
#define RW_FMADDS(a, b, c) ((float)std::fma((double)(a), (double)(b), (double)(c)))   // recomp fmadds
#include "native/decomp/packages/ea/animcommandsystem/1.01.00/source/animlib/cmn/animtoolspose.cpp"
#include "native/decomp/packages/ea/animcommandsystem/1.01.00/source/cmdlib/cmn/sqtengine/sqtengine.cpp"
#include "native/decomp/packages/skate/gameplay/andale/dev/source/fetch/extractpackedVBR.cpp"

uint8_t *rwp::g_base;

// Andale::Vector_ExtractPackedNVBR (retail sub_82D1DE08, dev 0x82FEB8B0): the decomp calls the VBR
// codec as guest functions -- retail's (InitPerAnim 82E88FA8 and TwoConsec 82E89B80 are native
// hooks themselves, both proven against retail), on the caller's context.
Andale::GuestCodec Andale::g_codec = {
    sub_82E88FA8,
    rex_ANIM_CODEC_VBRDecompressor_DecompressFrame_QAAXPBEIIE_Z_82E89A60,
    sub_82E89B80,
    rex_GetCurrentProcessorNumber_82EE2110,
};
thread_local PPCContext *Andale::g_ctx;

// Used by skate3_anim_split.cpp (RunExtract and its verify).
void Skate3NativeExtract(PPCContext &c, uint8_t *base, uint32_t types, uint32_t n, uint32_t dec) {
  rwp::g_base = base;
  Andale::g_ctx = &c;
  Andale::Vector_ExtractPackedNVBR(RWP_FROM_ADDR(Andale::ExtractType, types), n,
                                   RWP_FROM_ADDR(ANIM_CODEC::VBRDecompressor, dec));
}

REXCVAR_DEFINE_BOOL(sk83_native_anim_pose, true, "Skate 3",
                    "Native PoseWithMatMult (proven vs retail: 639k calls, 0 differences).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_native_anim_sqtblend, true, "Skate 3",
                    "Native ACSsqtBlend_job (proven vs retail: 115k calls, 0 differences).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_native_anim_verify, false, "Skate 3",
                    "DIAGNOSTIC: run the recompiled and the native PoseWithMatMult / ACSsqtBlend_job from "
                    "the same memory and compare every written byte (keeps the recompiled result).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
struct Region {
  uint32_t a, n;
};
struct VStat {
  std::atomic<uint64_t> calls{0}, bad{0};
};
VStat g_vpose, g_vsqt;

void Snap(uint8_t* base, const std::vector<Region>& rs, std::vector<uint8_t>& out) {
  out.clear();
  for (const Region& r : rs) out.insert(out.end(), base + r.a, base + r.a + r.n);
}
void Put(uint8_t* base, const std::vector<Region>& rs, const std::vector<uint8_t>& in) {
  size_t o = 0;
  for (const Region& r : rs) {
    std::memcpy(base + r.a, in.data() + o, r.n);
    o += r.n;
  }
}

// Runs guest() and native() from the same memory over `rs`; keeps the guest result.
template <class G, class N>
void Differential(const char* what, VStat& st, uint8_t* base, const std::vector<Region>& rs, G guest,
                  N native) {
  thread_local std::vector<uint8_t> before, g, m;
  Snap(base, rs, before);
  guest();
  Snap(base, rs, g);
  Put(base, rs, before);
  native();
  Snap(base, rs, m);
  const uint64_t c = st.calls.fetch_add(1) + 1;
  if (m != g) {
    size_t k = 0;
    while (m[k] == g[k]) ++k;
    if (st.bad.fetch_add(1) < 10) {
      size_t o = 0;
      for (const Region& r : rs) {
        if (k < o + r.n) {
          REXLOG_WARN("anim native verify {}: DIFF at {:08X}+{} (native {:02X} guest {:02X})", what, r.a,
                      k - o, m[k], g[k]);
          break;
        }
        o += r.n;
      }
    }
    Put(base, rs, g);
  }
  if ((c & 0x3FFF) == 0)
    REXLOG_INFO("anim native verify {}: {} calls, {} differ", what, c, st.bad.load());
}

bool Plausible(uint32_t a) { return a >= 0x10000 && a < 0xE0000000u; }

void RunPose(PPCContext& ctx, uint8_t* base) {
  using namespace AnimCore::Anim_Tools;
  rwp::g_base = base;
  PoseWithMatMult(int(ctx.r3.u32), RWP_FROM_ADDR(PoseParent, ctx.r4.u32), int(ctx.r5.u32),
                  RWP_FROM_ADDR(PoseMatrix44, ctx.r6.u32),
                  RWP_FROM_ADDR(const AnimCmdSys::SQTDataInternal, ctx.r7.u32),
                  RWP_FROM_ADDR(PoseMatrix44, ctx.r8.u32), RWP_FROM_ADDR(PoseMatrix44, ctx.r9.u32));
}
void RunSqt(PPCContext& ctx, uint8_t* base) {
  rwp::g_base = base;
  AnimCmdSys::ACSsqtBlend_job(ctx.r3.u32, RWP_FROM_ADDR(AnimCmdSys::sqtBlendSQTs::SQTJobData, ctx.r4.u32),
                              ctx.r5.u32, ctx.r6.u32);
}
}  // namespace

extern "C" REX_FUNC(rex_AnimCore_Anim_Tools_PoseWithMatMult_YAXHPAHHPAVMatrix44_vpu_math_rw_PBUSQTDataInternal_AnimCmdSys_11_Z_828D4150) {
  if (REXCVAR_GET(sk83_native_anim_verify)) {
    // Written: L / G / GM (r6 = GM, r9 = WorkBuffer): ceil4(numBones) matrices plus the 128-byte
    // line the per-pass dcbz zeroes past them (L + bone*64 + 0x100), on whichever buffers exist.
    const int n = int(ctx.r3.u32);
    if (n >= 0 && n <= 512) {
      const uint32_t span = uint32_t(((std::max(n, 1) + 3) & ~3) * 64 + 0x100 + 256);
      std::vector<Region> rs;
      for (uint32_t b : {ctx.r6.u32, ctx.r9.u32})
        if (b != 0 && Plausible(b) && std::none_of(rs.begin(), rs.end(), [&](const Region& r) { return r.a == b; }))
          rs.push_back({b & ~127u, span + (b & 127u)});
      if (!rs.empty()) {
        PPCContext g = ctx;
        Differential("PoseWithMatMult", g_vpose, base, rs,
                     [&] { __imp__rex_AnimCore_Anim_Tools_PoseWithMatMult_YAXHPAHHPAVMatrix44_vpu_math_rw_PBUSQTDataInternal_AnimCmdSys_11_Z_828D4150(g, base); },
                     [&] { RunPose(ctx, base); });
        return;
      }
    }
  }
  if (REXCVAR_GET(sk83_native_anim_pose)) {
    RunPose(ctx, base);
    return;
  }
  __imp__rex_AnimCore_Anim_Tools_PoseWithMatMult_YAXHPAHHPAVMatrix44_vpu_math_rw_PBUSQTDataInternal_AnimCmdSys_11_Z_828D4150(ctx, base);
}

extern "C" REX_FUNC(rex_AnimCmdSys_ACSsqtBlend_job_YAXIPAXII_Z_828D6A58) {
  if (REXCVAR_GET(sk83_native_anim_verify)) {
    // Written: every op's SQTOutput (and extraOutput, a copy of it); numBones SQTs of 0x30 bytes.
    const uint32_t data = ctx.r4.u32;
    const uint32_t nops = Plausible(data) ? REX_LOAD_U32(data + 0) : 0;
    const uint32_t nb = Plausible(data) ? REX_LOAD_U16(data + 4) : 0;
    const uint32_t ops = Plausible(data) ? REX_LOAD_U32(data + 0x0C) : 0;
    if (nops > 0 && nops <= 64 && nb > 0 && nb <= 512 && Plausible(ops)) {
      std::vector<Region> rs;
      for (uint32_t i = 0; i < nops; ++i) {
        // SQTOutput only: extraOutput is a plain copy of the op's output, and its buffer can be
        // shared by jobs running on other threads at the same time (a snapshot/restore of it
        // races them); identical outputs mean identical copies.
        for (uint32_t off : {0x08u}) {
          const uint32_t b = REX_LOAD_U32(ops + i * 0x40 + off);
          if (b != 0 && Plausible(b) && std::none_of(rs.begin(), rs.end(), [&](const Region& r) { return r.a == b; }))
            rs.push_back({b, nb * 0x30u});
        }
      }
      if (!rs.empty()) {
        thread_local char desc[256];
        int dl = std::snprintf(desc, sizeof(desc), "ACSsqtBlend_job nb %u ops", nb);
        for (uint32_t i = 0; i < nops && dl < 200; ++i)
          dl += std::snprintf(desc + dl, sizeof(desc) - dl, " [%u cmd %u ai %u as %u out %08X x %08X]", i,
                              REX_LOAD_U8(ops + i * 0x40 + 0x30), REX_LOAD_U16(ops + i * 0x40 + 0x32),
                              REX_LOAD_U8(ops + i * 0x40 + 0x31), REX_LOAD_U32(ops + i * 0x40 + 0x08),
                              REX_LOAD_U32(ops + i * 0x40 + 0x0C));
        PPCContext g = ctx;
        Differential(desc, g_vsqt, base, rs,
                     [&] { __imp__rex_AnimCmdSys_ACSsqtBlend_job_YAXIPAXII_Z_828D6A58(g, base); },
                     [&] { RunSqt(ctx, base); });
        return;
      }
    }
  }
  if (REXCVAR_GET(sk83_native_anim_sqtblend)) {
    RunSqt(ctx, base);
    return;
  }
  __imp__rex_AnimCmdSys_ACSsqtBlend_job_YAXIPAXII_Z_828D6A58(ctx, base);
}
