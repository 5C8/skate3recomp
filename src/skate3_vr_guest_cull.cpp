// VR: the game's per-object frustum test culls against the headset's view.
//
// DoFrustumCulling (retail sub_82816A40; sk83_na_m reimplements it natively
// in skate3_frustum_cull.cpp, which retail does not build) reads the
// camera's six planes from cam + 5152 (r4 = camera). In VR the main
// camera's planes are replaced by a frustum covering both eyes before the
// guest body runs (skate3::vr::ApplyGuestCullPlanes), so turning the head
// finds geometry and nothing outside the view is submitted. The guest
// recomputes its planes when its camera updates, so nothing is restored.

#include "generated/skate3_init.h"

#include "skate3_vr.h"

namespace skate3::frustum_cull {
// Native DoFrustumCulling (skate3_frustum_cull_retail.cpp); false = run the
// guest body.
bool Run(PPCContext& ctx, uint8_t* base);
}  // namespace skate3::frustum_cull

extern "C" REX_FUNC(sub_82816A40) {
  skate3::vr::ApplyGuestCullPlanes(base, ctx.r4.u32);
  if (skate3::frustum_cull::Run(ctx, base)) {
    return;
  }
  __imp__sub_82816A40(ctx, base);
}
