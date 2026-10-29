// APT (Flash UI) wide-aspect spreading: move HUD elements out to the edges of
// a wider-than-16:9 output without stretching them.
//
// The screen movies are authored on a 1280x720 stage and their HUD elements
// are the movie's direct children, whose translation is the element's screen
// position (everything below is a local offset). The native renderer fits the
// 16:9-authored 2D stream into a centered band (clip.x scaled by
// (16/9) / output aspect), so an element whose root translation is pushed
// out by dx stage pixels lands dx * band scale further out on screen while
// its textures keep their size.
//
// Skate 3 always runs APT's "Opti" render path (AptIntegration init ORs bit
// 4 into [0x830856D8]): PushMatrices (rex_PushMatrices_82E82F88) pushes onto a global
// matrix stack -- base pointer [0x830854D8], u16 index 0x830854E4, 80-byte
// entries (Mat44: a +0, b +4, c +16, d +20, tx +48, ty +52; colour +68/+76)
// with entry[idx] = item->matrix x entry[idx-1]; PopMatrices (rex_PopMatrices_82E83158)
// decrements the index. The wrappers above the screen movies (main.apt,
// the screen manager) are identity, so entries are in 1280x720 stage space.
// Adding dx to an element root's entry right after its push moves the whole
// subtree; the pop discards it, so the CIH state (script _x, hit tests) never
// sees it. Subtrees that did not change are replayed from cached render
// lists without a push; stamps in the entries let the leaf hook move those
// copies to the current aspect.
//
// An element root's side comes from where its leaves land, not from its own
// origin (see RootBounds); edge anchoring then keeps each side's margin.
//
// Ported from sk83_na_m (src/skate3_apt_spread.cpp there), where the layout
// was measured; retail's PushMatrices / PopMatrices / drawCharacterInstOpti /
// CallLegacyTransformCallbacks match the dev build instruction for
// instruction apart from these globals. Measured (skate3_apt_probe): screen movies sit at stack depth 4 under the
// screen manager (d2 main.apt 854x480 -> d3 550x400) or at depth 2 (autosave);
// hudbase nests a 1280x720 movie of its own.

#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <stdlib.h>

#include "skate3_apt_spread.h"
#include "skate3_native_scene.h"
#include "skate3_vr.h"

REXCVAR_DECLARE(bool, skate3_native_render_scene);

REXCVAR_DEFINE_BOOL(skate3_apt_spread, true, "Skate3",
                    "Wide outputs: move the 16:9 HUD's elements out toward the screen "
                    "edges (unscaled) instead of keeping the whole HUD in a centered band");
REXCVAR_DEFINE_INT32(skate3_apt_spread_mode, 0, "Skate3",
                     "HUD spreading: 0 = edge anchoring (elements left / right of a narrow "
                     "center zone keep their edge margin), 1 = proportional (positions "
                     "scale out from the center)");
REXCVAR_DEFINE_BOOL(skate3_apt_spread_all, false, "Skate3",
                    "Spread every 1280x720 APT screen (menus too), not only the HUD movies");
REXCVAR_DEFINE_BOOL(skate3_apt_probe, false, "Skate3",
                    "Log each APT render item (depth <= 7) when first pushed and whenever "
                    "its world x changes");

namespace {

constexpr uint32_t kOptiStackBasePtr = 0x830854D8;
constexpr uint32_t kOptiStackIndex = 0x830854E4;
constexpr uint32_t kOptiEntrySize = 80;
constexpr uint32_t kIdentityAptMatrix = 0x8307FFDC;
constexpr uint32_t kCharShape = 1, kCharText = 2, kCharSprite = 5, kCharMovie = 9;
constexpr float kStageW = 1280.0f;
constexpr float kStageCenter = kStageW * 0.5f;

// The 1280x720 HUD movies (data/fe/source/screens/hud2) plus the autosave
// icon. The end-of-challenge panels (hudComponents) are centered dialogs
// built from several sibling roots and stay in the band.
constexpr std::string_view kHudMovies[] = {
    "actionoverlay2", "dpo", "highlight_reel_overlay", "homscoring",
    "hud_freeskateactivity_overlay", "hud_online_eventlog2", "hudabd", "hudbase",
    "hudcentral", "hudcountdown", "huddistance", "hudminimap", "hudobjective2",
    "hudonlinevoting", "hudphonelist", "hudprefilm", "hudthrowmes", "hudtimer",
    "hudtutorial", "leaderboardhud", "manualmeter", "online_chyron", "onlineotsstat",
    "trickanalyser", "trickdisplay2", "tutorial", "autosave",
};

uint32_t LoadU32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, REX_RAW_ADDR(addr), 4);
  return _byteswap_ulong(v);
}
uint16_t LoadU16(uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, REX_RAW_ADDR(addr), 2);
  return _byteswap_ushort(v);
}
float LoadF32(uint8_t* base, uint32_t addr) {
  const uint32_t u = LoadU32(base, addr);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void StoreU32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(REX_RAW_ADDR(addr), &v, 4);
}
void StoreF32(uint8_t* base, uint32_t addr, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = _byteswap_ulong(u);
  std::memcpy(REX_RAW_ADDR(addr), &u, 4);
}

bool InGuestHeap(uint32_t a) { return a >= 0x40000000u && a < 0xC0000000u; }

// "<name>" of the "__Packages.<name>" string in the movie's .apt image,
// searched between the "Apt Data:" header (scanning back from the Movie
// character) and the character itself. "" when there is none.
std::unordered_map<uint32_t, std::string> g_movie_names;
const std::string& MovieName(uint8_t* base, uint32_t movie_char) {
  auto it = g_movie_names.find(movie_char);
  if (it != g_movie_names.end()) return it->second;
  std::string name;
  for (uint32_t back = 0; back <= 0x100000 && movie_char >= back + 0x40000000u; back += 4) {
    const uint32_t blob = movie_char - back;
    if (std::memcmp(REX_RAW_ADDR(blob), "Apt Data:", 9) != 0) continue;
    const std::string_view hay(reinterpret_cast<const char*>(REX_RAW_ADDR(blob)), back);
    constexpr std::string_view kPrefix = "__Packages.";
    const size_t at = hay.find(kPrefix);
    if (at != std::string_view::npos) {
      const char* s = hay.data() + at + kPrefix.size();
      name.assign(s, strnlen(s, std::min<size_t>(64, hay.size() - at - kPrefix.size())));
    }
    break;
  }
  return g_movie_names.emplace(movie_char, std::move(name)).first->second;
}

bool IsHudMovie(const std::string& name) {
  return std::find(std::begin(kHudMovies), std::end(kHudMovies), name) != std::end(kHudMovies);
}

// Per stack depth: the spreading state of the item pushed there (Pop only
// decrements the index, so entries below the current index belong to this
// item's ancestors).
enum class State : uint8_t {
  kNone,      // outside any spread scope
  kScope,     // spread movie: its children are element roots
  kPassThru,  // root container at the movie origin: its children are roots
  kDone,      // inside an element root (shifted or not)
};
constexpr uint32_t kMaxDepth = 64;
State g_state[kMaxDepth];
// Unshifted stage x of the element root each depth belongs to.
float g_root_x[kMaxDepth];

// Shift unit of an element whose visual center is at stage x: its offset is
// u * (k - 1) stage pixels for k = output aspect / (16/9). Edge anchoring
// keeps only a narrow center zone in place (the centered HUD pieces sit
// within 570..763: manual meter, trick analyser, countdown, voting).
constexpr float kCenterHalfWidth = 128.0f;
float SpreadUnit(float x) {
  if (REXCVAR_GET(skate3_apt_spread_mode) == 1) {
    return x - kStageCenter;
  }
  if (x < kStageCenter - kCenterHalfWidth) return -kStageCenter;
  if (x > kStageCenter + kCenterHalfWidth) return kStageCenter;
  return 0.0f;
}

// Stamps in Mat44 slots the 2D path never sets (memset to 0 on push, ignored
// by MatMul2d and CallLegacyTransformCallbacks), inherited by every entry
// below an element root: the root item (+36), its shift unit (+56) and the k
// its tx was shifted with (+44). Cached render lists keep whole-entry copies,
// so the stamps reach every leaf draw (see the CallLegacyTransformCallbacks
// hook).
constexpr uint32_t kStampRoot = 36;
constexpr uint32_t kStampBakedK = 44;
constexpr uint32_t kStampUnit = 56;

// Where an element root's leaves actually land (unshifted stage x of each
// leaf origin), per ~100 ms epoch. A root's own origin is no guide to that:
// trickdisplay2's trick-name stack hangs off a root at the stage center, an
// offset +447 (x1.75) to the right, then -621.5 back to the left edge.
struct RootBounds {
  uint32_t epoch = 0;
  float cur_min = 1e9f, cur_max = -1e9f;
  float last_min = 0.0f, last_max = 0.0f;
  bool have_last = false;
  // Text boxes below the root, relative to the root's own origin (stable
  // while the element animates or replays from cache). A leaf origin is a
  // text field's LEFT edge, while the game centers text inside its box:
  // hudcentral's district name sits at x 334 in a 616-wide box, centered
  // on the stage.
  float text_rel_min = 1e9f, text_rel_max = -1e9f;
  float unit = 0.0f;
  bool decided = false;
};
std::mutex g_bounds_mutex;
std::unordered_map<uint32_t, RootBounds> g_bounds;

uint32_t CurrentEpoch() {
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count() /
                  100);
}

void RecordLeaf(uint32_t root, float x) {
  const uint32_t epoch = CurrentEpoch();
  std::lock_guard<std::mutex> lock(g_bounds_mutex);
  if (g_bounds.size() > 8192) g_bounds.clear();
  RootBounds& b = g_bounds[root];
  if (b.epoch != epoch) {
    if (b.cur_min <= b.cur_max) {
      b.last_min = b.cur_min;
      b.last_max = b.cur_max;
      b.have_last = true;
    }
    b.cur_min = 1e9f;
    b.cur_max = -1e9f;
    b.epoch = epoch;
  }
  b.cur_min = std::min(b.cur_min, x);
  b.cur_max = std::max(b.cur_max, x);
}

void RecordText(uint32_t root, float rel_left, float rel_right) {
  std::lock_guard<std::mutex> lock(g_bounds_mutex);
  RootBounds& b = g_bounds[root];
  b.text_rel_min = std::min(b.text_rel_min, rel_left);
  b.text_rel_max = std::max(b.text_rel_max, rel_right);
}

// The root's shift unit: from its leaves' last measured extent plus its
// text boxes (sticky within 48 px of a zone edge so animation near a
// boundary cannot flip it), else from its own origin until leaves have been
// seen.
float RootUnit(uint32_t root, float origin_x) {
  std::lock_guard<std::mutex> lock(g_bounds_mutex);
  auto it = g_bounds.find(root);
  if (it == g_bounds.end() || !it->second.have_last) {
    return SpreadUnit(origin_x);
  }
  RootBounds& b = it->second;
  float lo = b.last_min, hi = b.last_max;
  if (b.text_rel_min <= b.text_rel_max) {
    lo = std::min(lo, origin_x + b.text_rel_min);
    hi = std::max(hi, origin_x + b.text_rel_max);
  }
  const float x = 0.5f * (lo + hi);
  const float u = SpreadUnit(x);
  if (b.decided && u != b.unit && REXCVAR_GET(skate3_apt_spread_mode) == 0) {
    const float edge_dist =
        std::min(std::abs(x - (kStageCenter - kCenterHalfWidth)),
                 std::abs(x - (kStageCenter + kCenterHalfWidth)));
    if (edge_dist < 48.0f) return b.unit;
  }
  b.unit = u;
  b.decided = true;
  return u;
}

void ProbeNewItem(uint8_t* base, uint32_t idx, uint32_t item, uint32_t entry, uint32_t chr,
                  uint32_t type, float dx) {
  // Logged again whenever the item's final world x moves by a pixel.
  static std::unordered_map<uint32_t, int> seen;
  if (idx > 7 || seen.size() >= 20000) return;
  const int wx = int(LoadF32(base, entry + 48));
  auto [it, fresh] = seen.emplace(item, wx);
  if (!fresh && it->second == wx) return;
  it->second = wx;
  const uint32_t lm = LoadU32(base, item + 8);
  std::string local = "identity";
  if (InGuestHeap(lm) && lm != kIdentityAptMatrix) {
    local = fmt::format("[{:.2f} {:.2f} {:.2f} {:.2f} | {:.1f} {:.1f}]", LoadF32(base, lm),
                        LoadF32(base, lm + 4), LoadF32(base, lm + 8), LoadF32(base, lm + 12),
                        LoadF32(base, lm + 16), LoadF32(base, lm + 20));
  }
  std::string movie;
  if (type == kCharMovie) {
    movie = fmt::format(" movie=\"{}\" stage={}x{}", MovieName(base, chr),
                        LoadU32(base, chr + 0x24), LoadU32(base, chr + 0x28));
  }
  REXLOG_INFO("apt-probe: {:>{}}d{} type={} item={:08X} chr={:08X} state={} dx={:.1f} "
              "local={} world=({:.1f} {:.1f}){}",
              "", idx * 2, idx, type, item, chr, int(g_state[idx]), dx, local,
              LoadF32(base, entry + 48), LoadF32(base, entry + 52), movie);
}

}  // namespace

float Skate3AptSpreadFactor() {
  if (!REXCVAR_GET(skate3_apt_spread) || !REXCVAR_GET(skate3_native_render_scene) ||
      !rex::graphics::IsNativeGuestOutputActive() || skate3::vr::Active()) {
    return 1.0f;
  }
  const float out = skate3::native_scene::g_native_output_aspect.load(std::memory_order_relaxed);
  const float k = out / (16.0f / 9.0f);
  return k > 1.01f ? std::min(k, 4.5f) : 1.0f;
}

extern "C" REX_FUNC(rex_PushMatrices_82E82F88) {
  const uint32_t item = ctx.r4.u32;
  __imp__rex_PushMatrices_82E82F88(ctx, base);

  const uint32_t stack = LoadU32(base, kOptiStackBasePtr);
  const uint32_t idx = LoadU16(base, kOptiStackIndex);
  if (stack == 0 || idx == 0 || idx >= kMaxDepth || !InGuestHeap(item)) return;
  const uint32_t entry = stack + idx * kOptiEntrySize;
  const uint32_t chr = LoadU32(base, item + 4);
  const uint32_t type = InGuestHeap(chr) ? LoadU32(base, chr) : 0;
  const State parent = g_state[idx - 1];

  float dx = 0.0f;
  State state = State::kNone;
  const uint32_t parent_entry = entry - kOptiEntrySize;
  const bool screen_movie = type == kCharMovie && LoadU32(base, chr + 0x24) == 1280 &&
                            LoadU32(base, chr + 0x28) == 720;
  if (parent == State::kDone) {
    state = State::kDone;
    // Descendants of an element root carry its stamps.
    for (const uint32_t off : {kStampRoot, kStampUnit, kStampBakedK}) {
      std::memcpy(REX_RAW_ADDR(entry + off), REX_RAW_ADDR(parent_entry + off), 4);
    }
    g_root_x[idx] = g_root_x[idx - 1];
    const uint32_t root = LoadU32(base, entry + kStampRoot);
    if (type == kCharText && InGuestHeap(root)) {
      // Text box (Text char: left +16, right +24) in unshifted stage x,
      // relative to the root.
      const float u = LoadF32(base, entry + kStampUnit);
      const float k = LoadF32(base, entry + kStampBakedK);
      const float tx = LoadF32(base, entry + 48) - u * (k - 1.0f);
      const float scale_x = LoadF32(base, entry);
      const float left = LoadF32(base, chr + 16);
      const float right = LoadF32(base, chr + 24);
      if (right > left && right - left < 4.0f * kStageW && scale_x > 0.0f) {
        RecordText(root, tx + left * scale_x - g_root_x[idx], tx + right * scale_x - g_root_x[idx]);
      }
    }
  } else if (screen_movie) {
    // A nested screen movie inherits its parent's scope (hudbase nests one).
    const bool allowed = parent == State::kScope || parent == State::kPassThru ||
                         REXCVAR_GET(skate3_apt_spread_all) ||
                         IsHudMovie(MovieName(base, chr));
    state = allowed ? State::kScope : State::kNone;
  } else if (parent == State::kScope || parent == State::kPassThru) {
    // Only x matters: a container at tx 0 (online_chyron's mPopup at (0, 52),
    // content at x 512) passes its children on as the element roots.
    const uint32_t lm = LoadU32(base, item + 8);
    const bool at_origin =
        !InGuestHeap(lm) || lm == kIdentityAptMatrix || LoadF32(base, lm + 16) == 0.0f;
    if (at_origin && (type == kCharSprite || type == kCharMovie)) {
      state = State::kPassThru;
    } else {
      state = State::kDone;
      // Stamped even when unshifted (u = 0, or k = 1) so the root's leaves
      // are measured and a later widening reaches cached copies.
      const float x = LoadF32(base, entry + 48);
      g_root_x[idx] = x;
      float u = 0.0f;
      if (at_origin && type == kCharShape) {
        // A shape placed at the origin carries its position in its geometry
        // (retail hudminimap's lens: a root shape at (0, 0) whose bounds are
        // 1063..1191, over mMiniMap), so its leaf origins say nothing: side
        // from the shape's bounds (Shape char: left +16, right +24). Shapes
        // spanning most of the stage are backdrops and stay in the band.
        const float left = LoadF32(base, chr + 16);
        const float right = LoadF32(base, chr + 24);
        if (right > left && right - left < kStageW * 0.6f) {
          u = SpreadUnit(x + 0.5f * (left + right));
        }
      } else {
        u = RootUnit(item, x);
      }
      const float k = Skate3AptSpreadFactor();
      dx = u * (k - 1.0f);
      StoreF32(base, entry + 48, x + dx);
      StoreU32(base, entry + kStampRoot, item);
      StoreF32(base, entry + kStampUnit, u);
      StoreF32(base, entry + kStampBakedK, k);
    }
  }
  g_state[idx] = state;
  if (REXCVAR_GET(skate3_apt_probe)) {
    ProbeNewItem(base, idx, item, entry, chr, type, dx);
  }
}

// CallLegacyTransformCallbacks (rex_CallLegacyTransformCallbacks_82E85038)(entry): every APT leaf's matrix, fresh or
// replayed from a cached render list, goes through here. A stamped entry
// baked with another k (window resized since the list was built) is moved
// to the current k in place, so the cache heals on its first replay.
extern "C" REX_FUNC(rex_CallLegacyTransformCallbacks_82E85038) {
  const uint32_t entry = ctx.r3.u32;
  const uint32_t root = InGuestHeap(entry) ? LoadU32(base, entry + kStampRoot) : 0;
  if (InGuestHeap(root)) {
    const float u = LoadF32(base, entry + kStampUnit);
    const float baked_k = LoadF32(base, entry + kStampBakedK);
    if (baked_k >= 1.0f && baked_k <= 4.5f) {
      const float k = Skate3AptSpreadFactor();
      float x = LoadF32(base, entry + 48);
      if (k != baked_k) {
        x += u * (k - baked_k);
        StoreF32(base, entry + 48, x);
        StoreF32(base, entry + kStampBakedK, k);
      }
      RecordLeaf(root, x - u * (k - 1.0f));
    }
  }
  __imp__rex_CallLegacyTransformCallbacks_82E85038(ctx, base);
}

// ---- Online nametags (world-projected cFont text, outside APT) ----
// Retail DrawName (827840E8 -> 82805B68 -> 82805938) projects the skater with the game's wide 3D camera, then passes
// the anchor in the fixed 1280x720 stage to cFont's shadow/text wrapper.
// Undo the renderer's 16:9 band fit for that anchor, as for the NPC arrow.
// Do this before glyph alignment and shadow offsets, so neither is scaled.
namespace {
thread_local int t_nametag_depth = 0;
struct NametagScope {
  NametagScope() { ++t_nametag_depth; }
  ~NametagScope() { --t_nametag_depth; }
};

void AdjustNametagAnchor(PPCContext& ctx) {
  if (t_nametag_depth == 0 || !REXCVAR_GET(skate3_native_render_scene) ||
      !rex::graphics::IsNativeGuestOutputActive() || skate3::vr::Active()) return;
  const float out = skate3::native_scene::g_native_output_aspect.load(std::memory_order_relaxed);
  const float k = out / (16.0f / 9.0f);
  if (std::isfinite(k) && k > 1.01f && std::isfinite(ctx.f1.f64)) {
    ctx.f1.f64 = kStageCenter + (ctx.f1.f64 - kStageCenter) * k;
  }
}
}  // namespace

extern "C" REX_FUNC(sub_827840E8) {
  NametagScope scope;
  __imp__sub_827840E8(ctx, base);
}

// cFont::Drawstring<char/unsigned short>: the outer shadow/text wrappers.
// Ordinary HUD/menu/debug strings retain their existing placement.
extern "C" REX_FUNC(sub_828064B8) {
  AdjustNametagAnchor(ctx);
  __imp__sub_828064B8(ctx, base);
}
extern "C" REX_FUNC(sub_82806650) {
  AdjustNametagAnchor(ctx);
  __imp__sub_82806650(ctx, base);
}

// ---- Chase arrow (Sk8::FE::ChaseArrowMan, outside APT) ----
//
// The arrow over an angry, chasing NPC is drawn by ChaseArrowMan::Render
// (rex_Sk8_FE_ChaseArrowMan_Render_QAAXIABVMatrix44_vpu_math_rw_Z_825E2B80; dev 0x8274B348) straight from FrontEndManager::Render2D,
// not through APT: it projects the chaser with the game camera -- which
// follows the output aspect (skate3_guest_aspect) -- into 1280x720 stage
// coordinates, and the renderer then fits the stage into the 16:9 band.
// A projected point must be spread proportionally to land on the chaser.

// ChaseArrowMan::RenderOnScreenArrow(this r3, x f1, y f2, scale f3, f4,
// state r7) (sub_825E31F8; dev 0x8274A1E0).
extern "C" REX_FUNC(sub_825E31F8) {
  const double x = ctx.f1.f64;
  const float k = Skate3AptSpreadFactor();
  if (REXCVAR_GET(skate3_apt_probe)) {
    static int n = 0;
    if (n++ % 30 == 0) {
      REXLOG_INFO("chase-arrow: on-screen f1={:.1f} f2={:.1f} f3={:.3f} f4={:.3f} r7={} k={:.3f}",
                  ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64, ctx.r7.s32, k);
    }
  }
  if (k != 1.0f && x > -kStageW && x < 2.0 * kStageW) {
    ctx.f1.f64 = kStageCenter + (x - kStageCenter) * k;
  }
  __imp__sub_825E31F8(ctx, base);
}

// ChaseArrowMan::RenderOffScreenArrow(this r3, angle f1, scale f2, alpha f3,
// state r6) (sub_825E2DF0; dev 0x82749DA8): places the arrow on the screen
// edge itself, in screen pixels: center (W/2, H/2) pushed out along the
// angle and clamped to the screen rectangle minus a margin, with W / H the
// ints at [0x83083C60] + 0x11FE8 / 0x11FEC. For the wide output the width
// is raised to W * k for the call (the clamp then reaches the wide edges),
// and the quad it hands to SimpleDraw (sub_824FA550) is moved back by
// (W * k - W) / 2 into the 1280-wide stage the renderer band-fits.
namespace {
constexpr uint32_t kRenderGlobalsPtr = 0x83083C60;
constexpr uint32_t kScreenDimA = 0x11FE8;
constexpr uint32_t kScreenDimB = 0x11FEC;
thread_local float t_offscreen_arrow_dx = 0.0f;
}  // namespace

extern "C" REX_FUNC(sub_825E2DF0) {
  const float k = Skate3AptSpreadFactor();
  const uint32_t globals = LoadU32(base, kRenderGlobalsPtr);
  uint32_t width_slot = 0, width = 0;
  if (k != 1.0f && InGuestHeap(globals)) {
    const uint32_t a = LoadU32(base, globals + kScreenDimA);
    const uint32_t b = LoadU32(base, globals + kScreenDimB);
    // The larger of the pair is the width (16:9 render target).
    width_slot = globals + (a >= b ? kScreenDimA : kScreenDimB);
    width = a >= b ? a : b;
    if (width < 320 || width > 8192) width_slot = 0;
  }
  if (width_slot != 0) {
    const uint32_t wide = uint32_t(std::lround(double(width) * k));
    StoreU32(base, width_slot, wide);
    t_offscreen_arrow_dx = -0.5f * float(wide - width);
    if (REXCVAR_GET(skate3_apt_probe)) {
      static int n = 0;
      if (n++ % 30 == 0) {
        REXLOG_INFO("chase-arrow: off-screen angle={:.3f} width {} -> {} dx={:.1f}", ctx.f1.f64,
                    width, wide, t_offscreen_arrow_dx);
      }
    }
  }
  __imp__sub_825E2DF0(ctx, base);
  if (width_slot != 0) {
    StoreU32(base, width_slot, width);
    t_offscreen_arrow_dx = 0.0f;
  }
}

// SimpleDraw textured quad setup (this r3, Vector4 verts[4] r4, uv r5,
// texture r6, colour r7, matrix r8); see RenderOffScreenArrow above.
extern "C" REX_FUNC(sub_824FA550) {
  const float dx = t_offscreen_arrow_dx;
  const uint32_t verts = ctx.r4.u32;
  if (dx != 0.0f && InGuestHeap(verts)) {
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t vx = verts + i * 16;
      StoreF32(base, vx, LoadF32(base, vx) + dx);
    }
  }
  __imp__sub_824FA550(ctx, base);
}
