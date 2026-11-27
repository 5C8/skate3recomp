#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, skate3_character_load_trace);

inline void Skate3TraceCharacterFallback(uint32_t caller, uint32_t request) {
  if (!REXCVAR_GET(skate3_character_load_trace)) {
    return;
  }
  const char* reason = "unknown";
  switch (caller) {
    case 0x827D8324:
      reason = "completion rejected or resource absent";
      break;
    case 0x827D8A30:
      reason = "request returned -1";
      break;
    case 0x827D8A54:
      reason = "custom request unavailable";
      break;
  }
  REXLOG_INFO("character-load: fallback caller={:08X} request={:08X} reason={}",
              caller, request, reason);
}

inline void Skate3TraceCharacterAllocation(uint32_t category, uint32_t heap, uint32_t bytes,
                                           uint32_t alignment, uint32_t caller, uint32_t result) {
  // Include failed requests from every category to catch companion pools. Only
  // log large successful CharMarquee allocations to keep normal traces small.
  if (result == 0 || (category == 22 && bytes >= 256 * 1024)) {
    REXLOG_INFO("character-load: alloc category={} heap={:08X} bytes={} alignment={} "
                "caller={:08X} result={:08X}",
                category, heap, bytes, alignment, caller, result);
  }
}
