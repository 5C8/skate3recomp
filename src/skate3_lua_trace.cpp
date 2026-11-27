// Lua script errors, logged before the game handles them.
//
// The game's Lua (5.1, 32-bit, 4-byte numbers) raises a runtime error by formatting a message
// (luaG_typeerror -> luaG_runerror -> luaO_pushvfstring -> luaS_newlstr -> luaM_realloc). In
// some states the allocator pointer is null and that allocation is a null indirect call, so
// the error itself never shows. This logs what failed: the operation ("index", "call" ...),
// the bad value's type, the running Lua function (source, line, the instruction and its
// constant key) with its string constants, and the guest call chain (which game system ran
// the script). Then the original runs unchanged.
//
// Offsets (lua 5.1 x360): lua_State ci +20, savedpc +24; CallInfo func +4; Closure isC +6,
// Proto* +16; Proto k +8, code +12, lineinfo +20, source +32, sizek +40, sizecode +44,
// linedefined +60; TValue {value, tt} 8 bytes; TString len +12, chars +16.

#include "generated/skate3_init.h"

#include <rex/logging.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

bool Ok(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000u; }

uint32_t U32(uint8_t* base, uint32_t a) {
  if (!Ok(a) || (a & 3)) return 0;
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint8_t U8(uint8_t* base, uint32_t a) { return Ok(a) ? base[a] : 0; }

std::string CStr(uint8_t* base, uint32_t a, size_t max = 80) {
  std::string s;
  for (size_t i = 0; Ok(a) && i < max; ++i) {
    char c = static_cast<char>(base[a + i]);
    if (!c) break;
    s += (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}

std::string LuaStr(uint8_t* base, uint32_t ts) {
  if (!Ok(ts)) return "<?>";
  uint32_t len = U32(base, ts + 12);
  return CStr(base, ts + 16, len < 120 ? len : 120);
}

const char* TypeName(uint32_t tt) {
  static const char* n[] = {"nil", "boolean", "lightuserdata", "number", "string",
                            "table", "function", "userdata", "thread"};
  return tt < 9 ? n[tt] : "?";
}

std::string Constant(uint8_t* base, uint32_t k, uint32_t i) {
  uint32_t tv = k + i * 8;
  uint32_t tt = U32(base, tv + 4);
  if (tt == 4) return "\"" + LuaStr(base, U32(base, tv)) + "\"";
  if (tt == 3) {
    uint32_t bits = U32(base, tv);
    float f;
    memcpy(&f, &bits, 4);
    return std::to_string(f);
  }
  return TypeName(tt);
}

}  // namespace

extern "C" REX_FUNC(rex_luaG_typeerror_82915B80) {
  const uint32_t L = ctx.r3.u32, o = ctx.r4.u32, op = ctx.r5.u32;
  REXLOG_CRITICAL("[lua-error] attempt to {} a {} value (L={:08X} lr={:08X})", CStr(base, op),
                  TypeName(U32(base, o + 4)), L, uint32_t(ctx.lr));
  const uint32_t ci = U32(base, L + 20);
  const uint32_t func = U32(base, ci + 4);
  const uint32_t cl = U32(base, func);
  if (U32(base, func + 4) == 6 && Ok(cl) && U8(base, cl + 6) == 0) {
    const uint32_t p = U32(base, cl + 16);
    const uint32_t k = U32(base, p + 8), code = U32(base, p + 12), lines = U32(base, p + 20);
    const uint32_t sizek = U32(base, p + 40), sizecode = U32(base, p + 44);
    const uint32_t savedpc = U32(base, L + 24);
    const int32_t pc = (Ok(savedpc) && savedpc > code) ? int32_t((savedpc - code) / 4) - 1 : -1;
    std::string ins = "?";
    if (pc >= 0 && uint32_t(pc) < sizecode) {
      const uint32_t i = U32(base, code + pc * 4);
      const uint32_t opc = i & 0x3F, a = (i >> 6) & 0xFF, c = (i >> 14) & 0x1FF, b = (i >> 23) & 0x1FF;
      ins = "op=" + std::to_string(opc) + " A=" + std::to_string(a) + " B=" + std::to_string(b) +
            " C=" + std::to_string(c);
      if (c >= 256 && c - 256 < sizek) ins += " key=" + Constant(base, k, c - 256);
      if (b >= 256 && b - 256 < sizek) ins += " B=" + Constant(base, k, b - 256);
    }
    const uint32_t line = (pc >= 0 && Ok(lines)) ? U32(base, lines + pc * 4) : 0;
    REXLOG_CRITICAL("[lua-error] in {} line {} (defined at {}) pc {} {}", LuaStr(base, U32(base, p + 32)), line,
                    U32(base, p + 60), pc, ins);
    std::string ks;
    for (uint32_t i = 0; i < sizek && i < 40; ++i) ks += " " + Constant(base, k, i);
    REXLOG_CRITICAL("[lua-error] constants ({}):{}", sizek, ks);
  } else {
    REXLOG_CRITICAL("[lua-error] running function is not a Lua closure (func tt {})", U32(base, func + 4));
  }
  std::string chain;
  uint32_t sp = ctx.r1.u32;
  for (int n = 0; n < 32; ++n) {
    const uint32_t prev = U32(base, sp);
    if (!Ok(prev) || prev <= sp || prev - sp > 0x100000) break;
    chain += " " + fmt::format("{:08X}", U32(base, prev - 8));
    sp = prev;
  }
  REXLOG_CRITICAL("[lua-error] guest stack:{}", chain);
  __imp__rex_luaG_typeerror_82915B80(ctx, base);
}
