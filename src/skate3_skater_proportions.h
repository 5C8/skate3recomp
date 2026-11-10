#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace skate3::skater_proportions {

// Mods > Own skater proportions: before Sk8::UpdateBoneTransforms (retail sub_827A52C8) runs for a
// skater, points its matrices argument (r5) at a copy recomputed with the skater's own bone
// offsets (skate3_skater_proportions.cpp). No-op when the mod is off or for non-skaters.
void BeforeBoneTransforms(PPCContext& ctx, uint8_t* base);

// The SkaterPresEntity whose bones the next UpdateBoneTransforms on this thread places; returns
// the previous one. Set around each of the three retail callers: SkaterPresEntity::Update
// (sub_82781DE0, hooked here), SkaterPresEntity::StartJobs (0x827825B0) and the skater-family
// virtual UBT+Pack driver (sub_82785778) - the last two hooked in skate3_native_render.cpp.
uint32_t ExchangeEntity(uint32_t entity);

}  // namespace skate3::skater_proportions
