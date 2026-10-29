// Online play (EA Nation / Blaze) on the retail recomp.
//
// Ported from the sk83_na_m (milestone build) skate3_online.cpp. The milestone
// forced the game's Net config onto the insecure transport so it would reach the
// community Blaze server (retail marks the redirector connection secure/SSL, which
// needs an Xbox LIVE LSP address the recomp's plain-IP sockets do not have; a secure
// setup sits on "Connecting to EA Nation..." forever). The DNS side (net_host_overrides
// pointing gosredirector / skate3_web at the custom server) is provided by the rexglue
// fork's XAM layer and is shared with sk83_na_m - only this transport patch is per-build.
//
// The struct offsets differ between the milestone and retail builds, so this is a port
// of the LOGIC, not of the addresses. The two functions were located by cross-build
// analysis of the decrypted images (opcode-stream + string-table + the distinctive
// 'frrt' argv tag), matching the milestone pair:
//   milestone sub_828A0250  (Blaze hub param filler, secure flag)  -> retail rex_Sk8_Net_Core_EnablePresentation_QAAX_N_Z_8272C7E0
//   milestone InitDirtySock 0x828A03B0 (XNetStartup, argv 'frrt')  -> retail rex_Sk8_Net_PlasmaSystemPlatform_InitDirtySock_MAAXXZ_8272C8B8
//
// Retail rex_Sk8_Net_Core_EnablePresentation_QAAX_N_Z_8272C7E0 is the one that decides secure vs insecure. Its own logic:
//   r8 = *(config + 488);  *(params + 684) = r8;
//   if (r8 == 0) *(params + 680) = 0;   // 0 => insecure path (adds -nosecure)
// so zeroing the config word at +488 before the function runs makes the game's own
// code select the insecure setup - exactly parallel to how the milestone build zeroed
// its config's secure byte.

#include "generated/skate3_init.h"

#include <cstdint>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <rex/logging.h>

namespace {

// Config offset read by retail rex_Sk8_Net_Core_EnablePresentation_QAAX_N_Z_8272C7E0 to choose the secure transport.
// (Milestone equivalent lived at a different struct offset; see the header note.)
constexpr uint32_t kSecureConfigOffset = 488;

void ForceInsecureTransport(PPCContext& ctx, uint8_t* base) {
  const uint32_t config = ctx.r3.u32;
  if (config >= 0x40000000u && config < 0x90000000u) {
    REXLOG_INFO("[live-gate] force-insecure: config={:08X} secure(+488) was {:08X} -> 0", config,
                REX_LOAD_U32(config + kSecureConfigOffset));
    REX_STORE_U32(config + kSecureConfigOffset, 0);
  }
}

}  // namespace

// Retail Blaze hub / DirtySock secure-transport setup (milestone sub_828A0250).
// Forcing its config input insecure here is what lets the redirector connection
// succeed against the plain-IP community server.
extern "C" REX_FUNC(rex_Sk8_Net_Core_EnablePresentation_QAAX_N_Z_8272C7E0) {
  ForceInsecureTransport(ctx, base);
  __imp__rex_Sk8_Net_Core_EnablePresentation_QAAX_N_Z_8272C7E0(ctx, base);
}

// ---------------------------------------------------------------------------
// LIVE-logon / sign-in gate diagnostics + fixes.
//
// Retail shows "You need to be signed into Xbox LIVE to access Xbox LIVE game
// play features" although every rexglue sign-in export reports LIVE. The only
// unimplemented import the retail game hits is XNetLogonGetTitleID (x18 in the
// logs) - a bare REX_EXPORT_STUB that leaves r3 as garbage, so the game gets a
// bogus title ID from the LIVE-logon API. These overrides replace the XNetLogon
// stubs with real values and log the sign-in checks (same semantics as the
// fork's own implementations) so one run shows which gate the game trips.
// ---------------------------------------------------------------------------

#include <atomic>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

namespace {
std::atomic<int> g_logon_logs{0};
bool LogOnce(int limit = 40) { return g_logon_logs.fetch_add(1, std::memory_order_relaxed) < limit; }
}  // namespace

// DWORD XNetLogonGetTitleID(void) - the title ID the LIVE logon is bound to.
extern "C" void __imp__XNetLogonGetTitleID(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const uint32_t title = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->title_id() : 0;
  ctx.r3.u64 = title;
  if (LogOnce()) REXLOG_INFO("[live-gate] XNetLogonGetTitleID -> {:08X} (lr={:08X})", title, uint32_t(ctx.lr));
}

// HRESULT XNetLogonGetMachineID(ULONGLONG* id) - a stable per-machine ID.
extern "C" void __imp__XNetLogonGetMachineID(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32;
  uint64_t xuid = 0;
  if (REX_KERNEL_STATE() && REX_KERNEL_STATE()->user_profile()) xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  const uint64_t id = 0xFA00000000000000ULL | (xuid & 0x00FFFFFFFFFFFFFFULL);
  if (out) REX_STORE_U64(out, id);
  ctx.r3.u64 = 0;  // S_OK
  if (LogOnce()) REXLOG_INFO("[live-gate] XNetLogonGetMachineID -> {:016X} (lr={:08X})", id, uint32_t(ctx.lr));
}

namespace {
rex::system::xam::UserProfile* Profile() {
  return REX_KERNEL_STATE() ? REX_KERNEL_STATE()->user_profile() : nullptr;
}
}  // namespace

// Logging re-implementations of the fork's sign-in exports (identical results).
extern "C" void __imp__XamUserGetSigninState(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const uint32_t idx = ctx.r3.u32;
  uint32_t state = 0;
  if (idx == 0 && Profile()) state = Profile()->signin_state();
  ctx.r3.u64 = state;
  if (LogOnce()) REXLOG_INFO("[live-gate] XamUserGetSigninState({}) -> {} (lr={:08X})", idx, state, uint32_t(ctx.lr));
}

extern "C" void __imp__XamUserCheckPrivilege(PPCContext& ctx, uint8_t* base) {
  const uint32_t idx = ctx.r3.u32, mask = ctx.r4.u32, out = ctx.r5.u32;
  const uint32_t granted = (Profile() && Profile()->is_live_signed_in()) ? 1 : 0;
  uint32_t ret = 0;
  if (idx != 0xFF && idx >= 4) ret = 0x57;        // X_ERROR_INVALID_PARAMETER
  else if (idx != 0xFF && idx) ret = 0x525;       // X_ERROR_NO_SUCH_USER
  else if (out) REX_STORE_U32(out, granted);
  ctx.r3.u64 = ret;
  if (LogOnce()) REXLOG_INFO("[live-gate] XamUserCheckPrivilege(user={}, priv={:X}) -> ret={:X} granted={} (lr={:08X})",
                             idx, mask, ret, granted, uint32_t(ctx.lr));
}

// ---------------------------------------------------------------------------
// XLIVEBASE 0x00058009 = XContentGetMarketplaceCounts.
//
// Retail's online flow (game sub_82582840 -> XOnline sub_82F39038 -> rex_XContentGetMarketplaceCounts_82F38F68)
// asks LIVE for the marketplace offer counts before it ever resolves the EA
// redirector. The fork's XLiveBaseApp has no case for 0x58009, so it returned
// X_E_FAIL, rex_XContentGetMarketplaceCounts_82F38F68 turned that into ERROR_FUNCTION_FAILED and the game gave
// up with "The EA server is not available at this time" - no DNS lookup ever ran.
//
// Request (16 bytes): { dwUserIndex, dwTitleId, dwContentCategories, pResults }
// Result  ( 8 bytes): XOFFERING_CONTENTAVAILABLE_RESULT { dwNewOffers, dwTotalOffers }
// There is no marketplace, so report zero offers, successfully.
//
// XMsgStartIORequest is overridden here (like the XNetLogon / sign-in imports
// above) because the handler lives in the prebuilt rexruntime; every other message
// is forwarded exactly as the fork's xeXMsgStartIORequestEx does.
// ---------------------------------------------------------------------------

#include <rex/system/xam/app_manager.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace {
constexpr uint32_t kXLiveBaseApp = 0xFC;
constexpr uint32_t kMsgMarketplaceCounts = 0x00058009;
}  // namespace

// ---------------------------------------------------------------------------
// XStringVerify (retail sub_82F39B60) - LIVE's profanity check on typed text.
//
//   DWORD XStringVerify(DWORD dwFlags, const CHAR* szLocale, DWORD dwNumStrings,
//                       const STRING_DATA* pStringData, DWORD cbResults,
//                       STRING_VERIFY_RESPONSE* pResults, XOVERLAPPED* pOverlapped);
//   STRING_VERIFY_RESPONSE { WORD wNumStrings; HRESULT* pStringResult; }  (packed)
//
// Identified from its caller, the Team Name check sub_827585B8, which passes
// exactly that argument list (flags 0, locale, 1 string, STRING_DATA at r1+112
// with its pointer at +2 - packed, cbResults 10, pResults at r1+120, no
// overlapped), and from the request it marshals (XLIVEBASE 0x5000C, captured:
// title 454108E6, locale "en-US", the typed word). The fork's XLiveBaseApp has
// no case for 0x5000C, so it failed, pResults was never written, and the caller
// - which ignores the return value and reads pResults->pStringResult[0], 0 =
// passed - read stack garbage: every word "did not pass the profanity check".
// There is no LIVE string service to ask, so every string passes: the packed
// response with its S_OK array placed right after the header inside the
// caller's cbResults buffer, exactly as LIVE's reply would be unpacked.
// ---------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82F39B60) {
  using namespace rex;
  const uint32_t num_strings = ctx.r5.u32, cb_results = ctx.r7.u32;
  const uint32_t results = ctx.r8.u32, overlapped = ctx.r9.u32;
  constexpr uint32_t kHeader = 6;  // WORD wNumStrings + HRESULT* pStringResult
  uint32_t fit = 0;
  if (results && cb_results >= kHeader) {
    fit = std::min(num_strings, (cb_results - kHeader) / 4);
    REX_STORE_U16(results + 0, uint16_t(fit));
    REX_STORE_U32(results + 2, results + kHeader);
    for (uint32_t i = 0; i < fit; ++i) REX_STORE_U32(results + kHeader + i * 4, 0);  // S_OK
  }
  REXLOG_INFO("[live-gate] XStringVerify({} string(s), cb={}) -> all passed", num_strings, cb_results);
  uint32_t result = X_ERROR_SUCCESS;
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
    result = X_ERROR_IO_PENDING;
  }
  ctx.r3.u64 = result;
}

extern "C" void __imp__XMsgStartIORequest(PPCContext& ctx, uint8_t* base) {
  using namespace rex;  // xtypes.h result macros expand to rex:: types
  const uint32_t app = ctx.r3.u32, message = ctx.r4.u32;
  const uint32_t overlapped = ctx.r5.u32, buffer = ctx.r6.u32, length = ctx.r7.u32;
  auto* ks = REX_KERNEL_STATE();

  uint32_t result;
  if (app == kXLiveBaseApp && message == kMsgMarketplaceCounts && buffer && length >= 16) {
    const uint32_t results = REX_LOAD_U32(buffer + 12);
    if (results) {
      REX_STORE_U32(results + 0, 0);  // dwNewOffers
      REX_STORE_U32(results + 4, 0);  // dwTotalOffers
    }
    result = X_ERROR_SUCCESS;
    REXLOG_INFO("[live-gate] XContentGetMarketplaceCounts(user={}, title={:08X}, cats={:08X}) -> 0 offers",
                REX_LOAD_U32(buffer + 0), REX_LOAD_U32(buffer + 4), REX_LOAD_U32(buffer + 8));
  } else {
    result = ks->app_manager()->DispatchMessageAsync(app, message, buffer, length);
    if (result == X_E_NOTFOUND) {
      result = X_E_INVALIDARG;
      rex::system::XThread::SetLastError(X_ERROR_NOT_FOUND);
    }
  }
  REXLOG_INFO("[live-gate] XMsgStartIORequest(app={:X}, msg={:08X}, len={}) -> {:08X}", app, message,
              length, uint32_t(result));
  if (overlapped) {
    ks->CompleteOverlappedImmediate(overlapped, result);
    result = X_ERROR_IO_PENDING;
  }
  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    rex::system::XThread::SetLastError(0);
  }
  ctx.r3.u64 = result;
}

// ---------------------------------------------------------------------------
// TEMPORARY net-path tracing (remove once connected). Each wrapper logs its
// arguments and result and forwards to the real rexruntime export, so behaviour
// is unchanged - retail's final build has no debug prints to follow instead.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
using GuestFn = void (*)(PPCContext&, uint8_t*);
GuestFn Real(const char* name) {
  static HMODULE rt = GetModuleHandleA("rexruntime.dll");
  return rt ? reinterpret_cast<GuestFn>(GetProcAddress(rt, name)) : nullptr;
}
}  // namespace

#define TRACE_FORWARD(NAME)                                                                      extern "C" void __imp__##NAME(PPCContext& ctx, uint8_t* base) {                                  static GuestFn real = Real("__imp__" #NAME);                                                   static std::atomic<int> n{0};                                                                  const uint64_t a3 = ctx.r3.u64, a4 = ctx.r4.u64, a5 = ctx.r5.u64, a6 = ctx.r6.u64;             const uint32_t lr = uint32_t(ctx.lr);                                                          if (real) real(ctx, base);                                                                     if (n.fetch_add(1) < 30)                                                                         REXLOG_INFO("[net-trace] " #NAME "({:X}, {:X}, {:X}, {:X}) -> {:X} (lr={:08X})", a3, a4,                   a5, a6, ctx.r3.u64, lr);                                                        }

TRACE_FORWARD(NetDll_WSAStartup)
TRACE_FORWARD(NetDll_XNetStartup)
TRACE_FORWARD(NetDll_XNetGetEthernetLinkStatus)
TRACE_FORWARD(NetDll_XNetGetConnectStatus)
TRACE_FORWARD(NetDll_XNetDnsLookup)
TRACE_FORWARD(NetDll_XNetServerToInAddr)
TRACE_FORWARD(NetDll_XNetQosServiceLookup)
TRACE_FORWARD(NetDll_XNetConnect)
TRACE_FORWARD(NetDll_socket)
namespace {
bool ResolveOverride(const std::string& host, uint8_t out[4]);
}
// The Xbox 360 build addresses its web tier (downloads.skate.online.ea.com's role: the
// /skate3/ws SOAP services and the EAWebKit pages) by a fixed IPv4 literal baked into the
// game - it calls inet_addr("10.10.237.194") and connects, with no DNS lookup - so the
// hostname entries in net_host_overrides never apply to it (the PS3 build uses the
// hostname, which is why the same server works under RPCS3). net_host_overrides entries
// whose key is an IPv4 literal ("10.10.237.194=127.0.0.1") are therefore applied here, at
// connect time: the destination is swapped for the call and the guest's sockaddr restored.
extern "C" void __imp__NetDll_connect(PPCContext& ctx, uint8_t* base) {
  static GuestFn real = Real("__imp__NetDll_connect");
  const uint32_t sock = ctx.r4.u32, sa = ctx.r5.u32;
  uint16_t fam = 0, port = 0;
  uint8_t ip[4] = {}, mapped[4] = {};
  bool remapped = false;
  if (sa) {
    fam = REX_LOAD_U16(sa + 0);
    port = REX_LOAD_U16(sa + 2);
    for (int i = 0; i < 4; ++i) ip[i] = REX_LOAD_U8(sa + 4 + i);
    if (fam == 2) {
      const std::string literal = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
                                  std::to_string(ip[2]) + "." + std::to_string(ip[3]);
      if (ResolveOverride(literal, mapped)) {
        for (int i = 0; i < 4; ++i) REX_STORE_U8(sa + 4 + i, mapped[i]);
        remapped = true;
      }
    }
  }
  if (real) real(ctx, base);
  if (remapped)
    for (int i = 0; i < 4; ++i) REX_STORE_U8(sa + 4 + i, ip[i]);
  REXLOG_INFO("[net-trace] connect(sock={:X}, fam={}, {}.{}.{}.{}:{}{}) -> {:X}", sock, fam, ip[0], ip[1], ip[2], ip[3],
              port,
              remapped ? fmt::format(" => {}.{}.{}.{}", mapped[0], mapped[1], mapped[2], mapped[3]) : std::string(),
              ctx.r3.u64);
}
namespace {
std::string HexDump(uint8_t* base, uint32_t ea, uint32_t len, uint32_t cap = 96) {
  static const char* d = "0123456789ABCDEF";
  std::string out;
  for (uint32_t i = 0; i < len && i < cap; ++i) {
    const uint8_t b = REX_LOAD_U8(ea + i);
    out += d[b >> 4]; out += d[b & 15];
    if ((i & 3) == 3) out += ' ';
  }
  if (len > cap) out += "...";
  return out;
}
}  // namespace

// NetDll_send(caller, s, buf, len, flags) / NetDll_recv(caller, s, buf, len, flags)
extern "C" void __imp__NetDll_send(PPCContext& ctx, uint8_t* base) {
  static GuestFn real = Real("__imp__NetDll_send");
  const uint32_t sock = ctx.r4.u32, buf = ctx.r5.u32, len = ctx.r6.u32;
  const std::string hex = HexDump(base, buf, len);
  if (real) real(ctx, base);
  REXLOG_INFO("[net-trace] send(sock={:X}, len={:X}) -> {:X} | {}", sock, len, ctx.r3.u64, hex);
}
extern "C" void __imp__NetDll_recv(PPCContext& ctx, uint8_t* base) {
  static GuestFn real = Real("__imp__NetDll_recv");
  const uint32_t sock = ctx.r4.u32, buf = ctx.r5.u32, len = ctx.r6.u32;
  if (real) real(ctx, base);
  const int32_t got = int32_t(ctx.r3.u32);
  if (got > 0)
    REXLOG_INFO("[net-trace] recv(sock={:X}, len={:X}) -> {:X} | {}", sock, len, got, HexDump(base, buf, uint32_t(got)));
}
TRACE_FORWARD(NetDll_select)
TRACE_FORWARD(NetDll_getsockopt)
TRACE_FORWARD(NetDll_closesocket)
TRACE_FORWARD(NetDll_WSAGetLastError)
TRACE_FORWARD(NetDll_shutdown)
TRACE_FORWARD(NetDll_inet_addr)
// ---------------------------------------------------------------------------
// XLIVEBASE 0x00058007 = XOnlineGetServiceInfo(dwServiceId, pServiceInfo).
//
// Retail does not DNS-resolve the EA redirector: it asks LIVE for the address of
// EA's title service 0x45410805 ("EA" + service 0805) and connects there. The
// fork hard-wires this message to ERROR_CONNECTION_INVALID (0x80151802), which is
// exactly the "The EA server is not available at this time" the game shows (the
// net trace had it on every online attempt, and no DNS/connect ever followed).
// The milestone build is a dev build that skips this and uses DNS instead.
//
// Answer with the custom server: the IP net_host_overrides maps
// gosredirector.ea.com to (so there is nothing new to configure) and the Blaze
// redirector port 42100.
//   XONLINE_SERVICE_INFO { DWORD dwServiceID; IN_ADDR serviceIP; WORD wServicePort; WORD wReserved; }
// ---------------------------------------------------------------------------
#include <rex/cvar.h>
#include <ws2tcpip.h>
#include <string>

namespace {
constexpr uint32_t kMsgGetServiceInfo = 0x00058007;
constexpr uint16_t kBlazeRedirectorPort = 42100;

// Target of net_host_overrides for `host`, resolved to IPv4 (network order bytes).
bool ResolveOverride(const std::string& host, uint8_t out[4]) {
  std::string spec = rex::cvar::Query<std::string>("net_host_overrides");
  for (char& c : spec) if (c == ',') c = ';';
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t end = spec.find(';', pos);
    if (end == std::string::npos) end = spec.size();
    std::string pair = spec.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = pair.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string t) {
      const size_t a = t.find_first_not_of(" 	"), b = t.find_last_not_of(" 	");
      return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    std::string key = trim(pair.substr(0, eq)), target = trim(pair.substr(eq + 1));
    if (_stricmp(key.c_str(), host.c_str()) != 0 || target.empty()) continue;
    in_addr a{};
    if (inet_pton(AF_INET, target.c_str(), &a) == 1) { std::memcpy(out, &a, 4); return true; }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(target.c_str(), nullptr, &hints, &res) == 0 && res) {
      std::memcpy(out, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, 4);
      freeaddrinfo(res);
      return true;
    }
  }
  return false;
}
}  // namespace

extern "C" void __imp__XMsgInProcessCall(PPCContext& ctx, uint8_t* base) {
  static GuestFn real = Real("__imp__XMsgInProcessCall");
  const uint32_t app = ctx.r3.u32, message = ctx.r4.u32;
  const uint32_t arg1 = ctx.r5.u32, arg2 = ctx.r6.u32;

  if (app == 0xFC && message == kMsgGetServiceInfo && arg2) {
    const uint32_t service_id = arg1, info = arg2;
    uint8_t ip[4] = {127, 0, 0, 1};
    const bool mapped = ResolveOverride("gosredirector.ea.com", ip);
    REX_STORE_U32(info + 0, service_id);
    for (int i = 0; i < 4; ++i) REX_STORE_U8(info + 4 + i, ip[i]);  // IN_ADDR, network order
    REX_STORE_U16(info + 8, kBlazeRedirectorPort);
    REX_STORE_U16(info + 10, 0);
    ctx.r3.u64 = 0;  // X_E_SUCCESS
    REXLOG_INFO("[live-gate] XOnlineGetServiceInfo({:08X}) -> {}.{}.{}.{}:{}{}", service_id, ip[0], ip[1],
                ip[2], ip[3], kBlazeRedirectorPort, mapped ? "" : " (no gosredirector.ea.com override; default)");
    return;
  }

  if (real) real(ctx, base);
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 30)
    REXLOG_INFO("[net-trace] XMsgInProcessCall({:X}, {:X}, {:X}, {:X}) -> {:X}", app, message, arg1, arg2,
                ctx.r3.u64);
}

// ---------------------------------------------------------------------------
// Team search crash: stale interface pointer called through a garbage vtable.
//
// Reproduced under the VS debugger (Team search -> crash) with a full native
// call stack pointing at sub_8275A3A0 (skate3_recomp.32.cpp:63007):
//   lwz r3,1052(r31)   ; r3 = this->field_1052   (a cached interface pointer)
//   lwz r11,0(r3)      ; r11 = r3->vtable
//   lwz r10,156(r11)   ; r10 = vtable[39]
//   mtctr r10 ; bctrl  ; -> abort() inside InvalidFunctionTrap
//
// Same shape (this+offset holds an interface pointer, vtable slot 39 called)
// as the LoadArenaAsset crash diagnosed earlier this session on sk83_na_m
// (there at this+284), on an entirely different code path (asset streaming
// vs. Team search) - strong evidence this is a shared engine-level bug in how
// a cached interface pointer outlives the container-allocator scope it was
// handed out from, not something specific to either build or to the Blaze/
// Clubs gap fixed server-side. Root-causing that allocator lifetime issue is
// a much larger job; this is a narrow guest-side guard at the exact call site
// that crashed, so a stale pointer degrades (the Team search silently skips
// this piece of data) instead of aborting the whole game.
//
// Installed via [[midasm_hook]] in manifests/skate3.toml.in at 0x8275A410
// (the bctrl itself), jumping to 0x8275A414 (the instruction right after it)
// when the pointer chain doesn't look like live guest memory.
bool SkipStaleTeamInterfaceCall(PPCRegister& r31) {
  uint8_t* base = REX_KERNEL_STATE()->memory()->virtual_membase();

  auto looksLikeGuestPtr = [](uint32_t addr) {
    return addr >= 0x40000000u && addr < 0x90000000u;
  };

  const uint32_t iface = REX_LOAD_U32(r31.u32 + 1052);
  if (!looksLikeGuestPtr(iface))
    return true;  // skip: not a plausible interface pointer

  const uint32_t vtable = REX_LOAD_U32(iface);
  if (!looksLikeGuestPtr(vtable))
    return true;  // skip: vtable pointer is garbage

  const uint32_t slot39 = REX_LOAD_U32(vtable + 156);
  if (slot39 < 0x80000000u || slot39 >= 0x90000000u)
    return true;  // skip: method pointer is outside the executable's code range

  return false;  // chain looks sane; let the original call through
}

// Guard against a REX_CALL_INDIRECT_FUNC abort in sub_82EFCD48
// (skate3_recomp.110.cpp:35933), one step further up the same Team/Clubs
// data-fetch chain that sub_82F0E988's this-pointer guard above (and
// SkipStaleTeamInterfaceCall further up still) already patches. Reproduced
// under the VS debugger: at the crash, r30 (this+28, an embedded
// sub-object of the function's own valid "this") had a vtable pointer of
// exactly 0 - not garbage, a genuinely zero-initialized/never-constructed
// sub-object - so `lwz r11,20(r11)` reads near guest address 0 and the
// resulting garbage function pointer isn't in the registered dispatch
// table, tripping InvalidFunctionTrap/REX_FATAL/abort.
//
// Unlike sub_82F0E988's guard, this can't be checked at function entry:
// r30 is a fixed this+28 offset available from entry, but what's stored at
// that address depends on what the function's own earlier internal call
// (its first bctrl, a few instructions after entry) does - entry-time and
// crash-time state aren't guaranteed to match, so this has to be a
// mid-function check at the actual call site, same as
// SkipStaleTeamInterfaceCall.
//
// No mutation needed: by the time this call is reached, r31 already holds
// the (already HRESULT-style) return status from the sub_82F0EA68 call
// earlier in this same function, and the function's own ending is just
// `r3 = r31; return`. Skipping straight to that return point via
// jump_address_on_true therefore preserves whatever status was already
// computed, instead of running the doomed call.
//
// Installed via [[midasm_hook]] in manifests/skate3.toml.in at 0x82EFCDF0
// (the `lwz r11,0(r30)` that starts the doomed dereference chain), jumping
// to 0x82EFCE04 (the instruction right after the bctrl) when the embedded
// sub-object's vtable pointer doesn't look like live guest memory.
namespace {
bool HasGarbageVtable(uint8_t* base, uint32_t objectPtr) {
  auto looksLikeGuestPtr = [](uint32_t addr) {
    return addr >= 0x40000000u && addr < 0x90000000u;
  };

  const uint32_t vtable = REX_LOAD_U32(objectPtr);
  return !looksLikeGuestPtr(vtable);
}
}  // namespace

bool GuardNullClubSubObjectVtable(PPCRegister& r30) {
  uint8_t* base = REX_KERNEL_STATE()->memory()->virtual_membase();
  return HasGarbageVtable(base, r30.u32);  // skip if the vtable pointer is garbage/null
}

// Same crash shape as GuardNullClubSubObjectVtable above (a null/garbage
// vtable pointer on an object involved in the same Team/Clubs background
// poll), reached from a different call site one level up the chain, in
// sub_82EF3FC8 (skate3_recomp.110.cpp:14736). Reproduced under the VS
// debugger: r29 was 0x3010 - not a real object.
//
// Notably reproduced with the Blaze server not even running, which rules
// out server response shape as this crash's cause: this is a client-side
// path (almost certainly a connection-timeout/failure branch) that runs
// the same unguarded vtable dispatch regardless of whether the RPC ever
// completes.
//
// Installed via [[midasm_hook]] in manifests/skate3.toml.in at 0x82EF40CC
// (the `lwz r11,0(r29)` that starts the doomed dereference chain), jumping
// to 0x82EF40E0 (the instruction right after the bctrl) when r29's vtable
// pointer doesn't look like live guest memory.
bool GuardNullClubSubObjectVtable29(PPCRegister& r29) {
  uint8_t* base = REX_KERNEL_STATE()->memory()->virtual_membase();
  return HasGarbageVtable(base, r29.u32);
}

// Guard against a third crash in the same Team/Clubs data-fetch chain, one
// level further up in sub_82EF2CB0 (skate3_recomp.110.cpp:11812). Earlier
// in this function, an out-parameter local (r1+96) is explicitly
// zero-initialized (`li r11,0; stw r11,96(r1)`) and its address passed to
// sub_82EF5520 - the classic "out pointer, may be left null if there's
// nothing to return" pattern, matching our Clubs server stub answering
// getClubMembershipForUsers/getInvitations with an empty response (no
// club). The function never checks whether that out-param actually got
// filled in before using it: it reloads r3 from the same stack slot here
// and immediately dispatches through its vtable slot 8 unconditionally.
// Reproduced under the VS debugger: r3 was exactly 0 at this point.
//
// r3 is a stack-local reload, not derived from anything available at
// function entry, so - like GuardNullClubSubObjectVtable above - this has
// to be a mid-function check at the actual read site, not an entry guard.
//
// By this point r31 already holds the (already HRESULT-style) result of
// the previous call in this same function, and the function's own ending
// is just `r3 = r31; return` (loc_82EF2D80, the same shared return point
// GuardNullClubSubObjectVtable's sibling check already targets one
// function down the chain). Skipping to it preserves that status instead
// of running the doomed call.
//
// Installed via [[midasm_hook]] in manifests/skate3.toml.in at 0x82EF2D70
// (the `lwz r11,0(r3)` right after r3 is reloaded from the stack, before
// it's dereferenced), jumping to 0x82EF2D80 (the function's shared return
// point) when r3 doesn't look like live guest memory.
bool GuardNullClubQueryResult(PPCRegister& r3) {
  auto looksLikeGuestPtr = [](uint32_t addr) {
    return addr >= 0x40000000u && addr < 0x90000000u;
  };

  return !looksLikeGuestPtr(r3.u32);  // skip if the out-param was left null/garbage
}

// A different bug than the four Clubs RPC-proxy guards above: this one is
// in sub_82B20EF8's own error-handling path (skate3_recomp.72.cpp:24232),
// the generic RPC plumbing that calls into the Clubs proxy - the same
// function whose `cmpwi r3,0 / bge` was cited as evidence the plumbing
// already expects a negative HRESULT-style return from the guards above.
// It does: reached only when the Clubs call returns an error, it reads a
// listener/notification object from this+8 and dispatches through its
// vtable unconditionally, with no check that the pointer is real.
// Reproduced under the VS debugger: r11 (this+8) was 15 - not a real
// object, same invalid-pointer signature as every guard above.
//
// Whatever this listener mechanism is for, it isn't reached by any
// already-working RPC type (GameManager, Authentication, etc. never
// crashed here), only by the Clubs error path this session's own guards
// newly started exercising - so it was already broken/never-hit code
// before now, not something introduced by the guards above.
//
// The instruction right after the crashing bctrl unconditionally sets
// r3 = 0 and returns (it doesn't use the call's result), so skipping the
// call preserves the function's real return convention exactly.
//
// Installed via [[midasm_hook]] in manifests/skate3.toml.in at 0x82B210F4
// (the instruction right after `lwz r11,8(r31)`, checking the loaded
// value before it's dereferenced), jumping to 0x82B21110 (right after the
// bctrl) when r11 doesn't look like live guest memory.
bool GuardNullClubErrorListener(PPCRegister& r11) {
  auto looksLikeGuestPtr = [](uint32_t addr) {
    return addr >= 0x40000000u && addr < 0x90000000u;
  };

  return !looksLikeGuestPtr(r11.u32);
}

// A different failure shape than every guard above: in sub_82B48C48
// (skate3_recomp.73.cpp:57893, one level further up the same chain),
// reproduced under the VS debugger with this = r31 = 0x830E61EC (a real,
// in-range pointer, unlike the null/near-zero objects every prior guard
// caught) and vtable = r11 = 0x830DA670 (also in-range). But the "method
// pointer" read from that vtable's slot 0 was 0x830DA6A0 - only 48 bytes
// past the vtable pointer's own address, i.e. data from the same small
// blob as the vtable itself, not a real code pointer. Address-range
// heuristics (the "0x40000000-0x90000000" check every guard above uses)
// can't catch this: Skate 3's title memory maps code, data and heap into
// overlapping ranges, so a wrong-but-in-range address passes those checks
// fine. This is either a stale/dangling reference to freed-and-reused
// memory, or a type-confused object that was never actually the
// polymorphic Clubs-interface type this code expects.
//
// Rather than add another ad hoc range guess, this asks the same question
// REX_CALL_INDIRECT_FUNC itself asks before trusting a target address:
// rex::runtime::ResolveIndirectFunction (declared in rex/ppc/context.h)
// is the actual runtime function-dispatch lookup, so "is this address a
// real, registered function" is answered from ground truth instead of a
// heuristic, and this same check is reusable for any future crash in this
// shape without having to reason about address ranges again.
//
// The instruction right after this bctrl does NOT stop using the same
// corrupted r31 - it immediately dispatches through it a second time
// (`lwz r9,0(r31); lwz r8,12(r9); bctrl`) before r31 finally gets replaced
// with r28 (`mr r31,r28`, the natural convergence point after both calls
// in the working case). Reproduced under the VS debugger: on one run only
// the first of these two calls failed to resolve (jumping past both was
// enough); on another run the first happened to resolve to *something*
// (this is reused/corrupted memory, so which slot looks "valid" isn't
// consistent run to run) and only the second one crashed. So both calls
// need their own independent guard, landing at the same shared point.
//
// Installed via [[midasm_hook]] in manifests/skate3.toml.in at 0x82B48E84
// (the first bctrl), jumping to 0x82B48EA0 (past both vtable dispatches on
// the corrupted object, to where r31 is reassigned from r28) when the
// resolved target isn't an actual registered function.
//
// NOTE: rex::runtime::ResolveIndirectFunction never actually returns
// nullptr - on failure it returns a pointer to an internal InvalidFunctionTrap
// stub (see rexglue-skate3/src/system/function_dispatcher.cpp), so an
// `== nullptr` check on it (as an earlier version of this guard did) never
// triggers and silently lets the doomed call through. The dispatcher's own
// FunctionDispatcher::GetFunction, which ResolveIndirectFunction wraps, does
// return nullptr on failure and is the public API this guard actually needs.
namespace {
bool IsRegisteredFunction(uint32_t guestAddress) {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::runtime::FunctionDispatcher* dispatcher =
      runtime ? runtime->function_dispatcher() : nullptr;
  return dispatcher && dispatcher->GetFunction(guestAddress) != nullptr;
}
}  // namespace

bool GuardUnresolvedClubVtableSlot(PPCRegister& r10) {
  return !IsRegisteredFunction(r10.u32);
}

// Second half of the pair above: the same corrupted-object crash, on the
// second of the two consecutive vtable dispatches in sub_82B48C48
// (skate3_recomp.73.cpp:57908). Installed at that bctrl (0x82B48E9C),
// jumping to the same shared landing point (0x82B48EA0, where r31 gets
// reassigned from r28) when r8's resolved target isn't a real function.
bool GuardUnresolvedClubVtableSlot2(PPCRegister& r8) {
  return !IsRegisteredFunction(r8.u32);
}

// One level further up the same Team/Clubs poll chain, outside the Clubs
// RPC-proxy class entirely: in sub_828DD9F0 (skate3_recomp.50.cpp:7473),
// generic notification/callback plumbing that dispatches through an object
// reached via r6+8. Reproduced under the VS debugger: r6 was 80 - not a
// real object, same invalid-pointer signature as the guards below.
// r6 isn't derived from this function's own "this" pointer (r31, used
// separately and safely both before and after this block), so skipping
// just this one call is safe.
//
// Installed via [[midasm_hook]] in manifests/skate3.toml.in at 0x828DDB34
// (the `lwz r3,8(r6)` that starts the doomed dereference chain), jumping
// to 0x828DDB48 (the instruction right after the bctrl) when r6 doesn't
// look like live guest memory.
bool GuardNullCallbackObject(PPCRegister& r6) {
  auto looksLikeGuestPtr = [](uint32_t addr) {
    return addr >= 0x40000000u && addr < 0x90000000u;
  };

  return !looksLikeGuestPtr(r6.u32);
}

// Guard against another stale-interface crash, this time in sub_82F0E988
// (skate3_recomp.111.cpp:9429), reached from deep in the same Team/Clubs
// data-fetch chain (sub_82EEF128 -> ... -> sub_82EFCD48 -> sub_82F0EA68 ->
// sub_82F0E988). Reproduced under the VS debugger: at the crash, r31 (the
// "this" pointer used partway through the function) was 1 - not a real
// object, the wrapped result of some upstream code computing
// (interface_ptr + 4) from an interface_ptr of 0xFFFFFFFD (-3), i.e. an
// error/"not found" sentinel flowing downstream as if it were a valid
// pointer, unguarded. Left alone, the function does
// `lwz r11,0(r31)` / `lwz r11,4(r11)` / `bctrl`, walking off into unmapped
// host memory - a real 0xC0000005 access violation, not the recompiler's
// own InvalidFunctionTrap.
//
// r31 is set once, at function entry, directly from r3 (the incoming "this"
// argument) and is never reassigned on the path that reaches the crash, so
// the same check can be made up front instead of via a mid-function
// [[midasm_hook]] (which would need a full codegen regenerate just to add
// one guard - unlike SkipStaleTeamInterfaceCall's call-site hook, this one
// doesn't need that: the whole function can be intercepted at entry like
// every other override in this file).
//
// sub_82F0E988 already has a graceful failure convention for this exact
// situation: elsewhere in the function, a sibling branch (the "r30 == 0"
// case) sets its return value to 0x8007000E instead of proceeding, and
// every caller in this chain already treats a negative return as "no data,
// not fatal" (checked via cmpwi/blt). This override reuses that same
// convention for the "r3 isn't a plausible object pointer" case: skip the
// real function entirely and hand back the same HRESULT-style error code,
// rather than letting it run on a garbage "this" pointer.
extern "C" REX_FUNC(sub_82F0E988) {
  auto looksLikeGuestPtr = [](uint32_t addr) {
    return addr >= 0x40000000u && addr < 0x90000000u;
  };

  if (!looksLikeGuestPtr(ctx.r3.u32)) {
    REXLOG_INFO("[club-data] sub_82F0E988: bad this={:08X}, returning 0x8007000E instead of running",
                ctx.r3.u32);
    ctx.r3.s64 = -2147024896;
    ctx.r3.u64 = ctx.r3.u64 | 14;
    return;
  }

  __imp__sub_82F0E988(ctx, base);
}
