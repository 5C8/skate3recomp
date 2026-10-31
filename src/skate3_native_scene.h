#pragma once
#include <atomic>

// Native scene renderer (data-driven): consumes the per-frame MeshContext
// stream collected by skate3_native_render.cpp, walks the verified guest
// structures on the game thread into a
// compact NativeFrameScene, and renders it with our own shaders into the
// guest output texture via the SDK native-guest-output hook, replacing the
// emulated frame when active.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <type_traits>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include "native/skate3_flat_map.h"
#include "skate3_deck_shadow.h"

namespace skate3::native_scene {

struct DrawEntry {
  uint32_t prim;
  uint32_t base_vertex;
  uint32_t start_index;
  uint32_t index_count;
};

// Vector of trivially copyable elements with inline storage for the first
// N: DrawItem::draws holds 1-4 entries for nearly every item, and items are
// copied into the published scene every frame (item cache -> scene, dynamic
// captures -> scene, retained copies), so a heap-backed list cost an
// allocation + free per item per frame on the guest render thread.
// Implements the std::vector subset the renderer uses.
template <typename T, uint32_t N>
class InlineVec {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  using value_type = T;
  using size_type = size_t;
  using iterator = T*;
  using const_iterator = const T*;
  using reference = T&;
  using const_reference = const T&;

  InlineVec() = default;
  InlineVec(const InlineVec& o) { assign(o.begin(), o.end()); }
  InlineVec(InlineVec&& o) noexcept { Steal(o); }
  InlineVec(std::initializer_list<T> il) { assign(il.begin(), il.end()); }
  InlineVec(const std::vector<T>& v) { assign(v.begin(), v.end()); }
  ~InlineVec() { delete[] heap_; }
  InlineVec& operator=(const InlineVec& o) {
    if (this != &o) assign(o.begin(), o.end());
    return *this;
  }
  InlineVec& operator=(InlineVec&& o) noexcept {
    if (this != &o) {
      delete[] heap_;
      heap_ = nullptr;
      Steal(o);
    }
    return *this;
  }
  InlineVec& operator=(const std::vector<T>& v) {
    assign(v.begin(), v.end());
    return *this;
  }
  operator std::vector<T>() const { return std::vector<T>(begin(), end()); }

  template <typename It>
  void assign(It first, It last) {
    const size_t n = size_t(std::distance(first, last));
    size_ = 0;
    reserve(n);
    std::copy(first, last, data());
    size_ = uint32_t(n);
  }
  void reserve(size_t n) {
    if (n <= capacity()) return;
    T* p = new T[n];
    if (size_ != 0) std::memcpy(p, data(), size_t(size_) * sizeof(T));
    delete[] heap_;
    heap_ = p;
    heap_cap_ = uint32_t(n);
  }
  void push_back(const T& v) {
    if (size_ == capacity()) {
      const T copy = v;  // v may live in this vector
      reserve(size_t(capacity()) * 2);
      data()[size_++] = copy;
      return;
    }
    data()[size_++] = v;
  }
  template <typename... A>
  T& emplace_back(A&&... a) {
    push_back(T{std::forward<A>(a)...});
    return back();
  }
  void pop_back() { --size_; }
  void resize(size_t n) {
    reserve(n);
    for (size_t i = size_; i < n; ++i) data()[i] = T{};
    size_ = uint32_t(n);
  }
  template <typename It>
  iterator insert(const_iterator pos, It first, It last) {
    const size_t at = size_t(pos - begin());
    const size_t n = size_t(std::distance(first, last));
    reserve(size_t(size_) + n);
    T* d = data();
    std::memmove(d + at + n, d + at, (size_t(size_) - at) * sizeof(T));
    std::copy(first, last, d + at);
    size_ += uint32_t(n);
    return d + at;
  }
  iterator erase(const_iterator first, const_iterator last) {
    T* d = data();
    const size_t a = size_t(first - d), b = size_t(last - d);
    std::memmove(d + a, d + b, (size_t(size_) - b) * sizeof(T));
    size_ -= uint32_t(b - a);
    return d + a;
  }
  iterator erase(const_iterator pos) { return erase(pos, pos + 1); }
  void clear() { size_ = 0; }
  void shrink_to_fit() {}

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  size_t capacity() const { return heap_ ? heap_cap_ : N; }
  T* data() { return heap_ ? heap_ : inline_; }
  const T* data() const { return heap_ ? heap_ : inline_; }
  iterator begin() { return data(); }
  iterator end() { return data() + size_; }
  const_iterator begin() const { return data(); }
  const_iterator end() const { return data() + size_; }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }
  T& operator[](size_t i) { return data()[i]; }
  const T& operator[](size_t i) const { return data()[i]; }
  T& front() { return data()[0]; }
  const T& front() const { return data()[0]; }
  T& back() { return data()[size_ - 1]; }
  const T& back() const { return data()[size_ - 1]; }

 private:
  void Steal(InlineVec& o) {
    size_ = o.size_;
    if (o.heap_) {
      heap_ = o.heap_;
      heap_cap_ = o.heap_cap_;
      o.heap_ = nullptr;
      o.heap_cap_ = 0;
    } else if (size_ != 0) {
      std::memcpy(inline_, o.inline_, size_t(size_) * sizeof(T));
    }
    o.size_ = 0;
  }

  T* heap_ = nullptr;
  uint32_t size_ = 0;
  uint32_t heap_cap_ = 0;
  T inline_[N];
};

struct DrawItem {
  // Guest MeshContext this item was captured from (0 for frame-end world
  // items served from the item cache). Identity key for the palette serve
  // and the entity store: the owning cModelInstance is resolved backward by
  // m_arrMeshContext containment (there is no forward mesh->owner pointer).
  uint32_t ctx = 0;
  // LivingWorld entity store mapping,
  // stamped at scene build when this ctx belongs to a live LW entity:
  // lw_alpha = the entity's authoritative opacity (entity+528.x: exactly
  // the value BindConstants serves this ctx as the shader's output alpha;
  // -1 = unmapped), lw_entity = the owning entity address (the game's own
  // per-instance identity, used to key the pose rings so same-mesh clone
  // reshuffles can never mispair).
  float lw_alpha = -1.0f;
  uint32_t lw_entity = 0;
  uint32_t mesh;      // guest mesh object address (resource cache key)
  uint32_t vb_obj;    // guest renderengine::VertexBuffer object (draw matching)
  uint32_t ib_obj;    // guest renderengine::IndexBuffer object (draw matching)
  uint32_t vb_addr;   // guest address of raw vertex data
  uint32_t ib_addr;   // guest address of raw index data (u16 big-endian)
  uint32_t vb_bytes;
  uint32_t ib_count;
  uint32_t diffuse_tex;   // guest renderengine::Texture address (0 = none)
  uint32_t lightmap_tex;  // guest renderengine::Texture address (0 = none)
  // Streamed-artwork diffuse override (event posters): the Massive ad
  // system rebinds the diffuse fetch slot at draw time with the current
  // event-ad art: either over a 16x16 min-mip placeholder channel (the
  // ace-of-spades "Big Event" grid) or REPLACING a full-size default
  // poster (the letter-writing frames showing the MONDO "THE NEW VIDEO"
  // ad). When the item's own main-pass draw (validated by slot 3 == this
  // item's lightmap) bound a different texture at fetch slot 4, these six
  // draw-time fetch words resolve the diffuse instead (words-keyed texture
  // cache). All zero = no override.
  uint32_t diffuse_fetch[6] = {};
  // Same mechanism for environment.decal: every decal family samples its
  // art at fetch slot 4 (diffuse rides slot 6). Filled by
  // ApplyDecalDrawAddressing from the item's main-pass draw whenever the
  // game bound a texture there other than the `decal` channel's object
  // (e.g. event-ad art over a decal section).
  uint32_t decal_fetch[6] = {};
  // "macrooverlay" material channel (environment shaders): a large-scale
  // grime/crack overlay multiplied over the diffuse at uv *
  // macroOverlayUVScale with macroOverlayOpacity: the ground/wall
  // weathering that reads as "extra grit".
  uint32_t macro_tex;     // guest renderengine::Texture address (0 = none)
  float macro_scale;      // macroOverlayUVScale channel constant
  float macro_opacity;    // macroOverlayOpacity channel constant
  // uAnimationSpeed / vAnimationSpeed channel constants (texcoords per
  // second of the frame's animation time): the scrollincandescent UV
  // scroll (fam 14: the stadium LED chyron band). 0 = no scroll.
  float scroll_u;
  float scroll_v;
  uint16_t pos_offset;
  uint16_t uv_offset;
  uint16_t uv2_offset;
  // A DEDICATED decal texcoord (usage index 3), present on the
  // environment.decal layouts (5 elements / stride 36). When absent the
  // decal art falls back to the .zw half of a half4 TEXCOORD0, which is
  // what the decal_tileable layouts carry instead.
  uint16_t uv3_offset;
  uint16_t bw_offset;  // blend weights (u8x4), valid when skinned
  uint16_t bi_offset;  // blend indices (u8x4), valid when skinned
  uint16_t normal_offset;  // usage-3 vertex normal (0 fmt = none)
  // usage-6 tangent / usage-7 binormal (k_10_11_11): NPC character meshes
  // (default_cloth / default_hair / livingworld peds / skateboard) carry NO
  // usage-3 normal; the game's VS derives it as cross(tangent, binormal)
  // (verified numerically against the emulated VS outputs, dot 0.9999).
  // tb_fmt = 16 when both are present; DecodeMesh then synthesizes the
  // normal, replacing the faceted ddx/ddy face-normal fallback.
  uint16_t tangent_offset = 0;
  uint16_t binormal_offset = 0;
  uint8_t tb_fmt = 0;
  uint8_t stride;
  uint8_t pos_fmt;    // xenos vertex format (26 s16x4, 32 half4, 57 float3)
  uint8_t uv_fmt;     // xenos vertex format of the first texcoord (0 = none)
  uint8_t uv2_fmt;    // xenos vertex format of the second texcoord (0 = none)
  uint8_t uv3_fmt;    // ... and of the dedicated decal texcoord (0 = none)
  uint8_t normal_fmt;  // 16 = k_10_11_11 packed (0 = none, face-normal fallback)
  // sky.* materials draw fullbright: per-facet lighting turns the multi-km
  // sky dome into visibly shaded rectangular panels.
  bool unlit;
  bool skinned;
  // Grayscale-tinted material (CAS hair): the shader multiplies the diffuse
  // by a per-character color staged in the PIXEL constant bank. Detected via
  // the AttribulatorMaterialName channel; tint captured with the palette.
  bool hair;
  // Character-family exact shading (defaultcharacter.fx and friends).
  // Classified from AttribulatorMaterialName: 1 = character.default_*
  // (defaultcharacter PS layout), 2 = CAC player pieces (skin / face / cloth
  // / leather / shift / cloth_ropa: cacstamp layout), 3 =
  // character.livingworld_* pedestrians (stamp recolor), 4 = character.hair
  // (CAC), 5 = character.default_hair (NPC hair), 6 =
  // character.livingworld_vehicles (vehicle.fx: colorize_red/blue paint
  // recolor + phong spec + environment-cube reflection), 7 =
  // character.livingworld_vehicles_glass (reflection-only, alpha-blended
  // windows). 0 = not a character.
  // char_rows = the canonical per-draw lighting block captured from the
  // PIXEL constant bank at palette-capture time (CaptureCharLighting):
  // 18 float4 rows consumed by the scene PS character branch (cbuffer CH,
  // b2). rows[14].y = family, > 0 only when the capture validated; items
  // without it fall back to the legacy empirical character shading.
  uint8_t char_family = 0;
  float char_rows[72] = {};
  // Hair strand coverage: the hair mesh's "alpha" channel texture, sampled
  // at the SECOND texcoord (raw float2); hair renders alpha-blended in the
  // sorted sub-pass (the opaque path is the blocky-helmet look).
  uint32_t hair_alpha_tex = 0;
  // CAC cloth dirt layer: the material's "decal2" channel (the cloth PSes'
  // tf5), blended over the tinted diffuse by its own alpha x the
  // per-character dirt amount (PS c22.y, char_rows tintB.x).
  uint32_t dirt_tex = 0;
  // AttribulatorMaterialName starts "environment.decal": wall/ground
  // sections whose pixel shader composites a `decal` art texture (graffiti,
  // painted branding, grime) over the base diffuse INSIDE the shader,
  // lerp(base, decal.rgb, decal.a), and draws OPAQUE (disassembled from
  // decalenvironment_defaultPS; these meshes ARE the surface there, so
  // rendering them alpha-blended punches holes in walls). Without the
  // composite the paint is missing (the no-graffiti bug).
  bool decal;
  // environment.decal_tileable material name. Only the FALLBACK addressing
  // (wrap; plain decals clamp) until decal_addr below records what the game
  // actually bound: the tileable and plain shaders are the same microcode.
  bool decal_tileable;
  // The decal art's addressing exactly as the GAME bound it on this item's
  // main-pass draw (tf4 = the art on every decal family; plain and
  // _tileable share identical microcode, so the sampler state IS the
  // difference): bit 15 = observed, bits 0-2 = clamp_x, 3-5 = clamp_y
  // (Xenos ClampMode, fetch dword 0 bits 10-15), 6-7 = border_color (fetch
  // dword 5 bits 0-1). renderengine writes these from its SamplerState in
  // Xbox2SetGpuTextureFetchRegister (82C727F8). 0 = not observed yet: the
  // shader falls back to the family default (tileable wraps, plain clamps).
  // Per-frame state (see ApplyDecalDrawAddressing), not part of the cached
  // item core.
  uint16_t decal_addr = 0;
  // VS c10.x of the same draw: the floor of the macro-overlay fade under
  // the art, ov' = lerp(1, ov, max(1 - art.a, decal_fade))
  // (decalenvironment_defaultPS 23/27/60). < 0 = not captured (no fade).
  float decal_fade = -1.0f;
  // environment.decal2 (decal2environment shaders): a SECOND art layer at
  // the game's tf5, sampled at the mesh's extra half2 vertex element (the
  // stride-36 layouts' usage-3 texcoord) and composited over the first.
  // The first layer (tf4) reads TEXCOORD0.zw like every decal family.
  bool decal2 = false;
  // Layer 2's tf5 fetch constant as the game bound it on the main-pass
  // draw (words[1] == 0 = not observed: layer 2 not drawn), and its
  // addressing in DrawItem::decal_addr's encoding. Per-frame state.
  uint32_t decal2_fetch[6] = {};
  uint16_t decal2_addr = 0;
  // trafficlight.one / trafficlight.two (env_family 17; both draw with the
  // same trafficlight_*_defaultVS/PS ucode): the game's per-draw lamp states
  // (VS c7.xyzw, one per lamp index; > 1 switches the cell) and emissive scale
  // (PS c3.y), captured off the item's main-pass draw. Per-frame state;
  // traffic_seen = false until the draw is observed (lamps unlit, scale 1).
  float traffic_c7[4] = {};
  float traffic_c3y = 1.0f;
  bool traffic_seen = false;
  // character.default_hom: the Hall of Meat x-ray (the skater's bones
  // through the body), drawn with defaulthom_defaultVS/PS. Skins like the
  // other character families (fam 1 palette); shades with the PS constants
  // captured off its own draw (hom_seen; until then it keeps the legacy
  // character shading): c2 = tint rgb + fresnel power, c7.xy = mix scale /
  // texture floor, c1.y = output scale. The game draws it as a plain opaque
  // LEQUAL + z-write draw (RB_DEPTHCONTROL 0x708737, blend ONE/ZERO) that
  // still shows through all geometry, i.e. against a depth buffer holding
  // only the bones; drawn last here against a cleared depth buffer of its
  // own (RendererState::hom_depth, see the HOM pass in RenderScene).
  bool hom = false;
  bool hom_seen = false;
  float hom_c2[4] = {};
  float hom_c7[2] = {};
  float hom_c1y = 1.0f;
  uint32_t decal_art;  // `decal` channel texture (0 = none)
  // AttribulatorMaterialName starts "environment.transparent": mist/cloud
  // sheets, glass, fences, vines. Alpha-BLENDED in a sub-pass after all
  // opaque items (back-to-front, depth test on, z-write off); the opaque
  // pass's alpha-test turned the soft mist gradients into solid hard-edged
  // white cloud blobs.
  bool transparent;
  // AttribulatorMaterialName starts "water.": canal/ocean surfaces
  // (flowingwater.fx family). Drawn in the transparent sub-pass with a
  // dedicated shading branch (ripple normal taps + fresnel reflection);
  // `water_normal` is the material's `normal` channel texture, bound in the
  // macro slot (water never carries a macro overlay).
  bool water;
  // AttribulatorMaterialName starts "water.flowing": the flowingwateralpha
  // shader family (canal / waterfall surfaces). These take the exact water
  // branch (hand-ported from flowingwateralpha_defaultPS and verified
  // per-pixel against the ucode) when the frame's water m_params rows are
  // captured; other water.* / ocean.* materials keep the legacy water
  // shading until their own shader layouts are verified from a capture.
  bool water_flowing = false;
  // water.flowing draws with flowingwater_defaultPS, whose output alpha is 1
  // (oC0._001); water.flowingalpha with flowingwateralpha_defaultPS (alpha
  // = max(term, c18.w)). The two are otherwise the same ucode.
  bool water_opaque = false;
  // This flowing-water item's own material rows and colour map, captured
  // off its draw (FrameScene::water_rows layout); the frame-global set
  // otherwise held ONE material's rows and colour map for every flowing
  // surface. Per-frame state.
  // The game's render-state registers for this item's own main-pass draw
  // (DrawRegState layout: depth, blend, color_ctl, color_mask, stencil,
  // mode_cntl), captured per island. FrameScene::draw_rs is keyed by index
  // buffer alone (last draw wins), and the game packs several meshes with
  // different materials into one buffer pair, so it could hand an item a
  // neighbour island's state. World / dynobj / park items only; per-frame.
  bool game_rs_seen = false;
  uint32_t game_rs[6] = {};
  bool water_rows_seen = false;
  float water_rows[32] = {};
  uint32_t water_tf4 = 0;
  // advertisement.default (fam 15): this item's own advertisement_defaultPS
  // rows (c6.z lightmap floor, c10.y multiplier, c9.x exposure) and the
  // textures the game binds on its draw: tf4 (the art; streamed ad art is
  // bound there at draw time) and tf3 (the lightmap the PS's four
  // half-texel taps read). The frame-global ad rows are ONE draw's, and the
  // material channels need not name what the draw binds (no lightmap
  // channel = flat white lighting). Per-frame state.
  bool ad_seen = false;
  float ad_rows[3] = {};
  uint32_t ad_fetch[6] = {};
  uint32_t ad_lm_fetch[6] = {};
  // water.alpha* materials run wateralpha_defaultPS, which shares this
  // port's shading but ends `max oC0.___w, r0.wwww, c25.wwww` -- a real
  // alpha -- where water_defaultPS ends `max oC0._001` (w = 1, opaque). So
  // these belong in the SORTED pass, blended, not the opaque one.
  bool water_alpha = false;
  // ocean materials: 1 = ocean.default (the sea surface, ocean_defaultPS:
  // PCA dual-component normal + ward spec + cube reflection, drawn OPAQUE),
  // 2 = ocean.reflection (the baked horizon reflection sheet, height-ramp
  // alpha). Exact branches gated on the frame's ocean rows.
  uint8_t water_ocean = 0;
  uint32_t water_normal;
  // `normal2` channel: the ocean's second PCA normal component map (the
  // flowingwater families ignore it).
  uint32_t water_normal2 = 0;
  uint32_t water_env;  // `environment` channel cube map (reflection term)
  // water_defaultPS refracts the FRAMEBUFFER from tf3. The game resolves the
  // scene into a texture and binds it there itself, so we take that binding
  // (recorded by the D3DDevice_SetTexture hook) rather than adding a
  // scene-colour copy of our own.
  uint32_t refract_tex = 0;
  // water_defaultPS masks (guest tf5): .x gates the sun specular and .z
  // gates the cube reflection, both thresholded by c25.y. The reflection
  // only fires where .z clears 0.76, which is what makes the reference's
  // foam PATCHY rather than a broad sheen.
  uint32_t water_mask = 0;
  // Usage-6 tangent present WITHOUT a usage-7 binormal (tan_fmt = 16, the
  // water-mesh layout: pos + uv + lm_norm + tangent). The flowingwater VS
  // reconstructs the frame as N from lm_norm.zw + sign bits, B =
  // sign.x * cross(N, T); DecodeMesh mirrors that (see the water pack).
  uint8_t tan_fmt = 0;
  // character.cloth_ropa (Ropa cloth-simulated garments, e.g. player tees):
  // the VS variant branches on a flag row kept in front of the bone palette
  // - sim active means the dynamic VB already holds deformed root-local
  // positions and the draw is RIGID (one affine at c188/c191), not skinned.
  // Detected via the AttribulatorMaterialName channel; consumed by
  // CaptureSkinnedState.
  bool ropa;
  // character.alpha: translucent CAC accessory pieces (sunglass lenses).
  // cac_alphaPS outputs alpha = the mesh's "alpha" channel texture sampled
  // at the raw second texcoord x the entity alpha constant (ucode: oC0.w =
  // tf5(uv2).r * c22.x) and the game draws them AFTER every opaque
  // character piece: rendered opaque the lens is a solid black patch.
  // Routed to the sorted alpha sub-pass like hair; the coverage texture
  // rides the same hair_alpha_tex/decal-slot plumbing (ch_misc.z > 0
  // signals the PS fam-2 branch to take alpha from t4@uv2).
  bool char_alpha = false;
  // character.skin (cacstamp_skin_defaultPS): the albedo is
  // D^2 * c23 * tf6(cg), tf6 = the CAS custom graphic (tattoo) the engine
  // binds at draw time (char_cg_fetch). cg = the custom-graphic placement of the RAW second texcoord,
  // (dot((u,v,1), c190.xyz), dot((u,v,1), c191.xyz)) from the skin VS's
  // i_customGraphic rows (vs_cacstamp_skin_defaultVS 40/41), and tf6
  // CLAMPS TO A WHITE BORDER (fetch dword0 clamp 6, dword5 border 1): the
  // graphic (a tattoo) covers only its placed rectangle, white elsewhere.
  // Sampled at the raw uv2 instead it painted the tattoo sheet over the
  // arms and darkened the whole face. character.face (cac_face_defaultPS)
  // reads its tf6 (the skater's injury map) only through a c22.z lerp,
  // non-zero once the skater is bruised (see face_inj_*).
  bool char_skin = false;
  // The skin draw's c190/c191 rows (xyz each) at the served pose's main
  // pass (see char_skin); char_cg_valid = captured this frame.
  float char_cg[6] = {};
  bool char_cg_valid = false;
  // The skin draw's tf6 fetch words: the CAS-chosen graphic, bound by the
  // engine at draw time (the material's decal channel is NOT it).
  uint32_t char_cg_fetch[6] = {};
  // character.face (cac_face_*PS): the face piece (see sss_*).
  bool char_face = false;
  // The face's injury layer (see g_frame_face_injury): tf6 fetch words and
  // c22.z strength, face_inj_valid = captured recently.
  uint32_t face_inj_fetch[6] = {};
  float face_inj_amount = 0.0f;
  bool face_inj_valid = false;
  // CAC cloth dirt / skin injury layer (see g_frame_char_layer): the fetch
  // words the game bound at the piece's main pass and its strength.
  uint32_t char_layer_fetch[6] = {};
  float char_layer_amount = 0.0f;
  bool char_layer_valid = false;
  // CAS subsurface scattering (see g_frame_skin_sss): the game ran this
  // piece's texture-space lighting pass within the last couple of builds;
  // sss_mask = its main pass's "blurmask" fetch words (face tf7, skin tf8),
  // sss_amount = c21.x. The native renderer then runs the unwrap + jitter
  // blur and the nis mix for the piece.
  bool sss = false;
  float sss_amount = 0.0f;
  uint32_t sss_mask[6] = {};
  // The game submitted this ctx through an ortho (shadow caster-cascade)
  // bank this frame; the game's caster list is PER-PIECE (the CAS trucker
  // hat never enters the shadow passes) and the native atlas pass mirrors
  // it for character items. Rescued/gap-filled copies inherit the stored
  // flag so a refused-capture frame doesn't blink the entity's shadow.
  bool shadow_caster = false;
  // The game drew these buffers into its world-shade pass this frame (see
  // g_frame_ws_keys): a caster of the native world-shade map.
  bool ws_caster = false;
  // The game drew these buffers through a `_skatepark` world pixel shader
  // this frame (see g_frame_dmo_keys): the item receives the skate.Park
  // shadow terms -- lerp(c11, 1, cascade) and lerp(c12, 1, park world map).
  bool dmo_recv = false;
  // The game drew these buffers through a park/dynamicobject `_foregroundPS`
  // variant this frame (see g_frame_fg_world): the editor's preview object
  // (pick-an-object menu). Its world is that draw's own VS world rows (the
  // published copy carried identity: its vertices are mesh-local), it is
  // drawn after the transparents in the game's two passes (inner faces
  // depth-ALWAYS, then outer faces LEQUAL: always visible) and shaded
  // without shadow / cookie terms (kd x albedo only).
  bool foreground = false;
  // The editor's SELECTED piece (being placed / moved): drawn with its
  // ordinary _defaultPS but its world only in that draw's VS rows (the
  // published copy carries identity) and in the game's two passes -- an
  // x-ray pass (uncull(ed), depth ALWAYS, no z-write: it shows through
  // whatever it overlaps) then a normal one (uncull(ed), LEQUAL + z-write).
  // Both write the stencil postfx_edgedetectstencil outlines.
  bool xray = false;
  // The selected piece while it overlaps another (can't be placed):
  // environmentparkdecal_highlightPS -- the normal shading with c24.rgb
  // added to the final output (ucode 110: `add oC0.xyz, r0, c24`; (0.251,
  // 0, 0) = the red warning). World from the draw's VS rows as for xray.
  bool highlight = false;
  float highlight_rgb[3] = {0.0f, 0.0f, 0.0f};
  // CSM tiles (bit k = cascade k) the game drew this ctx's caster into
  // within the last couple of builds (see g_frame_ortho_mag); 0 = unknown
  // (the native atlas pass then casts into every tile).
  uint8_t caster_cascades = 0;
  // ROPA shape blending (set by InterpolateDynamicItems): decoded shape
  // generations (DynDecodeJob seq) + weights reproducing the SAME temporal
  // kernel the body bones / garment world were evaluated with this frame:
  // the 8-tap boxcar when the world took it, else the plain bracketing
  // lerp. shape_count == 0 = no pairing this frame (draw the newest cached
  // decode). The stepped cloth shape against the interpolated body was the
  // tee jelly/clip-through (worse at LOWER fps: the excursion scales with
  // the guest period); a plain 2-generation lerp against the BOXCAR-
  // filtered body left the same period-scaled mismatch; the two filters
  // answer the same 60 Hz limb signal with different frequency responses,
  // so the kernels must match exactly.
  // 16 = the hard bound: the 8-tap kernel contributes at most two distinct
  // generations per tap, so no ingest cadence can overflow this.
  static constexpr int kShapeGens = 16;
  uint64_t shape_seq[kShapeGens] = {};
  float shape_w[kShapeGens] = {};
  int shape_count = 0;
  // Exact world-material shading family, classified from
  // AttribulatorMaterialName (models verified against the game's own pixel
  // shaders):
  // 0 = none (legacy empirical shading), 1 = environment.default
  // (baseenvironment), 2 = environmentsimple.default (defaultenvironment),
  // 3 = environment.decal, 4 = environment.decal_tileable(_simple),
  // 5 = environment.reflective, 6 = environment.reflective_simple,
  // 7 = environmentsimple.alphatest, 8 = environmentsimple.diffuse,
  // 9 = tree.default, 10 = animated.tree, 11 = proxyworld.default,
  // 12 = incandescent.default, 13 = environment.reflective_trans
  // (transparentenvironmentreflective: blended glass, alpha sub-pass),
  // 14 = incandescent.backlituvscroll (scrollincandescent: the emissive
  // time-scrolled LED chyron band).
  uint8_t env_family = 0;
  // dynamicobject.fx family (movable props: dispensers, dumpsters, benches,
  // cans, ...). 0 = not a dynamic object, 1 = dynamicobject.default,
  // 2 = dynamicobject.alphatest. These are rigid model-space props with per-
  // frame lighting rows (sun / ambient / exposure) captured from their PS
  // bank, NOT the frame-global world-material rows; hence a separate family
  // and its own scene PS branch (model verified exact against the game's
  // own pixel shader).
  //
  // 4 = environmentpark.diffuse, 5 = environmentpark.alphatest,
  // 6 = environmentpark.decal: the skate-park editor pieces (ramps, rails,
  // funboxes). They are dynamic objects that happen to move only in the
  // editor, and the game shades them as such -- all three park pixel
  // shaders do the DUAL shadow fetch (cascades + the baked world shade),
  // exactly like dynamicobject_defaultPS and unlike every static family
  // (baseenvironment does one fetch with no world term). They ride the
  // dynobj enum so the caster pass, pass ordering and mesh decode treat
  // them as props, but their lighting rows come from FrameScene::park_rows
  // (their own PS banks); the shading itself is ShadePark, not
  // ShadeDynObject -- see there for the two models.
  uint8_t dynobj = 0;
  // "specular" channel texture (spec mask / eccentricity / reflection mask)
  // for env families; the "noise" tint texture for animated.tree. Bound in
  // the decal slot (t4) on families that carry no decal art.
  uint32_t spec_tex = 0;
  // "detail" channel texture (env families 5/6): a constant 16x16 BC1 whose
  // hardware-decoded value folds into the normal composition as 2*d - 1,
  // derived per material at draw time (see the refl_bias cvars). On fams
  // 1/3/4 (per-pixel world shading) it is a REAL tiling detail normal map,
  // sampled per-pixel at t8 with the detailNormalUVScale channel constant
  // below.
  uint32_t detail_tex = 0;
  float detail_scale = 0.0f;
  float tint[4];  // rgb + enable flag in w
  // The item's per-draw state (bone palette for skinned, world matrix for
  // rigid model-space props) was not available at capture time; deferred
  // meshes draw AFTER the submit call, and world sort-list captures happen
  // before any draw. Cleared by the post-draw (ib,vb) fixup; items still
  // pending at frame end are dropped (wrong state renders garbage).
  bool pending;
  // Diagnosis: which path staged this item's palette/world. 0 = none/raw,
  // 1 = submit-exit capture, 2 = post-draw (ib,vb) fixup, 3 = bones-cache
  // rescue, 4 = ropa state rescue, 5 = ropa mode-flip hold (previous state
  // re-published while the flipped mode's decode is still in flight),
  // 6 = publish-time incoherent-palette heal (cached palette substituted
  // after the dense 32-sample spread gate failed).
  uint8_t dbg_src = 0;
  // The palette was captured from an ORTHOGRAPHIC (CSM caster-cascade)
  // bank. The game does not reliably re-evaluate fine bone animation for
  // the shadow passes (vehicle wheel spin measured ~40 ms stale in bursts)
  // - publishing such a palette as the item's MAIN pose makes the pose
  // stream jump and resets the motion-smoothing ring (the traffic judder).
  // Caster-sourced captures lose merge arbitration to perspective-bank
  // captures and stay registered for a refresh from the mesh's later
  // main-pass draw.
  bool caster_bank = false;
  // Cloth patch (non-indexed quad list, CPU-simulated absolute world-space
  // float3 verts): the renderer synthesizes quad->triangle indices instead
  // of reading a guest index buffer.
  bool cloth_quads;
  // Park-editor / object-mover selection: the game re-draws the selected
  // object after the sky (twice, stencil-marking it) and a postfx stencil
  // edge-detect adds the blue outline. Items matched to those re-draws by
  // (ib_obj, vb_obj, world translation) render into the native outline mask
  // (see kOutlineShaderSource).
  bool selected = false;
  // Re-appended from the off-screen retention map (edge-of-view guard band):
  // the game view-culled this item this frame, but the re-timed render
  // camera trails the guest pose and can still see it. Retained items
  // outlive their guest-side lifetime guarantees, so the render path draws
  // only their exact cached decodes: no guest-memory reads, no heals, no
  // miss enqueues.
  bool retained = false;
  // Captured through the dynamic-entity paths (characters, props, cloth,
  // quad-list garments) rather than the world sort lists. Set at capture so
  // every later copy (merges, rescues, retention re-publishes) inherits it;
  // the build-up showcase's dynamic-entities layer keys off it.
  bool dyn_entity = false;
  // Bone palette snapshot taken on the game thread: raw staged rows, 3
  // float4s per bone (column-vector affine [R | t], model -> world). The
  // guest staging bank is reused draw to draw, hence the copy.
  std::vector<float> bones;
  // Bone rows the game actually packed for this piece (the authoritative
  // palette serve knows the instance's count; rows past it are zero and
  // never referenced by a vertex). 0 = unknown (a draw-time bank capture):
  // treat every row as live. Lets per-frame palette work skip the dead tail
  // of the 84-row bank (character meshes use ~53).
  uint16_t bone_rows = 0;
  // Content fingerprint of the guest payloads. Streaming reuses arena
  // addresses and pages fill in after the mesh is first submitted, so cached
  // GPU copies must be revalidated against this every frame.
  uint64_t fingerprint;
  float world[16];    // row-vector convention, translation in row 3
  float bbox_min[3];  // mesh-local bounds, for decode validation
  float bbox_max[3];
  InlineVec<DrawEntry, 4> draws;
};

// The game's own render-state registers at a perspective 3D draw (read from
// the device register arrays, see g_rs_ptr_2200): RB_BLENDCONTROL0 (0x2201),
// RB_COLORCONTROL (0x2202, alpha test), RB_COLOR_MASK (0x2104),
// RB_DEPTHCONTROL (0x2200). Keyed by the draw's index buffer object.
struct DrawRegState {
  uint32_t blend = 0;
  uint32_t color_ctl = 0;
  uint32_t color_mask = 0xF;
  uint32_t depth = 0;
  // RB_STENCILREFMASK (0x210D, front face: ref bits 0-7, mask 8-15,
  // write mask 16-23). 0x210C is the BACK-face copy.
  uint32_t stencil = 0;
  // PA_SU_SC_MODE_CNTL (0x2205): cull_front bit 0, cull_back bit 1,
  // face bit 2, poly_mode bits 3-4, polymode_front_ptype bits 5-7,
  // polymode_back_ptype bits 8-10. The debug menu's Render -> Settings ->
  // Wireframe sets poly_mode = kDualMode(1) with both ptypes = kLines(1),
  // which is how the game draws the whole world as wireframe.
  uint32_t mode_cntl = 0;
};
// SimpleDraw debug-primitive vertex (see SimpleDrawCap): line endpoints a/b
// in WORLD space, colour, and the quad corner (x: 0 = at a, 1 = at b;
// y: side -1 / +1, 0 = plain triangle vertex).
struct SimpleDrawVert {
  float a[4];
  float b[4];
  float color[4];
  float corner[2];
};
struct FrameScene {
  uint64_t generation = 0;
  // Playback clock (steady_clock seconds) the scene's camera and dynamic
  // items were posed at (the smoother's g_smooth_play); 0 = not smoothed.
  // DIAGNOSTIC: the pace recorder compares it against present times.
  double play_time = 0.0;
  // Native reproduction of a PhotoEffects portrait pass (see PhotoJob):
  // render only `items` with this camera over clear_color, no post, into
  // the 1152x640 photo target.
  bool photo_job = false;
  float view_proj[16] = {};
  float cam_pos[3] = {};
  // Raw guest projection matrix (viewcam +0x60, row-vector, m23 = 1),
  // published alongside view_proj. Consumed by depth-based post passes
  // (SSAO linearize/unproject via m00/m11/m22/m32). Zeros until the first
  // publish; camera smoothing replaces only the view, never this.
  float proj[16] = {};
  // Global distance-fog parameter rows, captured once per frame from the
  // first main-pass draw's VS constant bank (main-pass layout: c4 = camera,
  // the validation key; c5 = fog ramp scale/bias/exponent, c6 = linear-space
  // fog color rgb + transmittance scale in w; identical across every
  // main-pass draw of a frame). Consumed by environment.transparent items.
  float fog_ramp[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  float fog_color[4] = {};
  // Dynamic-shadow (CSM) receiver constants, captured once per frame from a
  // world-material draw's PIXEL constant bank: raw rows c0..c8
  // (c0/c3/c4 = light-space X/Y/depth rows, c1/c2 = cascade 1/2 scale+offset,
  // c5.x = depth bias, c8 = shadow color + floor; c6 = sun dir, c7 = camera,
  // captured for validation only).
  bool shadow_valid = false;
  // Per-draw game render states this frame (see DrawRegState).
  FlatMap<uint32_t, DrawRegState> draw_rs;
  // The game's main render-target clear color this frame (see
  // g_clear_frame_rgb); the native frame clears to it when valid.
  float clear_color[3] = {};
  bool clear_valid = false;
  // The GAME camera's aspect (|m11 / m00| of the captured projection) BEFORE
  // the Hor+/Vert+ widening rewrites proj; 0 = unknown. The F5 hand-off ends
  // as soon as this is back to 16:9.
  float guest_cam_aspect = 0.0f;
  // |m11| of the game camera's projection before that widening: its
  // vertical field of view (the VR HUD sizes the 16:9 layer from it).
  float guest_cam_proj11 = 0.0f;
  // Character meshes rebuilt at a reused address since the last publish
  // (see BuildFrameScene's rebuild check): the render thread drops their
  // per-site texture history so a new garment never shows the old art.
  std::vector<uint32_t> rebuilt_meshes;
  // True while the rows above were captured within the last ~2 s of guest
  // frames. shadow_valid alone latches forever, so interior venues whose
  // environment banks never pass the capture sanity gate (park-editor
  // warehouses: near-zero sun, exposure ~0.001) serve stale outdoor rows;
  // sun-derived features (the native static sun-shadow map) must stand down
  // there instead of shading a sunless interior with an outdoor sun.
  bool shadow_fresh = false;
  // Extended to c0..c11: c10.x = scene exposure (2.5), c11.y = the material
  // multiplier (1.0), consumed by the exact world-shading tone chain.
  float shadow_rows[48] = {};
  // Frame-global rows of the OTHER world shader families, captured
  // opportunistically from their PS banks (debug-path classified):
  // [0..1] = tree PS c0.xy (lightmap scale 0.3435 / floor 0.02),
  // [2] = tree PS c4.y (tint multiplier), [3] = proxyworld PS c3.y (0.45).
  // Defaults from the day capture cover frames before the first hit.
  float family_rows[4] = {0.3435f, 0.02f, 1.0f, 0.45f};
  float tree_anim_rows[16] = {};
  bool tree_anim_valid = false;
  // flowingwateralpha.fx (the water.flowing* materials) m_params rows,
  // captured once per frame from a flowingwateralpha PS bank (debug-path
  // classified): [0..3] = m_params[0] (PS c11: normal scale xzw + the
  // material multiplier in y), [4..7] = m_params[1] (c12: the two animated
  // UV scroll speeds), [8..11] = m_params[2] (c13: the two tap UV scales),
  // [12..15] = m_params[3] (c14: y = mask threshold, z = spec power,
  // w = alpha floor), [16] = g_fAnimationTime (c15.x). Consumed by the
  // exact water scene PS branch (cam_pos.w = -30).
  bool water_valid = false;
  float water_rows[32] = {};
  // ocean_defaultPS material rows, captured once per frame from an ocean PS
  // bank (debug-path classified): [0..3] = PCA mean pre-swizzled for the
  // (R,G,B) channel order (PS c2.x, c2.z, c2.y), [4..27] = the six PCA
  // weight rows in model order (c3, c4, c7, c8, c5, c6), [28..31] =
  // m_params[0] (spec color, c12), [32..35] = m_params[1] (c13: y = mult,
  // z = tonedown scale, w = uv scale), [36..39] = m_params[2] (c14: ward
  // spread u/v + roughness).
  // water.default (water_defaultPS, fam 33). A DIFFERENT shader from
  // flowingwateralpha: PCA dual-component normals like the ocean, two
  // time-scrolled uv sets, cube reflection, fresnel and sun spec.
  // Rows, as read from this build's ucode:
  //   [0..3]   c8   PCA mean / tap bias
  //   [4..27]  c9..c14  six PCA weight rows
  //   [28..31] c22  normal strengths (.z/.w per tap set)
  //   [32..35] c23  scroll speeds (tap1 xy, tap2 zw)
  //   [36..39] c24  uv scales     (tap1 xy, tap2 zw)
  //   [40..43] c25  x = uv-scale multiplier, y = threshold, z = spec power
  //   [44..47] c26  x = animation time
  //   [48..51] c20  w = reflection strength
  //   [52..55] c21  y/z/w fresnel + spec scales
  bool waterdef_valid = false;
  float waterdef_rows[56] = {};
  // The game's tf7 for water_defaultPS: the SECOND normal tap set. The
  // ucode always fetches tf6 AND tf7 (lines 14-17); the port had only the
  // tf6 pair, with the tf7 taps gated off by an ocean-only flag. Published
  // per frame rather than stamped into items at mesh-decode time.
  uint32_t waterdef_tf7 = 0;
  // The game's tf6 for water_defaultPS: the FIRST normal tap set -- the
  // ripple map itself. item.water_normal resolves to a 16x16 5_6_5 texture
  // (a flat tint, not a normal map), so the decode was building its normal
  // from a 16-texel source magnified across the whole surface.
  // flowingwateralpha's tf4 colour map (see g_water_tf4).
  uint32_t water_tf4 = 0;
  // ocean_defaultPS's bound tf7 environment cube (see g_ocean_tf7).
  uint32_t ocean_tf7 = 0;
  uint32_t waterdef_tf6 = 0;
  // Per-material water.default constant sets (see kWaterdefSets): keyed by
  // the material's tf6 normal map, which is item.water_normal at the draw.
  float waterdef_set_rows[8][56] = {};
  uint32_t waterdef_set_key[8] = {};
  bool waterdef_set_have[8] = {};
  bool ocean_valid = false;
  // advertisement_defaultPS: [0] = c6.z lightmap floor, [1] = c10.y
  // material multiplier, [2] = c9.x exposure.
  float ad_rows[3] = {};
  bool ad_valid = false;
  // transparentenvironment_defaultPS: [0..2] = c12.xyz light direction,
  // [3] = c13.y material multiplier, [4..6] = c11.xyz spec tint,
  // [7] = c9.x exposure, [8] = c10.w / [9] = c11.w spec power scale/bias.
  float ts_rows[10] = {};
  bool ts_valid = false;
  float ocean_rows[40] = {};
  // oceanreflection row: x = PS c3.x multiplier, y/z = VS c7.y/c7.z, the
  // height-ramp alpha fade bounds.
  bool oceanrefl_valid = false;
  float oceanrefl_rows[4] = {};
  // scrollincandescent.fx rows, captured once per frame from a
  // scrollincandescent draw (debug-path classified): [0] = g_fAnimationTime
  // (VS c9.x), [1] = the material multiplier m_params[0].y (PS c3.y).
  // Consumed by the fam-14 branch (the scrolling LED chyron).
  bool scroll_valid = false;
  float scroll_rows[2] = {};
  // dynamicobject.fx frame-global lighting rows, captured from a
  // dynamicobject/alphatestdynamicobject PS bank (debug-path classified):
  // [0..2] sun direction (PS c9), [3] scene exposure (c13.x),
  // [4..6] flat ambient rgb (c15.xyz), [7] bounce scale (c15.w),
  // [8] material multiplier (c14.y), [9] static world-shadow floor (c8.w).
  // Consumed by the exact dynamicobject scene PS branch.
  bool dynobj_valid = false;
  // [10..12] CSM receive biases c8.x/y/z (finest/mid/outer cascade; 0 =
  // not captured), dynamicobject_defaultPS lines 38/41/44.
  float dynobj_rows[13] = {};
  // dynamicobject.fx static world-shadow transform (PS c5/c6/c7, captured
  // with the rows above): world-space projection rows into the game's
  // 512x512 baked-shade map (uv = dot(wp4, c5/c6) * (+-0.5) + 0.5, ray
  // depth = dot(wp4, c7)). The guest never renders that map in native mode
  // (its memory stays zeroed), so the native path re-renders it from the
  // frame's STATIC world items with these rows (see RenderShadowAtlas) and
  // props sample it exactly like the game's PS: shadow = min(csm * gate,
  // max(world, c8.w)); without it props inside baked building shade lit
  // at full key (the newspaper-machine mint brightening).
  bool dynobj_ws_valid = false;
  float dynobj_ws[12] = {};
  // environmentpark.* frame rows, captured from the park pixel shaders'
  // own banks (see DrawItem::dynobj 4/5/6). They are NOT the dynamicobject
  // rows: those are refreshed only by dynamicobject_defaultPS draws, so a
  // park-only view served stale lighting and the pieces flickered between
  // lit and shadowed frame to frame. The park banks also use a different
  // register layout per variant, so the offsets cannot be shared.
  //
  //   [0..2]  sun direction        (diffuse/decal c13, alphatest c9)
  //   [3]     tone exposure        (c16.x / c17.x / c13.x)
  //   [4..6]  shadow floor A       (c11.xyz; cascade term, model 0 only)
  //   [7]     material multiplier  (c18.y / c21.y / c17.y)
  //   [8..10] shadow floor B       (c12.xyz; world term, model 0 only)
  //           -- model 1 puts the ADDITIVE flat ambient here (c18.xyz)
  //   [11]    output alpha         (c19.x / c22.x / 1)
  //   [12..14] kd tint             (c20.xyz / c23.xyz / 1)
  //   [15]    bounce scale         (model 1 only, c18.w)
  //   [16..19] GetTangentLight kd axes + spec power (c17 / c18 / c14)
  //   [20..22] CSM receive biases, finest cascade first
  //            (c9.x/c10.y/c10.x for diffuse+decal, c8.xyz for alphatest)
  //   [23]    shading model: 0 = floor-lerp (diffuse / decal),
  //           1 = additive ambient (alphatest, the dynamicobject shape)
  // [m] = shading model m's set (0 diffuse / decal, 1 alphatest; see
  // g_park_rows): each piece reads the set of its own variant.
  bool park_valid[2] = {false, false};
  float park_rows[2][24] = {};
  // environmentpark{diffuse,decal}_defaultPS light cookie: the park's baked
  // light (the grey-blue indoor level plus the window sun shafts), a DXT
  // texture every model-0 piece samples as tf1 at a planar projection of its
  // world position and MIN's into its shade (ucode decal 63/90, diffuse
  // 46/67). park_lm = the VS rows (c5, c6: uv = dot(c.xyz, wp) + c.w);
  // park_lm_fetch = the tf1 fetch words (pass-global, identical on every
  // park draw).
  bool park_lm_valid = false;
  float park_lm[8] = {};
  uint32_t park_lm_fetch[6] = {};
  // Deck (skateboard) contact-shadow placement: the board's world matrix
  // from the guest's GetDeckMatrix, re-timed onto the SAME playback clock
  // and boxcar the rendered dynamic items use (InterpolateDynamicItems).
  // Taking the guest's raw latest matrix instead left the decal running
  // ahead of the skater at speed, because the board it belongs to renders
  // from the re-timed pose.
  // First-person anchor belongs to this immutable scene and both eyes.
  bool player_pose_valid = false;
  float player_head[3] = {};
  float player_forward[3] = {};
  float player_chest[3] = {};
  float player_face[3] = {};
  uint32_t player_entity = 0;
  deck_shadow::DeckPlacement decks[deck_shadow::kMaxSkaters] = {};
  // This frame's SimpleDraw triangle list (PublishSimpleDraws; world
  // space), re-timed onto the scene's playback clock when the camera is
  // (RetimeSimpleDraws), carried in the scene so the replay always draws
  // the lines that belong to the scene it renders.
  std::vector<SimpleDrawVert> sdraw_verts;
  // Character-receiver per-cascade CSM receive biases, captured from a
  // validated fam-2 character PIXEL bank (gameplay c8 / editor c9: one row
  // below the light row; frame-global, e.g. (0.005, 0.014, 0.020)). The
  // game's character shaders build their shadow reference as
  // saturate(rd - bias[cascade]) and take NINE point taps of the atlas at
  // +-1 game-texel, averaged (cacstamp_skin_nisPS ucode; literal 1/9 =
  // c255.w). 0 = not captured -> legacy single-tap char path.
  float char_shadow_bias[3] = {};
  // postfx_jitterBlurPS c0.xy from the game's CAS skin blur draw (see
  // DrawItem::sss); 0 = not seen yet.
  float skin_blur_c0[2] = {};
  // The game ran its world-shade pass this frame (ws_draws draws, of which
  // ws_matched landed on published items flagged ws_caster).
  uint32_t ws_draws = 0;
  uint32_t ws_matched = 0;
  // The world-shade pass's own projection rows (its VS c0/c1/c2): the rows
  // the map is rendered with. dynobj_ws holds the RECEIVERS' rows (their PS
  // c5/c6/c7), which the map is sampled with, as in the game.
  float ws_pass_rows[12] = {};
  // The game resolved its world-shade pass into the tf1 texture this frame
  // (see OnResolve): the native map is rebuilt from this frame's pass.
  bool ws_resolved = false;
  // World-shade casters the game's world-shade VIEW lists that the camera
  // view culled (a ceiling above the skater, out of frame): drawn only into
  // the world-shade map, never by the main / cascade passes.
  std::vector<DrawItem> ws_items;
  // The game's skate.Park world-shadow map (tf2 of environmentpark
  // {diffuse,decal}_defaultPS; see ParkWsDraw in skate3_native_scene_state.h):
  // the piece instances its two passes drew this frame, each with the mesh
  // to draw, the pass's own world matrix and the tiles it went into
  // (bit 0 = inner, bit 1 = outer).
  struct ParkWsCaster {
    uint32_t mesh = 0;
    std::vector<DrawEntry> draws;
    float world[16] = {};
    uint8_t tiles = 0;
    // environmentpark.alphatest pieces: their diffuse (alpha cutout; see
    // environmentparkalphatest_shadowPS, which kills a < 0.12). 0 = opaque.
    uint32_t clip_tex = 0;
  };
  std::vector<ParkWsCaster> park_ws_casters;
  // Rebuild the native map this frame (the game resolved into tf2 or drew
  // its passes); otherwise it keeps its contents, like the game's texture.
  bool park_ws_build = false;
  // Each tile's projection rows as the game drew it (VS c0 / c1 / c2).
  float park_ws_pass_rows[2][12] = {};
  bool park_ws_tile_have[2] = {false, false};
  // The receivers' sampling constants: PS c5, c6, c7, c8, c10.
  float park_ws_recv[20] = {};
  bool park_ws_recv_valid = false;
  // The `_skatepark` world receivers' c11.xyz / c12.xyz / c9.x (as [0..2] /
  // [4..6] / [8]; see g_park_dmo_rows) and whether any were captured.
  float park_dmo_rows[12] = {};
  bool park_dmo_valid = false;
  uint32_t park_dmo_items = 0;
  uint32_t park_ws_draws = 0;
  uint32_t park_ws_matched = 0;
  // Sky-dome viewpos: sky.* meshes are camera-relative (sky.fx VS adds
  // g_vViewPos), but the game pins the sky viewpos Y at a fixed level
  // elevation (165.0 in every capture) so the skyline doesn't bob with the
  // skater. Captured per frame from the sky draw's VS bank; the default
  // covers frames before the first capture.
  float sky_height = 165.0f;
  // Sky sun rows, captured together with sky_height from the sky draw's
  // PIXEL bank (sky_defaultPS layout): [0..2] = g_vLightDir (unit vector
  // toward the sun), [3] = sun angular scale (m_params[0].x),
  // [4] = sky pre-tone multiplier (m_params[0].y), [5] = scene exposure
  // (g_envattributes[2].x). Consumed by the exact sky scene-PS branch;
  // the emulated frame's big sun glow is computed IN the sky dome shader
  // from a 1D radial gradient (the sky material's `specular` channel),
  // not by a separate draw.
  bool sky_sun_valid = false;
  float sky_sun[6] = {};
  // Fullscreen UI background blur: 0 = off; otherwise the kernel scale of
  // the game's blur_hBlur/vBlur + basictex pass chain (PS c0.x, 8 in every
  // capture), detected per frame from the blur_hBlurPS draw. The native
  // replica runs after the MSAA resolve, before the 2D overlay draws.
  float ui_blur = 0.0f;
  // Blur modulate color (the blur passes' PS c1; both blur ucodes end in
  // `mul oC0, r0, c1`, so it applies per pass): (1,1,1) under gameplay
  // popups, the dark menu fade in the pause menu; its square is the
  // darkened pause backdrop.
  float ui_blur_color[3] = {1.0f, 1.0f, 1.0f};
  // The popup blur is the MASKED variant (blur_hBlurMaskedPS +
  // blur_vBlurMaskedPS, the pick-an-object menu): horizontal blur only (the
  // vertical pass is a plain copy) and the foreground preview object's
  // pixels keep the sharp, untinted frame.
  bool ui_blur_masked = false;
  // Selection-outline color (postfx_edgedetectstencilPS c0; the park-editor
  // blue in every capture), refreshed from the guest edge-detect draw when
  // it runs. Consumed by the outline composite when any item is selected.
  float outline_color[4] = {0.21569f, 0.64706f, 1.0f, 1.0f};
  // Photo-editor postfx chain (photo_fx.hlsl, exact ucode ports): the live
  // PS/VS constant rows of the game's own postfx passes, captured per frame
  // at the SetPending_AluConstants hook while a photo-mission photo editor
  // is up, plus the fetch words of the two static input textures the chain
  // samples (the 512x2 vignette gradient and the grain texture). Pass order:
  // visualfx, dof downsample, tap9dofMotionBlur, tap9dof, uber, fisheye.
  struct PhotoFx {
    bool valid = false;
    // The game's full cinematic chain (postfx_uberPS instead of
    // postfx_fast_uberPS) on a GAMEPLAY frame: bails / replays run the same
    // visualfx -> dof -> uber -> fisheye passes as the photo editor, with
    // visualfx's tf3 = the frame's bloom (bloom_dof chain result) and the
    // bloom threshold's colour matrix giving the bail its blue grade. The
    // renderer feeds the native bloom into the chain's accumulation input.
    bool gameplay = false;
    float ps[6][32][4] = {};
    float vs[6][8][4] = {};
    uint32_t vignette_fetch[6] = {};
    uint32_t grain_fetch[6] = {};
  };
  PhotoFx photo_fx;
  // The game's final fisheye-family postfx pass, valid while the game draws
  // it (native rendering suppresses it): the camera-man / filmer lens
  // (postfx_basictex_fisheyePS / _opaquePS: warp + vignette gradient + tint)
  // or the respawn "TV noise" (postfx_basictex_fisheye_noisePS: the same
  // plus c3 noise scroll, c4 scene fade / noise amount, c5/c6 noise dot
  // rows). Ported in photo_fx.hlsl. PS c0..c7, the quad VS rows, and the
  // fetch words of the vignette gradient (tf2) and the noise texture (tf5).
  struct LensFx {
    enum Kind { kNoise = 0, kFisheye = 1, kFisheyeOpaque = 2 };
    bool valid = false;
    int kind = kNoise;
    float ps[8][4] = {};
    float vs[8][4] = {};
    uint32_t vignette_fetch[6] = {};
    uint32_t noise_fetch[6] = {};
  };
  LensFx lens_fx;
  // The game's bloom chain (see g_game_bloom_rows): valid while the game
  // runs it. PS rows c0..c10 per pass, from its own draws:
  //   kThreshold  bloom_dof_tap4_minusthresholdPS  1152x640 -> 288x160
  //   kDownB      bloom_dof_tap4                   288x160  -> 72x40
  //   kBlurAH     bloom_dof_tap9PS (horizontal)     288x160
  //   kBlurBH     bloom_dof_tap9PS (horizontal)     72x40
  //   kBlurBV     bloom_dof_tap9PS (vertical)       72x40
  //   kCombine    bloom_dof_tap9_combine3PS         288x160 (A + B)
  //   kGhost      bloom_dof_bloom_ghostPS           288x160 (lens flare)
  // Ported in photo_fx.hlsl (ps_gb_*), run in the HDR post pass.
  struct GameBloom {
    enum Pass { kThreshold = 0, kDownB, kBlurAH, kBlurBH, kBlurBV, kCombine, kGhost };
    bool valid = false;
    // Bit per Pass the game drew this frame. The game skips the tap9 blurs
    // for stretches (combine weights (1, 1) then); a skipped pass passes
    // its input through.
    uint32_t passes = 0;
    float rows[7][11][4] = {};
  };
  GameBloom game_bloom;
  std::vector<DrawItem> items;
  // MeshContexts the build-side occlusion skip left out of `items` this
  // frame (see skate3_native_render_scene_occlusion_cull_build): the
  // render thread re-publishes them as culled so the guest dispatch filter
  // keeps excluding them between their staggered rebuild frames.
  std::vector<uint32_t> occl_build_skipped;
};

// PhotoEffects portrait pass (create-a-skater profile pictures), captured
// on the guest thread and rendered natively at the game's own resolve of
// the photo target (NativeResolveJobHooks): VIPresentation ->
// PhotoEffects::GeneratePhoto -> Apply draws the skater with the photo
// camera over a colour clear into the 1152x640 screenshot target and
// resolves it; ExtractTextureImageData then CPU-reads and crops it.
struct PhotoJob {
  uint32_t dest_phys = 0;  // the resolve destination (physical)
  uint32_t dest_tex = 0;   // the guest texture object resolved into
  std::shared_ptr<const FrameScene> scene;
  int64_t created_ms = 0;
};
// Command processor thread: take the queued job for this resolve.
bool TakePhotoJob(uint32_t dest_phys, PhotoJob* out);
// PhotoEffects::Apply scope (guest thread, from the hook layer).
void OnPhotoApply(bool enter);

// One frame submission record from the hook layer (see RenderMeshRecord).
struct SubmitRecord {
  uint32_t kind;
  uint32_t a;
  uint32_t b;
  uint32_t c;
};

// Graphics build-up showcase layer registry: the order-string tokens
// (cvar skate3_native_render_scene_showcase_order), the per-layer shader
// mask bits (contract in scene.hlsl ShowcaseMask: the split rows carry
// 256 + the cumulative mask, 0 = showcase off) and the display labels.
// Shared by the sequencer (skate3_native_scene_gpu.cpp) and the F12 setup
// window (skate3_native_debug_dialog.cpp). The material bits are
// progressive looks (materials subsumes lighting subsumes albedo); the
// rest are independent and can reveal in any order or grouping.
// The decals and dyn layers are SUBTRACTIVE: their content is part of the
// normal frame and the layer hides it until revealed. When such a layer is
// left out of the order (or '-'disabled), the sequencer folds its bit into
// every step so the run matches the non-showcase frame from clay onward.
// The decal/grime composite only renders on the full material look, so the
// decals layer reads best placed after materials.
// Bit 1024 (blackout) is NOT an orderable layer: the sequencer brackets
// every run with it (snap to black at start, wipe to black at the end) as
// the recording cut marker.
struct ShowcaseLayer {
  const char* token;
  uint32_t bit;
  const char* label;
};
inline constexpr ShowcaseLayer kShowcaseLayers[] = {
    {"albedo", 1u, "albedo textures"},
    {"lighting", 2u, "baked lighting"},
    {"materials", 4u, "materials & surface detail"},
    {"decals", 256u, "decals & grime"},
    {"dyn", 512u, "dynamic entities"},
    {"shadows", 8u, "dynamic shadows"},
    {"ao", 16u, "ambient occlusion"},
    {"ssr", 32u, "reflections"},
    {"vol", 64u, "volumetrics"},
    {"bloom", 128u, "bloom"},
};
inline constexpr size_t kShowcaseLayerCount =
    sizeof(kShowcaseLayers) / sizeof(kShowcaseLayers[0]);
// Bits of the subtractive layers (see above).
inline constexpr uint32_t kShowcaseSubtractiveMask = 256u | 512u;
inline constexpr const char* kShowcaseOrderDefault =
    "albedo,lighting,materials,decals,dyn,shadows+ao,ssr,vol+bloom";

bool Enabled();

// Host settings-overlay backdrop: while the SDK settings menu is open, run
// the game's own popup blur chain (FrameScene::ui_blur) over every finished
// frame; the game issues no popup blur of its own then. The overlay draws
// its dark tint on top of the blurred scene.
void SetSettingsMenuBlur(bool enabled);

// The captured (un-overridden) sun direction, for the debug dialog's sun
// override to seed its sliders from (unit vector toward the sun).
void GetCapturedSunDir(float out[3]);

// While the drone/free-fly camera is engaged, fills out_pos with the flown
// camera world position and returns true (false = guest camera in control).
// Consumed by the draw-distance hooks to recenter distance culls on the
// drone; the render camera itself is taken over via the ViewCamera::
// SetViewMatrix override in skate3_native_scene.cpp.
bool FreecamGuestPose(float out_pos[3]);

// True while the guest sits on a loading screen or in the frontend: the
// presence context is out of gameplay AND the world has stopped publishing
// perspective scenes. The in-game pause menu keeps publishing behind the
// menu and reports false. Mirrors the pause-vs-loading distinction in
// YieldForMenus (same 300 ms publish-staleness window). With scene capture
// disabled entirely, publishes never arrive and every out-of-gameplay
// context reports true (the conservative side).
bool LoadingOrFrontendActive();

// The game's per-entity spawn/streaming fade as carried by the item's
// validated character-lighting capture (peds c21.x, defaultcharacter c13.x,
// cacstamp c22.x, vehicle body c20.x, hair strand scale). 1.0 for items
// without a validated capture. Defined in skate3_native_scene.cpp; consumed
// by the render passes.
float CharFadeAlpha(const DrawItem& item);

// True when all 8 corners of the item's world-space bbox fall outside one
// clip plane of `vp` (row-vector view*proj). `margin` scales the tested
// frustum: < 1 shrinks it, > 1 widens it. Defined in
// skate3_native_scene.cpp; consumed by the off-screen retention pass and the
// per-item profiling attribution.
bool ItemOutsideFrustum(const DrawItem& it, const float vp[16], float margin);

// Occlusion-cull handoff for the guest-side dispatch filter (defined in
// skate3_native_scene_gpu.cpp): copies the most recent render frame's
// culled-static MeshContext set (sorted ascending) into `out`, clearing it
// instead when the last publish is older than a quarter second (native
// render idle: menus, loading). Returns true when `out` is non-empty.
bool CopyOcclusionCulledCtxs(std::vector<uint32_t>& out);
// Counts sort-list entries the guest-side filter kept away from the guest
// dispatcher (frame-stats telemetry).
void AddGuestOcclSkipped(uint32_t n);
// The guest's main camera SceneRenderView, as BuildFrameScene last picked it
// (0 = none yet).
inline std::atomic<uint32_t> g_main_scene_view{0};
// True when the guest's z-prepass of `view` (sub_827FAB50) can be skipped:
// skate3_native_render_scene_skip_guest_zprepass, the native scene on and
// drawing the world (a rendered frame within 250 ms), and `view` the main
// camera view. Counts the skips for the perf line. Guest render thread.
bool SkipGuestZPrepass(uint32_t view);
// The sort list (offset into its SceneRenderView) the guest draw-list
// dispatcher is drawing right now, 0 outside a dispatch (draws-by-pass
// diagnostic). Guest render thread only.
inline uint32_t g_dispatch_list_offset = 0;

// Called from the cProcessArenaAsset::RegisterTexture hook: guid -> guest
// renderengine::Texture object.
void OnRegisterTexture(uint64_t guid, uint32_t texture);

// Queue one mesh for the prewarm decode workers (guest thread; deduped per
// load).
void OnMeshRegistered(uint8_t* base, uint32_t mesh);

// Called (post-call) from the pegasus::tRModelData::Fixup hook: the
// rw-arena LOAD-time pointer resolve, fired per model while its arena
// streams in. The EARLY prewarm source: it spreads mesh/texture decoding
// across the load's whole disk-streaming phase instead of the
// final-seconds activation burst. (The `Unfix` atoms are the save/size
// path and never run at load.)
void OnModelFixup(uint8_t* base, uint32_t model);

// Called (post-call) from the WorldPresentation::AddRenderInstance hook:
// the world registry add that fires per placed instance during the load's
// final ACTIVATION phase. Walks tInstance -> tRModelData -> mesh table
// (self-validating offsets) and queues every optimesh-form mesh: the
// completeness backstop behind OnModelFixup.
void OnAddRenderInstance(uint8_t* base, uint32_t instance);

// Called from the D3D::SetPending_AluConstants hook (guest render thread,
// invoked from inside the Draw* functions). Records the location of the
// device's positional 256-register VS constant shadow bank.
void OnVsConstantUpload(uint8_t* base, uint64_t mask, uint32_t bank, uint32_t ptr,
                        uint32_t device);

// Called (per guest frame) from the Sk8::Challenge::PhotoReplayController::
// Update hook while a photo-mission's photo editor is up. Heartbeat for the
// photo-editor yield: the editor's depth-of-field / saturation / brightness
// / contrast effects are the game's own postfx chain, which only the
// emulated path executes.
void OnPhotoReplayUpdate();
// Called from the Sk8::FE::FrontEndState_Replay2::TakePhoto hook: a photo
// was just taken (replay editor / photo mission). Arms the photo-grab
// forced-readback window for the frames that render and CPU-grab the
// screenshot target.
void OnTakePhoto();
// The game's screenshot execution (ScreenshotBackEnd, guest thread) is about
// to CPU-read its targets: write the newest native frame into them now,
// waiting briefly for its GPU copy. False if none is available.
// Returns 1 written; -1 no readback pair yet, -2 nothing produced,
// -3 GPU copy timed out, -4 targets unresolvable.
int SyncWriteNewestGrab(uint8_t* base);
// CACPresEntity::GenerateSnapShotJPEG is about to read `texture` (the
// finished post-processed scene target, 0x04828000): write the newest
// native frame into it (no wait: the CPU copy of the last completed
// readback). 1 written, <= 0 not written.
int WriteNativeFrameToTexture(uint8_t* base, uint32_t texture);
// DIAGNOSTIC: save a guest k_8_8_8_8 texture's memory (untiled) as a PPM.
void DumpGuestTextureForDiag(uint8_t* base, uint32_t texture, const char* path);
// DIAGNOSTIC: log the draws/resolves of the last few guest frames (rolling
// record kept by OnDrawDone / OnResolve), tagged with `why`.
void DumpRecentPasses(uint8_t* base, const char* why);
// DIAGNOSTIC OnDrawDone section profiler (guest render thread): enable per
// frame; DrawSectionShares returns each section's share of the hook's cycles
// since the last call (" name=NN%" ...) and resets.
void EnableDrawSectProfiling(bool on);
std::string DrawSectionShares(bool enable_next);
// DIAGNOSTIC BuildFrameScene phase shares since the last call (resets).
std::string BuildSectionShares();
// DIAGNOSTIC: a labelled marker in the pass ring (label must be a literal).
void RecordPassMarker(const char* label, uint32_t a, uint32_t b, uint32_t lr);
// DIAGNOSTIC: log the VS rows of the next perspective 3D draw.
void DiagArmCameraLog();
// ScreenshotBackEnd grab REQUEST (fires 1-2 frames BEFORE the game renders
// the shot + card-composite frame sequence): arms the shutter burst.
void OnPhotoGrabRequest();
// GrabScreenshot COMPLETED (post-call): every card-build read from here on
// is CPU-side of already-copied memory, closes the shutter burst.
void OnPhotoGrabDone();
// True while the photo display-card quad is in the live 2D stream during an
// armed photo flow; the compose auto-trace trigger keys on it.
bool PhotoCardVisible();
void OnMovieDecode();
void OnMovieFrame(uint8_t* base, uint32_t renderer);
// Newest native auto-exposure measurement: the 32x16 luminance plane (row
// major, 0..255 = the game target's alpha byte) rebuilt from the finished
// native frame, and when it was published (steady_clock ns). False until the
// first readback completes. Consumed by the PostProcessor evaluator hook.
bool LatestExposureMeasurement(uint8_t out[32 * 16], int64_t* out_ns);

// 2D/APT phase bracket (the HUD and every other 2D element is a Flash SWF
// converted to EA APT, rendered through FE::AptRenderingIntegration). Guest
// draws issued while inside a bracket are 2D draws. bit: 0 =
// FrontEndManager::Render2D, 1 = AptMovieIntegration::Render, 2 =
// AptRenderingIntegration::DrawRenderingUnit, 3 =
// AptRenderingIntegration::UpdateRenderToTexture (in gameplay the whole HUD
// renders inside bit 3 into a screen-sized overlay texture at true screen
// coordinates; replaying those draws directly is the composite), 4 =
// Sk8::Render::cFont::DrawstringLocal (glyph text: trick names/scores;
// text can flush outside the APT brackets), 5 =
// Sk8::Render::SimpleDraw::DrawParameters::Draw (immediate-mode quads:
// chase arrows, in-world markers/beams).
void On2dPhase(uint32_t bit, bool enter);

// Current guest shader objects (D3DDevice_SetPixelShader/SetVertexShader),
// recorded per draw so offline analysis can group the 2D draw stream by
// shader variant.
void OnSetShader(bool pixel, uint32_t obj);
// D3DDevice_SetTexture(sampler, texture): records the slot binding.
void OnSetTexture(uint32_t slot, uint32_t tex);

// D3D::SetPending_RenderStates(device, mask, bank, ptr): the render-state
// shadow bank (blend/depth state for the 2D pass lives here).
void OnRenderStateUpload(uint64_t mask, uint32_t bank, uint32_t ptr);
// Aspect (width / height) of the native renderer's current guest output,
// published by RenderScene every native frame (0 = none yet). The frame
// build extends the game's 16:9 camera to it (WideOutputHorScale /
// WideOutputVertScale).
inline std::atomic<float> g_native_output_aspect{0.0f};
// Bumped after every CPU blend-shape morph job (Sk8::Morph_VMX exit hook):
// create-a-skater face/body sliders rewrite the character VB in place, a
// local vertex range the 16-sample payload fingerprint rarely touches.
// Folded into skinned items' fingerprints so every morph re-decodes them.
inline std::atomic<uint64_t> g_morph_epoch{0};

// D3DDevice_SetViewport / SetRenderState_ScissorTestEnable-adjacent scissor
// tracking, recorded per draw (render-to-texture APT passes show up as
// non-fullscreen viewports).
void OnSetViewport(uint8_t* base, uint32_t viewport_ptr);
void OnSetScissor(uint8_t* base, uint32_t rect_ptr);

// Guest buffer-binding trackers + post-draw state fixup: each
// DrawIndexedVertices whose bound IB/VB match a pending item fills that
// item's bone palette / world matrix from the constant bank (FIFO one-shot
// per buffer key; clones share mesh assets and draw in submit order).
void OnSetIndices(uint32_t ib_obj);
// D3DDevice_ClearF(device, flags, rects, D3DVECTOR4* color, z, stencil).
void OnClearF(uint8_t* base, uint32_t device, uint32_t flags, uint32_t color_ptr);
// D3DDevice_Resolve(r3 device, r4 flags, r5 rects, r6 dest texture, ...):
// notes the game's resolve into its world-shade texture.
void OnResolve(uint8_t* base, uint32_t flags, uint32_t dest_tex);
void OnSetStreamSource(uint32_t stream, uint32_t vb_obj, uint32_t offset, uint32_t stride);
// func: 0 = DrawIndexedVertices (also runs the pending fixup),
// 1 = DrawVertices, 2 = BeginVertices (inline/cloth vertex path).
void OnDrawDone(uint8_t* base, uint32_t func, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7);
// D3DDevice_BeginIndexedVertices (SimpleDraw capture, see SimpleDrawCap).
// iaddr / vaddr: the guest index / vertex write pointers the call returned.
void OnInlineIndexedDraw(uint8_t* base, uint32_t prim, uint32_t num_verts,
                         uint32_t index_count, uint32_t stride, uint32_t iaddr,
                         uint32_t vaddr);

// Offline-analysis recording, armed alongside the guest-memory snapshot:
// captures every draw's bound buffers + full VS constant bank, plus the
// per-frame captured items and final scene, every `stride` frames. Written
// as <stem>.scene.jsonl and <stem>.draws.bin next to the .gsnap.
void StartRecording(uint32_t stride);
void WriteRecording(const char* dir, const char* stem);

// Called post-call from the DrawVertices hook for cloth patch draws
// (r6 == 0x80000000 signature): reads the bound dynamic buffer's vertex
// fetch block and builds a world-space quad-list item. Returns a
// dynamic-state index (+1), 0 when the draw is not a cloth patch;
// *out_key receives the garment's stable identity (the buffer object).
uint32_t CaptureClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6,
                          uint32_t r7, uint32_t* out_key);

// Total guest draws completed (any draw function). The RenderMesh hook
// samples this around the original call to detect deferred meshes: if no
// draw ran inside, the constant bank still belongs to an earlier mesh and
// the item's transform/palette must come from the post-draw fixup.
uint64_t DrawSequence();

// Called from the RenderMesh hook (dynamic entities) and the world per-mesh
// draw hook (sub_827FAEA8), AFTER the original call returns (the mesh's
// constants are only flushed to the shadow bank by its own draws). Builds
// the complete DrawItem: geometry from the transient per-frame arena plus
// world matrix / bone palette read live from the shadow bank. drew_inside
// says whether any guest draw ran during the original call; when false the
// bank is stale and the transform is left pending for the post-draw fixup.
// With world_path, bails out early for meshes that need no per-draw
// transform (absolute fmt-57 geometry, handled at frame end) and captures
// only skinned meshes and rigid model-space props (vending machines and
// other movables reach the frame ONLY through the sort lists). Returns a
// dynamic-state index (+1), 0 on failure/skip.
// The world-shade pass's render function (retail sub_827FD170, dev
// 0x8297B5B0; WorldShadowView vtable slot 10): `owner` + 20200 / + 20204
// (dev + 20216 / + 20220) is the MeshContext* vector it renders (dev
// verified F11: 81 ctxs = the pass's 81 distinct buffers).
void OnWorldShadowRender(uint8_t* base, uint32_t owner);

uint32_t CaptureDynamicState(uint8_t* base, uint32_t ctx, bool world_path = false,
                             bool drew_inside = false);

// Called on the guest render thread at frame end with the frame's records.
void BuildFrameScene(uint8_t* base, const SubmitRecord* records, size_t count);

// Registers the native guest output renderer with the SDK. Safe to call once
// at startup regardless of cvar state.
void Install();

// Runtime renderer switch (bound to F5): flips skate3_native_render_scene
// live. Capture hooks, scene build, guest-output replacement and
// emulated-draw suppression all re-check the cvar every frame, so the swap
// between the native renderer and the emulated GPU is seamless. Returns the
// new state; refuses (returns false) when the skate3_native_render hook
// layer was not enabled at boot.
bool ToggleSceneEnabled();

// True once the scene renderer has hit an unrecoverable pipeline/resource
// failure and yields to the emulated output until re-enabled. Surfaced so
// the render-mode indicator can show the degraded state even though the
// renderer cvars are still enabled.
bool SceneFailed();

// Clears the sticky failure so the next frame retries the full pipeline
// build (ToggleSceneEnabled calls this on re-enable: a transient allocation
// failure can succeed on retry; a persistent one simply re-latches).
void ResetSceneFailure();

// Debug-dialog cache flushes (F12 native-render debug menu): retire every
// cached GPU texture / mesh decode so hot-toggled decode settings (mip
// chains, revalidation, ...) rebuild immediately.
void FlushTextureCache();
void FlushMeshCache();

// Camera-signal recorder (judder diagnosis): for `seconds`, records every
// distinct guest camera pose (1 kHz sampler timestamps) plus the per-frame
// raw and smoothed headings, then writes logs/cam_signal_<ts>.csv. Pan the
// camera with the stick at a steady rate while it runs; offline analysis
// shows whether the game's own pose sequence is irregular at the source.
void RecordCameraSignal(double seconds);

// F7: dump the rolling per-frame scene-composition ring (one compact
// signature per item per frame for the last ~900 frames) to
// logs/scene_ring_<ts>.csv on the render thread's next frame. Diffing the
// artifact frame against its neighbors names exactly which item appeared /
// vanished / changed textures during a 1-2 frame flash no capture can catch.
void RequestSceneRingDump();

// Bone-signal recorder (wheel-guard diagnosis): for `seconds`, records every
// raw entity pose (ring pushes: bone palettes / rigid worlds, timestamps)
// and each frame's interpolated output, then writes
// logs/bone_signal_<ts>.bin. Skate at a steady speed while it runs;
// offline analysis replays candidate guard strategies against the real
// board data.
void RecordBoneSignal(double seconds);

}  // namespace skate3::native_scene
