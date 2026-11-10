// Mods > Skater proportions: a skater keeps the bone lengths his own part models are bound
// with, instead of every skater taking the shared skeleton's.
//
// HOW THE GAME POSES A SKATER. Animations are offsets on a reference pose: a bone's local
// translation = reference.t + reference.q * (clip.t) (Andale "clip on pose"). Every skater's
// SkaterAnim::Initialize (dev 0x82E47230, retail 0x82B97E38) fetches the same reference,
// PlaybackData::GetPose("rig_Tpose") (retail 0x82D196A8), so all skaters share its bone
// lengths. The part models carry their own: SkaterPresEntity::InitSkinning (dev 0x8291B600,
// retail sub_82782178) fills a per-skater inverse-bind-pose buffer from them through
// FillInvBPBuffer (dev 0x829318B8, retail sub_827A5488: every hierarchy bone set to identity,
// then each model bone's inverse bind written at its hierarchy index, looked up by name).
//   retail SkaterPresEntity: +832 inverse bind buffer (Matrix44 per hierarchy bone),
//                            +912 HierarchyData* (u16 bones @0, u32 parts @4, s32 parents
//                                 @8, parts {name[24], count, first} @8 + 8 * bones)
//   (dev +976 / +1056; retail SkaterPresEntity fields sit 144 bytes lower here)
//
// WHAT THIS DOES. Like the miniskaters cheat (retail sub_8278E270, which scales the hips height
// and per-bone scales in each skater's LOCAL pose), it works on the local pose, right after the
// presentation samples it (sub_8278C9E8, before MoveRootToHips_Individual): everything after
// it - the root placement, the board, the skin matrices - follows natively. Per bone:
//     own offset c = row 3 of (bind_i x bind_parent^-1), bind = inverse of the buffer entry
//     reference r  = SkaterPresentationDB's RIG_TPOSE local translation, read once from the
//                    game's data/big/miscload.big (EB v3 entry, RefPack): the database holds
//                    the skeleton (SK83_TPOSE) and RIG_TPOSE (RD). It is not reachable from game
//                    memory: the skater animation fetches the body database's rig_Tpose (VBR,
//                    36 bones) and the two hand poses, not this one (measured in-game)
//     local.t += c - r
// then, like the cheat, the hips' place is scaled about the root by his size (own hips height /
// the reference's), so strides and stance shrink with him; the feet go to the animation's foot
// positions scaled the same way, but at the animation's heights up to deck level (soles stay on
// the ground and the board), each leg bent onto them by a two-bone IK in the plane the animation
// bent it in (skate3_skater_proportions_pose.h; the hips go lower only if a foot is out of
// reach). Planting the full-size foot positions instead made short legs lunge to reach them.
// Only bones whose offset differs by more than 5 mm move: the retail skaters' parts are bound
// in the reference pose (their bones match it to ~1.5 mm), so they are untouched. Bones no part
// binds keep the reference. A skater whose pose does not come through that sampler gets the
// same adjustment on the skin matrices UpdateBoneTransforms places (a copy in two guest buffers
// per skater, alternated per frame). Off while the miniskaters cheat is on.

#include "generated/skate3_init.h"
#include "skate3_skater_proportions.h"
#include "skate3_skater_proportions_pose.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(skate3_mod_skater_proportions, false, "Mods",
                    "Skaters keep the bone lengths of their own models (custom skaters with other "
                    "proportions). Retail skaters are unchanged.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3 {
const std::filesystem::path& GameDataRoot();   // skate3_app_common.cpp
}

namespace {

constexpr uint32_t kEntityInvBind = 832;
constexpr uint32_t kEntityHierarchy = 912;
constexpr uint32_t kMaxBones = 256;
constexpr float kMinDelta = 0.005f;          // metres
// record-name words of "RIG_TPOSE" (FastString encoding)
constexpr uint32_t kRigTposeWord0 = 0x86A87FCAu;
constexpr uint32_t kRigTposeWord1 = 0x79B90F58u;
constexpr uint32_t kHtPose = 3;
constexpr uint32_t kHtHierarchy = 4;
constexpr uint32_t kCodecRd = 0x4452u;

using Mat = std::array<float, 16>;            // row-vector RW layout: rows 0-2 axes, row 3 position

float LoadF(uint8_t* base, uint32_t addr) { return std::bit_cast<float>(REX_LOAD_U32(addr)); }
void StoreF(uint8_t* base, uint32_t addr, float v) { REX_STORE_U32(addr, std::bit_cast<uint32_t>(v)); }

Mat LoadMat(uint8_t* base, uint32_t addr) {
  Mat m;
  for (int i = 0; i < 16; ++i) m[i] = LoadF(base, addr + uint32_t(i) * 4);
  return m;
}

Mat Mul(const Mat& a, const Mat& b) {        // a x b, row vectors
  Mat r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  return r;
}

bool InverseAffine(const Mat& m, Mat& out) { // rows 0-2 a 3x3 (scale allowed), row 3 translation
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
  const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const float det = a * A + b * B + c * C;
  if (std::fabs(det) < 1e-12f) return false;
  const float inv = 1.0f / det;
  out = {A * inv, -(b * i - c * h) * inv, (b * f - c * e) * inv, 0.0f,
         B * inv, (a * i - c * g) * inv, -(a * f - c * d) * inv, 0.0f,
         C * inv, -(a * h - b * g) * inv, (a * e - b * d) * inv, 0.0f,
         0.0f, 0.0f, 0.0f, 1.0f};
  for (int j = 0; j < 3; ++j)
    out[12 + j] = -(m[12] * out[j] + m[13] * out[4 + j] + m[14] * out[8 + j]);
  return true;
}

bool IsIdentity(const Mat& m) {
  for (int i = 0; i < 16; ++i)
    if (m[i] != ((i % 5 == 0) ? 1.0f : 0.0f)) return false;
  return true;
}

struct Hierarchy {
  uint32_t bones = 0;
  std::vector<std::array<uint32_t, 2>> names;  // first two name words per bone (FastString)
  std::vector<int> parents;
  std::vector<int> order;                     // parents before children
  std::vector<std::pair<uint32_t, uint32_t>> parts;  // (first, count) by part index
  std::vector<std::string> part_names;        // TRAJECTORY, TORSO ... (name[24] of each part)
};

// Guest memory [addr, addr + size) mapped and readable. A skater entity freed (or its memory reused)
// between the game's update and ours leaves stale pointers in it: reading through them faulted the
// host (access violation in ReadHierarchy, Nov 2026 crash reports at skate3.exe+0x175F626/646).
bool GuestReadable(uint32_t addr, uint32_t size) {
  auto* mem = rex::system::kernel_memory();
  if (!mem || addr < 0x1000 || size == 0 || addr + size < addr) return false;
  for (uint32_t a : {addr, addr + size - 1}) {
    auto* heap = mem->LookupHeap(a);
    uint32_t prot = 0;
    if (!heap || !heap->QueryProtect(a, &prot) || !(prot & rex::memory::kMemoryProtectRead)) return false;
  }
  return true;
}

bool ReadHierarchy(uint8_t* base, uint32_t h, Hierarchy& out) {
  if (!GuestReadable(h, 8)) return false;
  const uint32_t nb = REX_LOAD_U16(h);
  const bool trajectory = REX_LOAD_U16(h + 2) != 0;
  const uint32_t np = REX_LOAD_U32(h + 4);
  if (nb == 0 || nb > kMaxBones || np == 0 || np > 64) return false;
  if (!GuestReadable(h, 8 + 8 * nb + 36 * np + 20 * nb)) return false;
  out.bones = nb;
  out.parents.resize(nb);
  for (uint32_t i = 0; i < nb; ++i) {
    const int32_t p = int32_t(REX_LOAD_U32(h + 8 + i * 4));
    out.parents[i] = (p >= 0 && uint32_t(p) < nb && uint32_t(p) != i) ? p : -1;
    if (trajectory && i != 0 && out.parents[i] < 0) out.parents[i] = 0;      // under the root
  }
  out.parts.clear();
  out.part_names.clear();
  for (uint32_t k = 0; k < np; ++k) {
    const uint32_t r = h + 8 + 8 * nb + 36 * k;
    const uint32_t cnt = REX_LOAD_U32(r + 24), first = REX_LOAD_U32(r + 28);
    if (first + cnt > nb) return false;
    out.parts.emplace_back(first, cnt);
    std::string name;
    for (uint32_t c = 0; c < 24; ++c) {
      const char ch = char(REX_LOAD_U8(r + c));
      if (ch == 0) break;
      name.push_back(ch);
    }
    out.part_names.push_back(name);
  }
  // bone names after the parts: 20 bytes (5 FastString words) per bone
  out.names.resize(nb);
  const uint32_t names = h + 8 + 8 * nb + 36 * np;
  for (uint32_t i = 0; i < nb; ++i) out.names[i] = {REX_LOAD_U32(names + 20 * i), REX_LOAD_U32(names + 20 * i + 4)};
  std::vector<int> state(nb, 0);
  out.order.clear();
  for (uint32_t s = 0; s < nb; ++s) {
    std::vector<uint32_t> chain;
    for (int b = int(s); b >= 0 && state[b] == 0; b = out.parents[b]) {
      state[b] = 1;
      chain.push_back(uint32_t(b));
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) out.order.push_back(int(*it));
  }
  return out.order.size() == nb;
}

// ---- the reference: SkaterPresentationDB's RIG_TPOSE, read from the game's own miscload.big

// Big-endian reads from a host buffer (0 past the end).
struct Bytes {
  const std::vector<uint8_t>& d;
  uint32_t U32(size_t o) const {
    return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0;
  }
  uint16_t U16(size_t o) const { return o + 2 <= d.size() ? uint16_t((d[o] << 8) | d[o + 1]) : 0; }
  float F(size_t o) const { return std::bit_cast<float>(U32(o)); }
};

// RefPack ("10 FB"), as the toolkit's formats/refpack.py (the game's archive compression).
bool RefPackDecompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
  if (n < 5 || src[1] != 0xFB || (src[0] & 0x3E) != 0x10) return false;
  const size_t width = (src[0] & 0x80) ? 4 : 3;
  size_t s = 2 + ((src[0] & 1) ? width : 0);
  size_t size = 0;
  for (size_t k = 0; k < width; ++k) size = (size << 8) | src[s + k];
  s += width;
  out.clear();
  out.reserve(size);
  while (s < n) {
    const uint8_t b0 = src[s];
    size_t lits = 0, length = 0, dist = 0;
    if (b0 < 0x80) {
      if (s + 1 >= n) return false;
      const uint8_t b1 = src[s + 1];
      lits = b0 & 3;
      length = ((b0 >> 2) & 7) + 3;
      dist = ((b0 & 0x60) << 3) + b1 + 1;
      s += 2;
    } else if (b0 < 0xC0) {
      if (s + 2 >= n) return false;
      const uint8_t b1 = src[s + 1], b2 = src[s + 2];
      lits = b1 >> 6;
      length = (b0 & 0x3F) + 4;
      dist = ((b1 & 0x3F) << 8) + b2 + 1;
      s += 3;
    } else if (b0 < 0xE0) {
      if (s + 3 >= n) return false;
      const uint8_t b1 = src[s + 1], b2 = src[s + 2], b3 = src[s + 3];
      lits = b0 & 3;
      length = ((b0 & 0x0C) << 6) + b3 + 5;
      dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
      s += 4;
    } else if (b0 < 0xFC) {
      lits = ((b0 & 0x1F) << 2) + 4;
      s += 1;
    } else {
      lits = b0 & 3;
      if (s + 1 + lits > n) return false;
      out.insert(out.end(), src + s + 1, src + s + 1 + lits);
      break;
    }
    if (s + lits > n) return false;
    out.insert(out.end(), src + s, src + s + lits);
    s += lits;
    if (length) {
      if (dist > out.size()) return false;
      const size_t start = out.size() - dist;
      for (size_t k = 0; k < length; ++k) out.push_back(out[start + k]);
    }
  }
  return out.size() == size;
}

// One file of an EB v3 archive (the Skate 3 .big): entries {u32 offset >> shift, pack size,
// size, djb2 hash of "dir/name"} from 0x30, a method byte per entry after them (0 stored,
// else RefPack).
bool ReadBigEntry(const std::filesystem::path& path, std::string_view name, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> head(0x30);
  if (!f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()))) return false;
  Bytes h{head};
  if (head[0] != 'E' || head[1] != 'B' || h.U16(2) != 3) return false;
  const uint32_t count = h.U32(4);
  const uint32_t shift = head[10];
  if (count == 0 || count > 1000000) return false;
  std::vector<uint8_t> table(size_t(count) * 17);
  if (!f.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()))) return false;
  Bytes t{table};
  uint32_t want = 5381;
  for (char c : name) want = want * 33 + uint8_t(c);
  for (uint32_t i = 0; i < count; ++i) {
    if (t.U32(size_t(i) * 16 + 12) != want) continue;
    const uint64_t off = uint64_t(t.U32(size_t(i) * 16)) << shift;
    const uint32_t pack = t.U32(size_t(i) * 16 + 4), size = t.U32(size_t(i) * 16 + 8);
    const uint8_t method = table[size_t(count) * 16 + i];
    std::vector<uint8_t> raw(method ? pack : size);
    f.seekg(std::streamoff(off));
    if (!f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()))) return false;
    if (!method) {
      out = std::move(raw);
      return true;
    }
    return RefPackDecompress(raw.data(), raw.size(), out) && out.size() == size;
  }
  return false;
}

struct Reference {
  std::vector<int> parents;
  std::vector<std::pair<uint32_t, uint32_t>> parts;   // (first, count) by part index
  std::vector<std::string> part_names;
  std::vector<std::array<float, 3>> t;        // local translation per bone
  std::vector<std::array<float, 4>> q;        // local rotation per bone (x y z w)
};

// RD packed rotation (azimuth 11 / elevation 10 / length 11 bits, w >= 0), as formats/abin.py
std::array<float, 4> QuatDecompress(uint32_t v) {
  constexpr float kPi = 3.14159265358979f;
  const float th = float((v >> 21) & 0x7FFu) * (2.0f * kPi / 2047.0f) - kPi;
  const float ph = float((v >> 11) & 0x3FFu) * (kPi / 1023.0f) - kPi / 2.0f;
  const float sv = float(v & 0x7FFu) / 2047.0f;
  return {sv * std::cos(ph) * std::cos(th), sv * std::cos(ph) * std::sin(th), sv * std::sin(ph),
          std::sqrt(std::max(0.0f, 1.0f - sv * sv))};
}

// The hierarchy and RIG_TPOSE of an .abin (records {u32 size, type, codec, guid, name[6], ...},
// data at +0x30; a DBINFO record first when present). RIG_TPOSE is RD: per part, channel masks and
// frame 0: scale 3 x f32, rotation packed u32 (cq) or 4 x f32, translation 3 x s16 + u16 range (ct)
// or 3 x f32, each only where its mask bit is set.
bool ParseReference(const std::vector<uint8_t>& file, Reference& ref) {
  Bytes b{file};
  size_t p = b.U32(4) == 1 ? 0x30 : 0;
  size_t hier = 0, pose = 0;
  while (p + 0x30 <= file.size()) {
    const uint32_t size = b.U32(p), type = b.U32(p + 4);
    if (size < 0x30) break;
    if (type == kHtHierarchy && !hier) hier = p + 0x30;
    if (type == kHtPose && b.U32(p + 8) == kCodecRd && b.U32(p + 16) == kRigTposeWord0 &&
        b.U32(p + 20) == kRigTposeWord1) {
      pose = p + 0x30;
    }
    p += size;
  }
  if (!hier || !pose) return false;
  const uint32_t nb = b.U16(hier), np = b.U32(hier + 4);
  const bool trajectory = b.U16(hier + 2) != 0;
  if (nb == 0 || nb > kMaxBones || np > 64) return false;
  ref.parents.resize(nb);
  for (uint32_t i = 0; i < nb; ++i) {
    const int32_t q = int32_t(b.U32(hier + 8 + size_t(i) * 4));
    ref.parents[i] = (q >= 0 && uint32_t(q) < nb && uint32_t(q) != i) ? q : -1;
    if (trajectory && i != 0 && ref.parents[i] < 0) ref.parents[i] = 0;      // under the root
  }
  std::vector<std::pair<uint32_t, uint32_t>> parts;
  for (uint32_t k = 0; k < np; ++k) {
    const size_t r = hier + 8 + 8 * size_t(nb) + 36 * size_t(k);
    parts.emplace_back(b.U32(r + 28), b.U32(r + 24));          // first, count
    std::string name;
    for (size_t c = 0; c < 24 && r + c < file.size() && file[r + c] != 0; ++c) name.push_back(char(file[r + c]));
    ref.part_names.push_back(name);
  }
  ref.parts = parts;
  ref.t.assign(nb, {0.0f, 0.0f, 0.0f});
  ref.q.assign(nb, {0.0f, 0.0f, 0.0f, 1.0f});
  const uint32_t n = b.U32(pose);
  if (n == 0 || n > parts.size()) return false;
  for (uint32_t k = 0; k < n; ++k) {
    const size_t part = pose + (b.U32(pose + 4 + 4 * size_t(k)) & 0xFFFFFFu);
    const uint32_t header = b.U32(part);
    const uint32_t pi = (header >> 1) & 0x1Fu, bones = ((header >> 6) & 0x3Fu) + 1;
    if (pi >= parts.size() || parts[pi].first + bones > nb) return false;
    const size_t ch = part + b.U32(part + 8);
    uint32_t masks[6];
    for (int i = 0; i < 6; ++i) masks[i] = b.U32(ch + size_t(i) * 4);
    // {u16 frame size, u8 bones - 1, u8 ct, u8 cq} at +24
    const bool ct = ch + 27 < file.size() && file[ch + 27] != 0, cq = ch + 28 < file.size() && file[ch + 28] != 0;
    size_t q = part + b.U32(part + 12);
    for (uint32_t bone = 0; bone < bones; ++bone) {
      auto bit = [&](int group) { return (masks[group * 2 + (bone >> 5)] >> (bone & 31)) & 1u; };
      if (bit(0)) q += 12;
      std::array<float, 4> rot{0.0f, 0.0f, 0.0f, 1.0f};
      if (bit(1)) {
        if (cq) {
          rot = QuatDecompress(b.U32(q));
          q += 4;
        } else {
          for (int c = 0; c < 4; ++c) rot[c] = b.F(q + size_t(c) * 4);
          q += 16;
        }
      }
      ref.q[parts[pi].first + bone] = rot;
      std::array<float, 3> v{0.0f, 0.0f, 0.0f};
      if (bit(2)) {
        if (ct) {
          const float sc = float(b.U16(q + 6)) / 32768.0f;
          for (int c = 0; c < 3; ++c) v[c] = float(int16_t(b.U16(q + size_t(c) * 2))) * sc;
          q += 8;
        } else {
          for (int c = 0; c < 3; ++c) v[c] = b.F(q + size_t(c) * 4);
          q += 12;
        }
      }
      ref.t[parts[pi].first + bone] = v;
    }
  }
  return true;
}

// Loaded once from <game>/data/big/miscload.big.
const Reference* GameReference() {
  static std::once_flag once;
  static Reference ref;
  static bool ok = false;
  std::call_once(once, [] {
    const auto root = skate3::GameDataRoot();
    for (const char* rel : {"data/big/miscload.big", "data/content/miscload.big"}) {
      std::vector<uint8_t> file;
      const auto path = root / rel;
      if (!std::filesystem::exists(path)) continue;
      if (ReadBigEntry(path, "data/anim/SkaterPresentationDB.abin", file) && ParseReference(file, ref)) {
        ok = true;
        REXLOG_INFO("skater-proportions: reference RIG_TPOSE from {} ({} bones; hips {:.3f} {:.3f} {:.3f})",
                    path.string(), ref.t.size(), ref.t.size() > 1 ? ref.t[1][0] : 0.0f,
                    ref.t.size() > 1 ? ref.t[1][1] : 0.0f, ref.t.size() > 1 ? ref.t[1][2] : 0.0f);
        return;
      }
    }
    REXLOG_WARN("skater-proportions: SkaterPresentationDB not read from {} - the mod does nothing", root.string());
  });
  return ok ? &ref : nullptr;
}

// ---- per skater

namespace P = skate3::skater_proportions::pose;

// first two FastString words of the bone names (the rest are 0 for these)
constexpr std::array<uint32_t, 2> kHips = {0x57758374u, 0x00000000u};
constexpr std::array<uint32_t, 2> kHead = {0x56E95B70u, 0x00000000u};   // formats/abin.name_words("Head")
constexpr std::array<uint32_t, 2> kTruckFront = {0x91441A3Fu, 0x4F20DE2Cu};
constexpr std::array<uint32_t, 2> kTruckBack = {0x91441A3Fu, 0x3A15B91Cu};
constexpr std::array<std::array<uint32_t, 2>, 3> kLeftLeg = {{{0x69D20B9Cu, 0x69D23408u},    // LeftUpLeg
                                                              {0x69D20A3Bu, 0x50498E60u},    // LeftLeg
                                                              {0x69D20961u, 0x79CC5140u}}};  // LeftFoot
constexpr std::array<std::array<uint32_t, 2>, 3> kRightLeg = {{{0x86A814A3u, 0x7D93C02Cu},   // RightUpLeg
                                                               {0x86A8149Au, 0x48F45FB0u},   // RightLeg
                                                               {0x86A81494u, 0x79465A40u}}}; // RightFoot

int FindBone(const Hierarchy& h, const std::array<uint32_t, 2>& name) {
  for (uint32_t i = 0; i < h.bones; ++i)
    if (h.names[i] == name) return int(i);
  return -1;
}

using Clock = std::chrono::steady_clock;

struct SkaterState {
  uint32_t hierarchy = 0;
  Hierarchy hier;
  std::vector<Mat> inv_bind;                  // the buffer contents the deltas were built from
  std::vector<Mat> bind;                      // their inverses (bind world matrices)
  P::Rig rig;                                 // parents, order, deltas, legs
  bool active = false;
  bool logged = false;
  int pose_logs = 0;
  Clock::time_point posed{};                  // last time its local pose was adjusted
  // fallback (a skater whose pose does not come through the presentation's sampler)
  uint32_t scratch[2] = {0, 0};
  uint32_t flip = 0;
  bool logged_fallback = false;
  std::vector<float> ref_len;                 // the reference's bone length (local offset) per bone
  std::vector<bool> board;                    // SKATEBOARD / SKATEBOARD_TRUCKS bones and below
  int matrix_kind = -1;                       // last seen: 0 skin, 1 world, 2 neither (logged on change)
};

struct Seen {                                 // a skater entity, as UpdateBoneTransforms last saw it
  uint32_t index = 0;                         // +868: its skater index in its presentation
  uint32_t callbacks = 0;                     // +872: the object it gets its matrices from
  Clock::time_point when{};
};

std::mutex g_state_mutex;
std::unordered_map<uint32_t, SkaterState> g_states;
std::unordered_map<uint32_t, Seen> g_seen;
std::unordered_map<uint64_t, uint32_t> g_pose_owner;   // (presentation, index) -> entity, logged once
thread_local uint32_t t_entity = 0;

// The per-bone deltas for a skater (own bind offset - reference offset). False: leave it alone.
bool Build(uint8_t* base, uint32_t entity, SkaterState& s) {
  if (!GuestReadable(entity + kEntityHierarchy, 4) || !GuestReadable(entity + kEntityInvBind, 4)) return false;
  const uint32_t h = REX_LOAD_U32(entity + kEntityHierarchy);
  const uint32_t ib = REX_LOAD_U32(entity + kEntityInvBind);
  if (h == 0 || ib == 0 || !GuestReadable(ib, 64)) return false;
  if (h != s.hierarchy) {
    s.hierarchy = h;
    if (!ReadHierarchy(base, h, s.hier)) {
      s.hierarchy = 0;
      return false;
    }
    P::Rig& rig = s.rig;
    rig.parents = s.hier.parents;
    rig.order = s.hier.order;
    rig.hips = FindBone(s.hier, kHips);
    rig.truck_front = FindBone(s.hier, kTruckFront);
    rig.truck_back = FindBone(s.hier, kTruckBack);
    rig.feet = rig.hips >= 0;
    for (int k = 0; k < 3; ++k) {
      rig.legs[0][k] = FindBone(s.hier, kLeftLeg[k]);
      rig.legs[1][k] = FindBone(s.hier, kRightLeg[k]);
      rig.feet = rig.feet && rig.legs[0][k] >= 0 && rig.legs[1][k] >= 0;
    }
    // each leg a direct UpLeg > Leg > Foot chain (the IK writes those three locals)
    for (const auto& leg : rig.legs)
      rig.feet = rig.feet && rig.parents[leg[1]] == leg[0] && rig.parents[leg[2]] == leg[1] &&
                 rig.parents[leg[0]] >= 0;
    REXLOG_INFO("skater-proportions: entity={:08X} hips={} left={},{},{} right={},{},{} -> feet {}", entity,
                rig.hips, rig.legs[0][0], rig.legs[0][1], rig.legs[0][2], rig.legs[1][0], rig.legs[1][1],
                rig.legs[1][2], rig.feet ? "planted" : "NOT planted (leg bones not found)");
    s.inv_bind.clear();
  }
  const uint32_t nb = s.hier.bones;
  if (!GuestReadable(ib, nb * 64)) return false;
  std::vector<Mat> inv(nb);
  for (uint32_t i = 0; i < nb; ++i) inv[i] = LoadMat(base, ib + i * 64);
  if (inv.size() == s.inv_bind.size() && std::memcmp(inv.data(), s.inv_bind.data(), nb * sizeof(Mat)) == 0) {
    return s.active;                          // the same parts as last frame
  }
  s.inv_bind = inv;
  s.active = false;
  s.pose_logs = 0;
  const Reference* ref_data = GameReference();
  if (ref_data == nullptr) return false;
  // the skater's bones -> the reference's. The in-game skaters use SkaterPresentationDB's skeleton
  // itself (bone for bone); other screens use variants of it (the character select / Create-a-Skater
  // skeleton: the board root under the hips instead of the root; cutscene rigs: a 2-bone PROP part
  // shifting everything after it): matched by part name and place in the part, as the game's own
  // animation part matching does. Unmatched bones (PROP) keep their parent's place.
  const Reference& R = *ref_data;
  const uint32_t rn = uint32_t(R.parents.size());
  std::vector<int> map(nb, -1);
  int matched = 0, reparented = 0;
  if (R.parents == s.hier.parents) {
    for (uint32_t i = 0; i < nb; ++i) map[i] = int(i);
    matched = int(nb);
  } else {
    for (size_t k = 0; k < s.hier.parts.size() && k < s.hier.part_names.size(); ++k) {
      for (size_t r = 0; r < R.parts.size() && r < R.part_names.size(); ++r) {
        if (R.part_names[r] != s.hier.part_names[k]) continue;
        const uint32_t n = std::min(s.hier.parts[k].second, R.parts[r].second);
        for (uint32_t j = 0; j < n; ++j) {
          const uint32_t a = s.hier.parts[k].first + j, b = R.parts[r].first + j;
          if (a < nb && b < rn && map[a] < 0) {
            map[a] = int(b);
            ++matched;
          }
        }
        break;
      }
    }
    for (uint32_t i = 0; i < nb; ++i) {
      const int p = s.hier.parents[i];
      if (map[i] >= 0 && p >= 0 && map[p] >= 0 && R.parents[map[i]] != map[p]) ++reparented;
    }
    if (matched < int(nb) * 4 / 5) {
      if (!s.logged) {
        REXLOG_WARN("skater-proportions: entity={:08X}: its skeleton ({} bones) matches SkaterPresentationDB's on "
                    "only {} - left as is", entity, nb, matched);
        s.logged = true;
      }
      return false;
    }
  }
  // the reference rest worlds (reference order), then each skater bone's reference local under ITS
  // parent (a reparented bone: the reference's world offset from that parent)
  auto ref_local = [&](uint32_t j) {
    const auto& q = R.q[j];
    const P::Q qq = {q[0], q[1], q[2], q[3]};
    const P::V3 x = P::QRot(qq, {1, 0, 0}), y = P::QRot(qq, {0, 1, 0}), z = P::QRot(qq, {0, 0, 1});
    return Mat{x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, R.t[j][0], R.t[j][1], R.t[j][2], 1};
  };
  const Mat ident = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::vector<Mat> rw(rn, ident);
  std::vector<uint8_t> rdone(rn, 0);
  for (uint32_t j0 = 0; j0 < rn; ++j0) {
    std::vector<uint32_t> chain;
    for (int j = int(j0); j >= 0 && !rdone[j]; j = R.parents[j]) chain.push_back(uint32_t(j));
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const int p = R.parents[*it];
      rw[*it] = p < 0 ? ref_local(*it) : Mul(ref_local(*it), rw[p]);
      rdone[*it] = 1;
    }
  }
  std::vector<Mat> ew(nb, ident), refL(nb, ident);
  for (int i : s.hier.order) {
    const int p = s.hier.parents[i];
    ew[i] = map[i] >= 0 ? rw[map[i]] : (p >= 0 ? ew[p] : ident);
    Mat pinv;
    refL[i] = (p >= 0 && InverseAffine(ew[p], pinv)) ? Mul(ew[i], pinv) : ew[i];
  }
  if (matched != int(nb) || reparented) {
    REXLOG_INFO("skater-proportions: entity={:08X}: a variant of SkaterPresentationDB's skeleton ({} bones, {} "
                "matched by part, {} under another parent) - matched by part names", entity, nb, matched, reparented);
  }
  std::vector<Mat>& bind = s.bind;
  bind.assign(nb, Mat{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
  std::vector<bool> have(nb, false);
  for (uint32_t i = 0; i < nb; ++i) {
    if (!IsIdentity(inv[i]) && InverseAffine(inv[i], bind[i])) have[i] = true;
  }
  // rest worlds (row vectors: world = local x parent): a bound bone's = its bind; another's = the
  // reference pose's local on its parent's rest world (the animation keeps unbound bones at the
  // reference's offsets, and they need not be bound: Shrek weights nothing to Hips or Neck1).
  // RF = the reference skeleton's own rest worlds.
  auto local_of = [&](int i) { return refL[i]; };
  const Mat identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::vector<Mat> rest(nb, identity), rf(nb, identity);
  for (int i : s.hier.order) {
    const int p = s.hier.parents[i];
    if (p < 0) continue;                      // the root (Trajectory): the origin, never bound
    rf[i] = Mul(local_of(i), rf[p]);
    rest[i] = have[i] ? bind[i] : Mul(local_of(i), rest[p]);
  }
  s.board.assign(nb, false);
  for (size_t k = 0; k < s.hier.parts.size() && k < s.hier.part_names.size(); ++k) {
    if (s.hier.part_names[k] != "SKATEBOARD" && s.hier.part_names[k] != "SKATEBOARD_TRUCKS") continue;
    for (uint32_t j = 0; j < s.hier.parts[k].second; ++j)
      if (s.hier.parts[k].first + j < nb) s.board[s.hier.parts[k].first + j] = true;
  }
  for (int i : s.hier.order)
    if (s.hier.parents[i] >= 0 && s.board[s.hier.parents[i]]) s.board[i] = true;
  s.ref_len.assign(nb, 0.0f);
  for (uint32_t i = 0; i < nb; ++i)
    s.ref_len[i] = std::sqrt(refL[i][12] * refL[i][12] + refL[i][13] * refL[i][13] + refL[i][14] * refL[i][14]);
  s.rig.delta.assign(nb, {0.0f, 0.0f, 0.0f});
  // the face: every bone below Head is posed by the skater's face rig (faces/<name>.abin: a pose
  // built from his own face-bone binds, or the template face's) - its offsets are already his.
  // Adding them again doubled them (Shrek's face blew apart).
  const int head = FindBone(s.hier, kHead);
  std::vector<bool> face(nb, false);
  if (head >= 0) {
    for (int i : s.hier.order) {
      const int p = s.hier.parents[i];
      face[i] = p >= 0 && (p == head || face[p]);
    }
  }
  int moved = 0;
  float worst = 0.0f;
  for (uint32_t i = 0; i < nb; ++i) {
    const int p = s.hier.parents[i];
    if (!have[i] || p < 0 || face[i]) continue;
    Mat pinv;
    if (!InverseAffine(rest[p], pinv)) continue;
    const Mat local = Mul(bind[i], pinv);
    const P::V3 d = {local[12] - refL[i][12], local[13] - refL[i][13], local[14] - refL[i][14]};
    const float len = P::Len(d);
    if (len > kMinDelta) {
      s.rig.delta[i] = d;
      ++moved;
      worst = std::max(worst, len);
    }
  }
  s.active = moved > 0;
  // his size, as the miniskaters cheat's: hip-joint height over the reference's (rest pose; the
  // hips bone itself may be unbound)
  s.rig.size = 1.0f;
  if (s.rig.feet) {
    const float own = 0.5f * (rest[s.rig.legs[0][0]][13] + rest[s.rig.legs[1][0]][13]);
    const float rh = 0.5f * (rf[s.rig.legs[0][0]][13] + rf[s.rig.legs[1][0]][13]);
    if (rh > 0.1f) s.rig.size = std::clamp(own / rh, 0.25f, 4.0f);
  } else if (s.rig.hips >= 0 && rf[s.rig.hips][13] > 0.1f) {
    s.rig.size = std::clamp(rest[s.rig.hips][13] / rf[s.rig.hips][13], 0.25f, 4.0f);
  }
  // rest ankle heights above the ground: own and the reference's
  s.rig.ankle_ref = {0.0f, 0.0f};
  s.rig.ankle_own = {0.0f, 0.0f};
  if (s.rig.feet) {
    for (int k = 0; k < 2; ++k) {
      const int f = s.rig.legs[k][2];
      s.rig.ankle_ref[k] = rf[f][13];
      s.rig.ankle_own[k] = rest[f][13];
    }
  }
  if (!s.logged || s.active) {
    REXLOG_INFO("skater-proportions: entity={:08X} bones={} bound={} moved={} largest={:.3f} m size {:.3f} "
                "ankles {:.3f} {:.3f} m (reference {:.3f} {:.3f})",
                entity, nb, int(std::count(have.begin(), have.end(), true)), moved, worst, s.rig.size,
                s.rig.ankle_own[0], s.rig.ankle_own[1], s.rig.ankle_ref[0], s.rig.ankle_ref[1]);
    s.logged = true;
  }
  return s.active;
}

// rows 0-2 = the bone's axes (no scale), as a quaternion (column rotation = transpose)
P::Q MatToQ(const Mat& m) {
  // R[r][c] = m[c * 4 + r]
  const float r00 = m[0], r11 = m[5], r22 = m[10];
  const float r01 = m[4], r10 = m[1], r02 = m[8], r20 = m[2], r12 = m[9], r21 = m[6];
  const float tr = r00 + r11 + r22;
  P::Q q;
  if (tr > 0.0f) {
    const float s = std::sqrt(tr + 1.0f) * 2.0f;
    q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
  } else if (r00 > r11 && r00 > r22) {
    const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
    q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
  } else if (r11 > r22) {
    const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
    q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
  } else {
    const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
    q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
  }
  return P::QNorm(q);
}

Mat QToMat(const P::Q& q, const P::V3& t) {    // rows = the rotated axes, row 3 = position
  const P::V3 x = P::QRot(q, {1, 0, 0}), y = P::QRot(q, {0, 1, 0}), z = P::QRot(q, {0, 0, 1});
  return {x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, t[0], t[1], t[2], 1};
}

// The miniskaters cheat (0x830B98A0, in the modes it applies to) scales the pose itself.
bool MiniSkaters(uint8_t* base) {
  if (REX_LOAD_U8(0x830B98A0u) == 0) return false;
  const uint32_t mode = REX_LOAD_U32(0x830A7AE8u);
  return mode == 0 || mode == 3 || mode == 4;
}

// The skater entity whose skin a presentation's skater index feeds: the one whose callbacks
// object is that presentation (the skin matrices come from it), with that index.
uint32_t FindEntity(uint32_t pres, uint32_t index) {
  const auto now = Clock::now();
  uint32_t exact = 0, near = 0, near_dist = 0xFFFFFFFFu;
  for (const auto& [entity, seen] : g_seen) {
    if (seen.index != index || now - seen.when > std::chrono::seconds(1)) continue;
    if (seen.callbacks == pres) exact = entity;
    const uint32_t dist = seen.callbacks > pres ? seen.callbacks - pres : pres - seen.callbacks;
    if (dist < near_dist) {
      near_dist = dist;
      near = entity;
    }
  }
  const uint32_t found = exact ? exact : (near_dist <= 0x4000u ? near : 0u);
  const uint64_t key = (uint64_t(pres) << 32) | index;
  auto it = g_pose_owner.find(key);
  if (it == g_pose_owner.end() || it->second != found) {
    g_pose_owner[key] = found;
    REXLOG_INFO("skater-proportions: presentation {:08X} skater {} -> entity {:08X} ({}; {} entities seen)", pres,
                index, found, exact ? "callbacks match" : (found ? "nearest callbacks" : "none"), g_seen.size());
    if (!found) {
      for (const auto& [entity, seen] : g_seen)
        REXLOG_INFO("skater-proportions:   entity {:08X} index {} callbacks {:08X}", entity, seen.index, seen.callbacks);
    }
  }
  return found;
}

}  // namespace

uint32_t skate3::skater_proportions::ExchangeEntity(uint32_t entity) {
  const uint32_t prev = t_entity;
  t_entity = entity;
  return prev;
}

// Sk8::SkaterPresEntity::Update (retail sub_82781DE0): one of the three UpdateBoneTransforms
// callers (the gameplay / viewer skaters go through StartJobs and sub_82785778 instead).
extern "C" REX_FUNC(sub_82781DE0) {
  const uint32_t prev = skate3::skater_proportions::ExchangeEntity(ctx.r3.u32);
  __imp__sub_82781DE0(ctx, base);
  skate3::skater_proportions::ExchangeEntity(prev);
}

// One skater's local pose (retail sub_8278C9E8(presentation, index), called per skater by the
// presentation update sub_8278CBA0 right before MoveRootToHips_Individual). The pose is
// presentation+1576 -> [+40 + index * 16]: 48 bytes per hierarchy bone, {scale, rotation
// (x y z w), translation}, each a vector4; the root (Trajectory) first, the hips under it.
extern "C" REX_FUNC(sub_8278C9E8) {
  const uint32_t pres = ctx.r3.u32, index = ctx.r4.u32;
  __imp__sub_8278C9E8(ctx, base);
  if (!REXCVAR_GET(skate3_mod_skater_proportions) || MiniSkaters(base)) return;
  const uint32_t data = REX_LOAD_U32(pres + 1576);
  if (data == 0) return;
  const uint32_t pose = REX_LOAD_U32(data + 40 + index * 16);
  if (pose == 0) return;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const uint32_t entity = FindEntity(pres, index);
  if (entity == 0) return;
  SkaterState& s = g_states[entity];
  if (!Build(base, entity, s)) return;
  const uint32_t nb = s.hier.bones;
  {
    // the pose's hierarchy: presentation data +12 -> +28 (sub_8278CBA0)
    const uint32_t block = REX_LOAD_U32(data + 12);
    const uint32_t ph = block ? REX_LOAD_U32(block + 28) : 0;
    const uint32_t pnb = ph ? REX_LOAD_U16(ph) : 0;
    if (pnb != nb) {
      if (s.pose_logs == 0) {
        ++s.pose_logs;
        REXLOG_WARN("skater-proportions: entity={:08X}: pose hierarchy {:08X} has {} bones, the skater's {:08X} {}",
                    entity, ph, pnb, s.hierarchy, nb);
      }
      return;
    }
  }
  std::vector<P::Bone> local(nb);
  for (uint32_t i = 0; i < nb; ++i) {
    const uint32_t b = pose + i * 48;
    for (int k = 0; k < 4; ++k) local[i].q[k] = LoadF(base, b + 16 + uint32_t(k) * 4);
    for (int k = 0; k < 3; ++k) local[i].t[k] = LoadF(base, b + 32 + uint32_t(k) * 4);
  }
  const P::Result r = P::Adjust(s.rig, local);
  for (uint32_t i = 0; i < nb; ++i) {
    const uint32_t b = pose + i * 48;
    for (int k = 0; k < 4; ++k) StoreF(base, b + 16 + uint32_t(k) * 4, local[i].q[k]);
    for (int k = 0; k < 3; ++k) StoreF(base, b + 32 + uint32_t(k) * 4, local[i].t[k]);
  }
  const auto now = Clock::now();
  if (s.pose_logs < 8 && now - s.posed > std::chrono::milliseconds(500)) {
    ++s.pose_logs;
    REXLOG_INFO("skater-proportions: entity={:08X} pose: hips moved {:.3f} {:.3f} {:.3f} m (drop {:.3f}), "
                "feet off the animation's L {:.4f} R {:.4f} m",
                entity, r.shift[0], r.shift[1], r.shift[2], r.drop, r.foot_error[0], r.foot_error[1]);
  }
  s.posed = now;
}

namespace {
// Diagnostics: what the skin-matrix path got and wrote, once per (entity, bound bone count), into
// <build>/logs/skater_proportions_<entity>_<bound>.bin (at most 24 files): "SKPD", u32 bones, u32 kind,
// u32 hips, i32 legs[6], f32 ankle_ref[2], ankle_own[2], then per bone i32 parent, f32 delta[3],
// inverse bind[16], incoming[16], written[16] (row-vector RW matrices, host floats).
std::mutex g_dump_mutex;
std::unordered_map<uint64_t, bool> g_dumped;
void DumpOnce(uint32_t entity, const SkaterState& s, const std::vector<Mat>& in, const std::vector<Mat>& outm, int kind) {
  const uint32_t nb = s.hier.bones;
  uint32_t bound = 0;
  for (const auto& m : s.inv_bind) bound += IsIdentity(m) ? 0u : 1u;
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  const uint64_t key = (uint64_t(entity) << 32) | bound;
  if (g_dumped.count(key) || g_dumped.size() >= 24) return;
  g_dumped[key] = true;
  std::error_code ec;
  const auto dir = skate3::GameDataRoot().parent_path() / "logs";
  std::filesystem::create_directories(dir, ec);
  char name[96];
  std::snprintf(name, sizeof(name), "skater_proportions_%08X_%u.bin", entity, bound);
  std::ofstream f(dir / name, std::ios::binary);
  if (!f) return;
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto f32 = [&](float v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  f.write("SKPD", 4);
  u32(nb); u32(uint32_t(kind)); u32(uint32_t(s.rig.hips));
  for (int k = 0; k < 2; ++k)
    for (int j = 0; j < 3; ++j) u32(uint32_t(s.rig.legs[k][j]));
  for (int k = 0; k < 2; ++k) f32(s.rig.ankle_ref[k]);
  for (int k = 0; k < 2; ++k) f32(s.rig.ankle_own[k]);
  for (uint32_t i = 0; i < nb; ++i) {
    u32(uint32_t(s.hier.parents[i]));
    for (int k = 0; k < 3; ++k) f32(i < s.rig.delta.size() ? s.rig.delta[i][k] : 0.0f);
    for (int k = 0; k < 16; ++k) f32(s.inv_bind[i][k]);
    for (int k = 0; k < 16; ++k) f32(in[i][k]);
    for (int k = 0; k < 16; ++k) f32(outm[i][k]);
  }
  REXLOG_INFO("skater-proportions: dumped entity={:08X} ({} bound) to {}", entity, bound, (dir / name).string());
}
}  // namespace

// Sk8::UpdateBoneTransforms(instances, count, matrices, remaps) (retail sub_827A52C8) is hooked
// by the native renderer (skate3_native_render.cpp, which snapshots the finished palettes after
// the call); it calls this first. It records which entity is which skater for the pose hook,
// and is the fallback for a skater whose pose was not adjusted there: the same adjustment made
// on the skin matrices (S = inverse bind x world, so world = bind x S).
void skate3::skater_proportions::BeforeBoneTransforms(PPCContext& ctx, uint8_t* base) {
  const uint32_t entity = t_entity;
  const uint32_t matrices = ctx.r5.u32;
  if (entity == 0 || matrices == 0 || !REXCVAR_GET(skate3_mod_skater_proportions)) {
    return;
  }
  uint32_t out = 0;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const auto now = Clock::now();
    g_seen[entity] = Seen{REX_LOAD_U32(entity + 868), REX_LOAD_U32(entity + 872), now};
    SkaterState& s = g_states[entity];
    if (now - s.posed < std::chrono::milliseconds(300)) return;   // done on its pose
    if (MiniSkaters(base) || !Build(base, entity, s)) return;
    const uint32_t nb = s.hier.bones;
    if (s.scratch[0] == 0) {
      auto* mem = rex::system::kernel_memory();
      s.scratch[0] = mem->SystemHeapAlloc(nb * 64, 16);
      s.scratch[1] = mem->SystemHeapAlloc(nb * 64, 16);
    }
    if (s.scratch[0] == 0 || s.scratch[1] == 0) return;
    if (!s.logged_fallback) {
      s.logged_fallback = true;
      REXLOG_INFO("skater-proportions: entity={:08X} (skater {}) adjusted on its skin matrices", entity,
                  REX_LOAD_U32(entity + 868));
    }
    out = s.scratch[s.flip];
    s.flip ^= 1u;
    // What the matrices are: in gameplay skin matrices (S = inverse bind x world: world = bind x S);
    // a screen may pass worlds instead. The animation keeps the reference's bone lengths, so the
    // right reading gives bound bones their reference lengths; with neither, the skater is left as is
    // (applying binds to worlds garbles every bound bone: the custom skaters on the select screen).
    std::vector<Mat> S(nb);
    for (uint32_t i = 0; i < nb; ++i) S[i] = LoadMat(base, matrices + i * 64);
    auto length_err = [&](bool skin) {
      std::vector<float> e;
      for (uint32_t i = 0; i < nb; ++i) {
        const int p = s.hier.parents[i];
        if (p < 0 || IsIdentity(s.inv_bind[i]) || s.ref_len[i] < 0.02f) continue;
        const Mat a = skin ? Mul(s.bind[i], S[i]) : S[i], b = skin ? Mul(s.bind[p], S[p]) : S[p];
        const float dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
        e.push_back(std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - s.ref_len[i]));
      }
      if (e.empty()) return 1e9f;
      std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
      return e[e.size() / 2];
    };
    const float e_skin = length_err(true), e_world = length_err(false);
    const int kind = e_skin < 0.01f && e_skin <= e_world ? 0 : (e_world < 0.01f ? 1 : 2);
    if (kind != s.matrix_kind) {
      s.matrix_kind = kind;
      REXLOG_INFO("skater-proportions: entity={:08X} (skater {}) gets {} matrices (median bone-length error as skin "
                  "{:.4f} m, as world {:.4f} m){}", entity, REX_LOAD_U32(entity + 868),
                  kind == 0 ? "skin" : (kind == 1 ? "world" : "unrecognised"), e_skin, e_world,
                  kind == 2 ? " - left as is" : "");
    }
    if (kind == 2) return;
    std::vector<Mat> w(nb);
    for (uint32_t i = 0; i < nb; ++i) {
      w[i] = kind == 0 ? Mul(s.bind[i], S[i]) : S[i];
      // affine part only: on the character select screen the ROOT's slot is no bone matrix (its 4th
      // column carries other data, 0.98 / 0.09 ...); composed through it (InverseAffine ignores that
      // column, Mul does not) every bone came out projective - the stretched columns. The root's
      // own slot is written back unchanged below.
      w[i][3] = w[i][7] = w[i][11] = 0.0f;
      w[i][15] = 1.0f;
    }
    // Only the skater's own offsets here (then his soles put back where the animation has them, in
    // WORLD space). Each bone's local is kept as the FULL matrix the game made (local = world x
    // parent world^-1, composed back the same way): with no offsets that gives back exactly what came
    // in, whatever is in the matrices. The character select screen's root matrix is not a rigid
    // placement of the body: turning locals into rotations against it (the pose path's quaternions)
    // dropped its counter-scale and stretched the custom skaters into columns. The pose path's
    // root-frame steps (hips scaled about the root, legs IK'd to root-frame foot targets) are not used
    // here: they assume the root is the ground under him - true in gameplay, not on that screen.
    // Measured on the screen's own matrices (dumps): they are relative to the HIPS (hips at the
    // origin, the spine along +x), the root matrix is unrelated to the ground, and the board hangs
    // off the hips, standing beside him. So nothing here uses the root or a world "up":
    //  - the board keeps exactly the matrices the game made;
    //  - every other bone gets his own offset in its parent's frame, keeping the rest of the game's
    //    full local matrix (no offsets = exactly what came in);
    //  - then his body (the hips and below, not the board) is moved as one so his ankles land on the
    //    animation's, his own ankle height over the reference's added along feet -> hips.
    // Offline on the dumps: board unchanged, ankles on the animation's, own bone lengths to 5 mm.
    std::vector<Mat> world(nb);
    for (int i : s.hier.order) {
      const int p = s.hier.parents[i];
      if (p < 0 || (i < int(s.board.size()) && s.board[i])) {
        world[i] = w[i];
        continue;
      }
      Mat pinv;
      if (!InverseAffine(w[p], pinv)) return;
      Mat l = Mul(w[i], pinv);
      if (s.hier.parents[p] >= 0) {           // the hips' own offset is the placement step's job
        const P::V3& d = s.rig.delta[i];
        l[12] += d[0];
        l[13] += d[1];
        l[14] += d[2];
      }
      world[i] = Mul(l, world[p]);
    }
    if (s.rig.feet && s.rig.hips >= 0) {
      const int lf = s.rig.legs[0][2], rf = s.rig.legs[1][2], h = s.rig.hips;
      const P::V3 amid = {(w[lf][12] + w[rf][12]) * 0.5f, (w[lf][13] + w[rf][13]) * 0.5f, (w[lf][14] + w[rf][14]) * 0.5f};
      const P::V3 omid = {(world[lf][12] + world[rf][12]) * 0.5f, (world[lf][13] + world[rf][13]) * 0.5f,
                          (world[lf][14] + world[rf][14]) * 0.5f};
      const P::V3 up = P::Unit(P::Sub({w[h][12], w[h][13], w[h][14]}, amid));
      const float ank = 0.5f * (s.rig.ankle_own[0] + s.rig.ankle_own[1]) - 0.5f * (s.rig.ankle_ref[0] + s.rig.ankle_ref[1]);
      const P::V3 v = P::Add(P::Sub(amid, omid), P::Scale(up, ank));
      std::vector<bool> under(nb, false);
      for (int i : s.hier.order) {
        const int p = s.hier.parents[i];
        under[i] = (i == h || (p >= 0 && under[p])) && !(i < int(s.board.size()) && s.board[i]);
      }
      for (uint32_t i = 0; i < nb; ++i) {
        if (!under[i]) continue;
        world[i][12] += v[0];
        world[i][13] += v[1];
        world[i][14] += v[2];
      }
    }
    std::vector<Mat> written(nb);
    for (uint32_t i = 0; i < nb; ++i) {
      // back in the kind it came in; a root (no bone of the body) exactly as it came
      const Mat skin = s.hier.parents[i] < 0 ? S[i] : (kind == 0 ? Mul(s.inv_bind[i], world[i]) : world[i]);
      written[i] = skin;
      for (int k = 0; k < 16; ++k) StoreF(base, out + i * 64 + uint32_t(k) * 4, skin[k]);
    }
    DumpOnce(entity, s, S, written, kind);
  }
  if (out != 0) ctx.r5.u32 = out;
}
