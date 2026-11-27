// Traffic (living-world cars), logged: is the road network (RAINDATA) loaded, how many roads the
// RAIN manager holds, is vehicle spawning on, the freeskate traffic option. Lines are logged only
// when a value changes. The originals run unchanged.

#include "generated/skate3_init.h"

#include <rex/logging.h>

#include <atomic>
#include <cstdint>

namespace {
std::atomic<int> g_rain_assets{0};
std::atomic<int64_t> g_roads{-1}, g_supers{-1}, g_spawn{-1}, g_setting{-1};

uint32_t U32(uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
}  // namespace

// cProcessArenaAsset::ProcessRAINData(cAssetList*, const tTOCEntry&, void*): one per RAIN asset
extern "C" REX_FUNC(rex_cProcessArenaAsset_ProcessRAINData_CAXPAVcAssetList_ABUtTOCEntry_pegasus_PAX_Z_82C9B3A0) {
  const uint32_t e = ctx.r4.u32;
  const int n = ++g_rain_assets;
  REXLOG_INFO("[traffic] RAIN asset #{} processed: toc id {:08X}{:08X} obj {:08X}", n, U32(base, e + 8),
              U32(base, e + 12), U32(base, e + 20));
  __imp__rex_cProcessArenaAsset_ProcessRAINData_CAXPAVcAssetList_ABUtTOCEntry_pegasus_PAX_Z_82C9B3A0(ctx, base);
}

extern "C" REX_FUNC(rex_Sk8_LivingWorld_RAINMan_GetNumRoads_UAAIXZ_8269B2E0) {
  __imp__rex_Sk8_LivingWorld_RAINMan_GetNumRoads_UAAIXZ_8269B2E0(ctx, base);
  const int64_t v = int32_t(ctx.r3.u32);
  if (g_roads.exchange(v) != v) REXLOG_INFO("[traffic] RAINMan roads = {}", v);
}

extern "C" REX_FUNC(rex_Sk8_LivingWorld_RAINMan_GetNumSuperRoads_UAAIXZ_8269B2E8) {
  __imp__rex_Sk8_LivingWorld_RAINMan_GetNumSuperRoads_UAAIXZ_8269B2E8(ctx, base);
  const int64_t v = int32_t(ctx.r3.u32);
  if (g_supers.exchange(v) != v) REXLOG_INFO("[traffic] RAINMan super roads = {}", v);
}

extern "C" REX_FUNC(rex_Sk8_LivingWorld_CensusMan_EnableVehicleSpawn_UAAX_N_Z_8269B160) {
  const int64_t v = ctx.r4.u32 & 0xFF;
  if (g_spawn.exchange(v) != v)
    REXLOG_INFO("[traffic] CensusMan vehicle spawn {} (lr {:08X})", v ? "ENABLED" : "disabled", uint32_t(ctx.lr));
  __imp__rex_Sk8_LivingWorld_CensusMan_EnableVehicleSpawn_UAAX_N_Z_8269B160(ctx, base);
}

extern "C" REX_FUNC(rex_Sk8_FE_FreeSkateOptionCmdTranslator_sMethod_GetTrafficSetting_SAPAVAptValue_PAV4_H_Z_825AD680) {
  __imp__rex_Sk8_FE_FreeSkateOptionCmdTranslator_sMethod_GetTrafficSetting_SAPAVAptValue_PAV4_H_Z_825AD680(ctx, base);
  const int64_t v = ctx.r3.u32;
  if (g_setting.exchange(v) != v) REXLOG_INFO("[traffic] freeskate traffic setting query -> {:08X}", uint32_t(v));
}
