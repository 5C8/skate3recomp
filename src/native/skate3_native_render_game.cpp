// Decompiled render-thread game code for retail (port of sk83_na_m src/native/sk83_native_render_game.cpp).
// Each function is enabled only after it is proven against RETAIL's own function with the snapshot
// replay (skate3_guest_replay.cpp, --sk83_replay): retail is LTCG, so layouts, inlining and bodies may
// differ from the sk83 decomp. Hooks run natively when sk83_native_render_game is on (and their
// per-function proof holds); the replay toggles that cvar off/on and lists native calls per hook.
// Status per function: tools/sk83_port/PERF_PORT_2026-11.md (render_game section).
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <rex/memory/utils.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#define RWP_GUEST 1
#include "native/decomp/game/source/render/renderview.cpp"
#include "native/decomp/game/source/render/renderview_retail.cpp"
#include "native/decomp/game/source/render/renderman.cpp"
#include "native/decomp/game/source/render/PostProcessor.cpp"
#include "native/decomp/game/source/scene/sys/scenemanagerimpl.cpp"
#include "native/decomp/packages/ea/attribsys/attribhashmap.cpp"
#include "native/decomp/packages/ea/nfsmix/nfsmixmap.cpp"
#include "native/decomp/packages/ea/apt/aptrender.cpp"
#include "native/decomp/game/source/fe/apt/aptintegration.cpp"
#include "native/decomp/packages/ea/apt/aptcxform.cpp"
#include "native/decomp/game/source/fe/apt/aptrenderingintegration.cpp"
#include "native/decomp/packages/sk8/sk8aptint/source/aptanimation.cpp"
#include "native/decomp/game/source/render/simpledraw.cpp"
#include "native/decomp/game/source/fe/apt/aptrenderingunit.cpp"
#include "native/decomp/game/source/render/rpostfx/rwgpfxrendertarget.cpp"
#include "native/decomp/packages/ea/nfsmix/nfsmixshape.cpp"
#include "native/decomp/game/source/audio/sfxctl/sfxctl_3dobjpos.cpp"
#include "native/decomp/packages/ea/csiscore/source/cmn/csis.cpp"
#include "native/decomp/game/source/audio/audiostates.cpp"
#include "native/decomp/game/source/render/modelinstance.cpp"
#include "native/decomp/game/source/presentation/presentationentity.cpp"
#include "native/decomp/game/source/presentation/world/worldpresentity.cpp"
#include "native/decomp/packages/skate/rendering/sk8shader/dev/source/stateshadow.cpp"
#include "native/decomp/packages/skate/rendering/sk8shader/dev/source/materialman.cpp"
#include "native/decomp/xdk/runtime_mem.cpp"
#include "native/decomp/xdk/d3d/d3d_state.cpp"
#include "native/decomp/packages/ea/eastl/hashtable_find.cpp"
#include "native/decomp/packages/ea/eastl/hashtable_insert.cpp"
#include "native/decomp/packages/ea/eastl/fixed_pool.cpp"
#include "native/decomp/packages/ea/eastl/red_black_tree.cpp"
#include "native/decomp/packages/skate/junktofixup/sk8util/dev/include/util/kdtree.h"
#include "native/decomp/packages/sk8/rawdiscretize/dev/include/rawdiscretize/quatcompress.h"
#include "native/decomp/game/source/presentation/skater/skaterpresentation.cpp"
#include "native/decomp/game/source/render/font/font.cpp"
// shader binders (build/livesk8/.../sk83_binder_sources.inc) are build-specific (retail CTAB registers differ): not ported

REXCVAR_DEFINE_BOOL(sk83_native_render_game, true, "Skate 3",
                    "Run the decompiled render-thread game functions natively (each proven against "
                    "retail with --sk83_replay); off = the recompiled ones");
REXCVAR_DEFINE_STRING(sk83_native_render_game_skip, "", "Skate 3",
                      "Hooks (comma-separated name substrings) that stay recompiled even when proven; read at start");
REXCVAR_DEFINE_BOOL(sk83_native_render_game_poison, false, "Skate 3",
                    "DIAGNOSTIC (negative control for --sk83_replay): the native SetViewport flips one bit of "
                    "the device's viewport x afterwards; the replay must report differences");
REXCVAR_DEFINE_BOOL(sk83_native_render_game_unproven, false, "Skate 3",
                    "DIAGNOSTIC: also run the decompiled render-thread functions not yet proven against "
                    "retail (the --sk83_replay decomp run turns this on)");

namespace rwp {
thread_local PPCContext* g_ctx;

GuestFn ResolveGuest(u32 addr) {
  PPCFunc* f = nullptr;
  if (uint32_t(addr - REX_CODE_BASE) < REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) f = REX_LOOKUP_FUNC(g_base, addr);
  if (!f) f = rex::runtime::ResolveIndirectFunction(addr);
  return f;
}
}  // namespace rwp

namespace sk83_replay { void CapturePoint(uint32_t fn, PPCContext& ctx, uint8_t* base); }

namespace {
// native-call counters per hook: the replay report lists them (which functions a snapshot set covers)
struct HookCount {
  const char* name;
  uint64_t n;
  HookCount* next;
};
HookCount* g_hooks = nullptr;
// enters hand-written code on this guest context (nested entries keep the outer one)
struct Enter {
  PPCContext* saved;
  Enter(PPCContext& ctx, uint8_t* base) : saved(rwp::g_ctx) {
    // remap_decomp.py --unresolved maps addresses without a retail counterpart to 0x5FFF0000: it must
    // stay uncommitted, so anything reaching one faults instead of reading wrong data
    static const bool sentinel_ok = [base] {
      size_t length = 4;
      rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
      const bool ok = !rex::memory::QueryProtect(base + 0x5FFF0000u, length, access) ||
                      access == rex::memory::PageAccess::kNoAccess;
      if (!ok) REXLOG_ERROR("render_game: guest 0x5FFF0000 is committed -- the unresolved-address sentinel is unsafe");
      return ok;
    }();
    (void)sentinel_ok;
    rwp::g_base = base;
    rwp::g_ctx = &ctx;
  }
  ~Enter() { rwp::g_ctx = saved; }
};
// sk83_native_render_game_skip: comma-separated substrings of hook names that stay recompiled (read once)
bool Skipped(const char* name) {
  static const std::string list = REXCVAR_GET(sk83_native_render_game_skip);
  if (list.empty()) return false;
  const std::string n = name;
  for (size_t p = 0; p < list.size();) {
    size_t q = list.find(',', p);
    if (q == std::string::npos) q = list.size();
    if (q > p && n.find(list.substr(p, q - p)) != std::string::npos) return true;
    p = q + 1;
  }
  return false;
}
// proven = shown identical to retail's function by the replay (then it runs in normal play)
bool Native(bool proven, const char* name, signed char& skip) {
  if (skip < 0) skip = Skipped(name) ? 1 : 0;
  return !skip && REXCVAR_GET(sk83_native_render_game) && (proven || REXCVAR_GET(sk83_native_render_game_unproven));
}
struct HookReg {
  explicit HookReg(HookCount& h) {
    h.next = g_hooks;
    g_hooks = &h;
  }
};
}  // namespace

namespace sk83_render_game {
// for the replay report: native calls per decompiled function since the last call
void ReportCoverage(FILE* f) {
  for (HookCount* h = g_hooks; h; h = h->next) {
    std::fprintf(f, "    native %-90s %llu calls\n", h->name, (unsigned long long)h->n);
    h->n = 0;
  }
}
}  // namespace sk83_render_game

// --sk83_replay capture points (retail addresses; sk83 counterpart in the comment)
#define REPLAY_CAPTURE_HOOK(sym, addr)          \
  extern "C" REX_FUNC(sym) {                    \
    sk83_replay::CapturePoint(addr, ctx, base); \
    __imp__##sym(ctx, base);                    \
  }
REPLAY_CAPTURE_HOOK(sub_827FF7D0, 0x827FF7D0u)   // RenderMan::Render, the frame's render body (sk83 829ACA40)
REPLAY_CAPTURE_HOOK(sub_827FB158, 0x827FB158u)   // SceneRenderView::Render (sk83 829A9E00)
REPLAY_CAPTURE_HOOK(sub_8279E350, 0x8279E350u)   // Presentation::Update (sk83 8292ED90)
REPLAY_CAPTURE_HOOK(sub_8281E9B0, 0x8281E9B0u)   // SceneManagerImpl::UpdateFrameBegin (sk83 829CD290)
REPLAY_CAPTURE_HOOK(rex_NFSMixMap_Update3DMixCtls_QAAXXZ_82951658, 0x82951658u)   // (sk83 82B0EEF8)

// One hook per decompiled function: the native body when Native(proven), else the recompiled one.
#define RENDER_GAME_HOOK(sym, proven, body)       static HookCount hc_##sym{#sym, 0, nullptr};    static HookReg hr_##sym(hc_##sym);              extern "C" REX_FUNC(sym) {                        static signed char sk_ = -1;                                                                                                                                                                                                                                                                                                                   \
                        if (Native(proven, #sym, sk_)) {                             ++hc_##sym.n;                                   Enter e_(ctx, base);                            body;                                           return;                                       }                                               __imp__##sym(ctx, base);                      }
// as RENDER_GAME_HOOK, natively only when cond holds (the rest stays with the recompiled code)
#define RENDER_GAME_HOOK_IF(sym, proven, cond, body)   static HookCount hc_##sym{#sym, 0, nullptr};         static HookReg hr_##sym(hc_##sym);                   extern "C" REX_FUNC(sym) {                             static signed char sk_ = -1;                                                                                                                                                                                                                                                                                                                                                      \
                             if (Native(proven, #sym, sk_) && (cond)) {                        ++hc_##sym.n;                                        Enter e_(ctx, base);                                 body;                                                return;                                            }                                                    __imp__##sym(ctx, base);                           }
// XDK / CRT / rw memory copies (decomp/xdk/runtime_mem.cpp): native unless the ranges overlap;
// entry = the routine's own stack writes, made first
#define RENDER_GAME_COPY_HOOK(sym, proven, entry, D_, S_, N_)   static HookCount hc_##sym{#sym, 0, nullptr};                 static HookReg hr_##sym(hc_##sym);                           extern "C" REX_FUNC(sym) {                                     static signed char sk_ = -1;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        \
                                     if (Native(proven, #sym, sk_)) {                                          rwp::g_base = base;                                          entry;                                                       if (RwpRuntime::Copy(D_, S_, N_)) {                            ++hc_##sym.n;                                                return;                                                    }                                                          }                                                            __imp__##sym(ctx, base);                                   }
// for symbols another hook owns (the native renderer): it calls sk83_render_game::fn
#define RENDER_GAME_DISPATCH(fn, imp, proven, body)   static HookCount hc_##fn{#fn, 0, nullptr};          static HookReg hr_##fn(hc_##fn);                    namespace sk83_render_game {                        void fn(PPCContext& ctx, uint8_t* base) {             static signed char sk_ = -1;                                                                                                                                                                                                                                                                                                                                                                                                   \
             if (Native(proven, #fn, sk_)) {                                 ++hc_##fn.n;                                        Enter e_(ctx, base);                                body;                                               return;                                           }                                                   imp(ctx, base);                                   }                                                   }

// xdk/d3d/d3d_state.cpp: listings identical to sk83 apart from callee / 0.5f addresses
// (SetStreamSource differs in retail and stays recompiled). PROVEN 2026-11-18: replay of 3 retail
// render-body snapshots (827FF7D0), 0 words differ, ~8.4k native calls per snapshot; the poison
// control (one flipped bit) is reported in all 3.
RENDER_GAME_DISPATCH(SetTexture, __imp__rex_D3DDevice_SetTexture_82B82478, true,
                     D3D::SetTexture(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u64))
RENDER_GAME_DISPATCH(SetIndices, __imp__rex_D3DDevice_SetIndices_82B79190, true, D3D::SetIndices(ctx.r3.u32, ctx.r4.u32))
RENDER_GAME_DISPATCH(SetPendingRenderStates, __imp__rex_D3D_SetPending_RenderStates_YAXPAVCDevice_1_KKPAX_Z_82B83C48, true,
                     D3D::SetPending_RenderStates(ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32))
RENDER_GAME_HOOK(rex_D3D_SetPending_FetchConstants_YAXPAVCDevice_1_K_Z_82B83E88, true,
                 D3D::SetPending_FetchConstants(ctx.r3.u32, ctx.r4.u64))
RENDER_GAME_DISPATCH(SetVertexShader, __imp__rex_D3DDevice_SetVertexShader_82B7F408, true, D3D::SetVertexShader(ctx.r3.u32, ctx.r4.u32))
RENDER_GAME_DISPATCH(SetPixelShader, __imp__rex_D3DDevice_SetPixelShader_82B7F150, true, D3D::SetPixelShader(ctx.r3.u32, ctx.r4.u32))
RENDER_GAME_DISPATCH(SetViewport, __imp__rex_D3D_SetViewport_YAXPAUD3DDevice_MMMMMMK_Z_82B769C0, true,
                     (D3D::SetViewport(ctx.r3.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64, ctx.f5.f64, ctx.f6.f64),
                      REXCVAR_GET(sk83_native_render_game_poison)
                          ? (void)REX_STORE_U32(ctx.r3.u32 + 0x3218, REX_LOAD_U32(ctx.r3.u32 + 0x3218) ^ 1u)
                          : (void)0))

// Group 2 (2026-11-18): listings opcode-identical to sk83 (tools/sk83_port/pairmap.py), addresses remapped
// (tools/sk83_port/remap/render_game.json). Unproven until a clean --sk83_replay.
// sk83 82DAE9C0
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_COPY_HOOK(rex_XMemCpy_82EDF460, true, RwpRuntime::XMemCpyEntry(ctx), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)
// sk83 83190EC0
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_COPY_HOOK(rex_memcpy_82F52B30, true, RwpRuntime::MemcpyEntry(ctx), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)
// sk83 82CE6AB0
RENDER_GAME_COPY_HOOK(rex_rw_core_stdc_MemCopy_YAXPAXPBXI_Z_82AED480, false, (void)0, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)
// sk83 82B6DCB0
RENDER_GAME_HOOK_IF(
    rex_eastl_CBGH_GU_pair_hashtable_find_V_eastl_allocator_0DC_00_xcore_U_use_first_U_pair_CBGH_eastl_2_U_equal_to_G_2_U_hash_G_2_Umod_range_hashing_2_Udefault_ranged_hash_2_Upr_82ED1B60, false,
    REX_LOAD_U32(ctx.r4.u32 + 8) != 0, eastl::HashFindU16(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32))
// sk83 82990780
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK_IF(
    rex_Sk8_FE_W4eSk8FEScreenState_hashtable_find_U_pair_CBW4eSk8FEScreenState_FE_Sk8_H_eastl_V_eastl_allocator_0L_0A_xcore_U_use_first_U_pair_CBW4eSk8FEScreenState_FE_Sk8_H_east_8279C060, true,
    REX_LOAD_U32(ctx.r4.u32 + 8) != 0, eastl::HashFindS32(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32))
// sk83 82FF1910
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK_IF(rex_Sk8_HUD_HUDManager2_CBIPAUHUDComponentInfo_IU_pair_hashtable_find_eastl_V_fixed_hashtable_allocator_0CC_0M_0CA_03_0A_0A_V_eastl_allocator_0BJ_00_xcore_2_U_use_first_U_pai_826C4688, true,
                    REX_LOAD_U32(ctx.r4.u32 + 8) != 0, eastl::HashFindS32(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32))
// sk83 82D173B8
RENDER_GAME_HOOK_IF(rex_eastl_fixed_pool_base_init_QAAXPAXIIII_Z_82B0E878, false, ctx.r6.u32 >= 4u || ctx.r7.u32 <= 0xfffffffcu,
                    eastl::fixed_pool_base_init(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32))
// sk83 82D17430
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK(rex_eastl_RBTreeIncrement_YAPAUrbtree_node_base_1_PBU21_Z_82B0EBF0, true, ctx.r3.u64 = eastl::RBTreeIncrement(ctx.r3.u32))
// sk83 830B1E48
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK(rex_Sk8_FE_CBIPAUMapIcon_IU_pair_rbtree_DoNukeSubtree_eastl_U_less_I_2_V_fixed_node_pool_0BI_00_03_0A_0A_V_eastl_allocator_07_0P_xcore_2_U_use_first_U_pair_CBIPAUMapIcon_FE_S_82425B70, true,
                 Sk8::Render::KdResultSet_DoNukeSubtree(ctx.r3.u32, ctx.r4.u32))
// sk83 82D7CB18
RENDER_GAME_HOOK_IF(rex_Attrib_HashMap_FindIndex_QBAI_K_Z_82B6FBB0, false, REX_LOAD_U16(ctx.r3.u32 + 8) != 0,
                    ctx.r3.u64 = Attrib::HashMap_FindIndex(ctx.r3.u32, ctx.r4.u64))
// sk83 82D7CC00
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK_IF(rex_Attrib_HashMap_Find_QBAPAVNode_2_K_Z_82B6FC98, true, REX_LOAD_U16(ctx.r3.u32 + 8) != 0,
                    ctx.r3.u64 = Attrib::HashMap_Find(ctx.r3.u32, ctx.r4.u64))
// sk83 82D7B830
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK_IF(rex_Attrib_KVCollection_VecHashMap_FindIndex_VTablePolicy_Class_2_00_0GA_QBAI_K_Z_82B6BF90, true,
                    REX_LOAD_U16(ctx.r3.u32 + 4) != 0, ctx.r3.u64 = Attrib::VecHashMap_FindIndex(ctx.r3.u32, ctx.r4.u64))
// sk83 82A694B0
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK(rex_Sk8_Audio_CSTATEMGR_Base_UpdateParams_UAAXM_Z_828DEE58, true,
                 Sk8::Audio::CSTATEMGR_Base_UpdateParams(ctx.r3.u32, ctx.f1.f64))
// sk83 82A69530
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK(rex_Sk8_Audio_CSTATEMGR_Base_ProcessUpdate_UAAXXZ_828DEED8, true, Sk8::Audio::CSTATEMGR_Base_ProcessUpdate(ctx.r3.u32))
// sk83 82A69970
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK(rex_Sk8_Audio_CSTATE_Base_ProcessUpdate_UAAXXZ_828DF518, true, Sk8::Audio::CSTATE_Base_ProcessUpdate(ctx.r3.u32))
// sk83 82A699C0
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK(rex_Sk8_Audio_CSTATE_Base_UpdateParams_UAAXM_Z_828DF568, true,
                 Sk8::Audio::CSTATE_Base_UpdateParams(ctx.r3.u32, ctx.f1.f64))
// sk83 83164140
// PROVEN: replay 2026-11-18 (retail_renderbody + retail_frameparts, 11 snapshots, 0 words differ)
RENDER_GAME_HOOK(rex_AptRenderElement_Render_QBAXI_Z_82E852D0, true, AptRenderElement_Render(ctx.r3.u32, ctx.r4.u32))

// Group 3 (2026-11-18): opcode-identical, whole native closure identical (tools/sk83_port/native_closure.py).
// sk83 82980CF8
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; register-exact closure)
RENDER_GAME_HOOK(sub_828150E0, true, Sk8::Render::SortKeys(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32))
// sk83 829ADDD8
// VectorizedCullingJob (retail 82817138) NOT hooked: its job data layout differs (dev reads +16, retail +8)
// sk83 829068D0
RENDER_GAME_HOOK(sub_827894C0, false, Sk8::SkaterPresentation_ApplyRelativeSQTs(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32))
// sk83 8296F490
// retail (LTCG) drops the unused first argument: out r3, w r4, h r5 (dev: self r3, out r4, w r5, h r6)
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; register-exact closure)
RENDER_GAME_HOOK(sub_827EB1B0, true, Sk8::Render::PostFX_BoxTapOffsets(0u, ctx.r3.u32, ctx.r4.s32, ctx.r5.s32))
// sk83 82B0B998
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; register-exact closure)
RENDER_GAME_HOOK(rex_NFSMixShape_GetPitchMultFromCents_YAMH_Z_8294B4D8, true,
                 ctx.f1.f64 = NFSMixShape::GetPitchMultFromCents_((int)ctx.r3.u32))
// sk83 82D94248
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; register-exact closure)
RENDER_GAME_HOOK(rex_D3DDevice_SetScissorRect_82B78EF8, true, D3D::SetScissorRect(ctx.r3.u32, ctx.r4.u32))
// sk83 82D91C90
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; register-exact closure)
RENDER_GAME_HOOK(rex_D3D_SetSurfaceClip_YAXPAVCDevice_1_HHHH_Z_82B766D0, true,
                 D3D::SetSurfaceClip(ctx.r3.u32, (int)ctx.r4.u32, (int)ctx.r5.u32, (int)ctx.r6.u32, (int)ctx.r7.u32))
// skate3_apt_spread.cpp owns the hook (APT HUD spread) and calls this for the original body
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ)
RENDER_GAME_DISPATCH(AptLegacyTransform, __imp__rex_CallLegacyTransformCallbacks_82E85038, true,
                     ::CallLegacyTransformCallbacks(ctx.r3.u32))

// StateShadow::SetRasterizer/DepthStencil/Blend/RenderTargetState (sk83 82F63B88/BD0/C18/C60): retail ICF-folded
// all four into 82CFC990, with ONE shadow global (0x83085370) and one device setter (0x82A7B660); any of the
// four decomp setters is that function (register-exact closure identical).
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; retail-specific decomp)
RENDER_GAME_HOOK(sub_82CFC990, true, StateShadow::SetBlendState(ctx.r3.u32))

// RenderMesh: retail-specific decomp (renderview_retail.cpp, written from retail's listing)
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; retail-specific decomp)
RENDER_GAME_HOOK(sub_827FAEA8, true, Sk8::Render::Retail::RenderMesh(ctx.r3.u32, ctx.r4.u32))
// PROVEN: replay 2026-11-18 (11 snapshots, 0 words differ; ~1240 meshes per call)
RENDER_GAME_DISPATCH(RenderMeshes, __imp__sub_827FAF50, true,
                     Sk8::Render::Retail::RenderMeshes(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32))

// D3DDevice_SetStreamSource, retail 0x82B78FF0: LTCG dropped the always-zero offset argument
// (retail: dev r3, stream r4, vb r5, stride r6, dirty r7); otherwise sk83's body with offset 0.
// PROVEN: replay 2026-11-24 (11 snapshots, 0 words differ, 34k native calls)
RENDER_GAME_DISPATCH(SetStreamSource, __imp__rex_D3DDevice_SetStreamSource_82B78FF0, true,
                     D3D::SetStreamSource(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0u, ctx.r6.u32, ctx.r7.u64))
