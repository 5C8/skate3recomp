// Mods > Skater proportions: the local-pose adjustment (no guest memory, testable on its own).
//
// The miniskaters cheat (retail sub_8278E270, run on each skater's local pose right after the
// animation is sampled) shows where the game wants a skater's size changed: in the LOCAL pose,
// before Sk8::SkaterPresentation::MoveRootToHips_Individual and the model-space / skin passes, so
// everything after it (root placement, board, skinning) follows. This does the same for a skater
// with his own bone lengths: his bones get their own offsets, the hips move so the supporting
// foot stays where the animation planted it, and both legs are bent (two-bone IK, in the plane
// the animation bent them in) to put the feet exactly on the animation's foot positions.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace skate3::skater_proportions::pose {

using V3 = std::array<float, 3>;
using Q = std::array<float, 4>;               // x, y, z, w (Hamilton, the pose's own layout)

inline V3 Add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline V3 Scale(const V3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 Cross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float Len(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Unit(const V3& a) {
  const float l = Len(a);
  return l > 1e-8f ? Scale(a, 1.0f / l) : V3{0.0f, 0.0f, 0.0f};
}

inline Q QMul(const Q& a, const Q& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline Q QConj(const Q& q) { return {-q[0], -q[1], -q[2], q[3]}; }
inline Q QNorm(const Q& q) {
  const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  return n > 1e-12f ? Q{q[0] / n, q[1] / n, q[2] / n, q[3] / n} : Q{0.0f, 0.0f, 0.0f, 1.0f};
}
inline V3 QRot(const Q& q, const V3& v) {    // v + w t + u x t, t = 2 u x v
  const V3 u = {q[0], q[1], q[2]};
  const V3 t = Scale(Cross(u, v), 2.0f);
  return Add(Add(v, Scale(t, q[3])), Cross(u, t));
}
// The shortest rotation taking direction a onto direction b.
inline Q QFromTo(const V3& a, const V3& b) {
  const V3 ua = Unit(a), ub = Unit(b);
  const float c = Dot(ua, ub);
  if (c > 1.0f - 1e-7f) return {0.0f, 0.0f, 0.0f, 1.0f};
  if (c < -1.0f + 1e-6f) {                    // opposite: any axis across a
    V3 axis = Cross(ua, {1.0f, 0.0f, 0.0f});
    if (Len(axis) < 1e-3f) axis = Cross(ua, {0.0f, 1.0f, 0.0f});
    axis = Unit(axis);
    return {axis[0], axis[1], axis[2], 0.0f};
  }
  const V3 k = Cross(ua, ub);
  return QNorm({k[0], k[1], k[2], 1.0f + c});
}

struct Bone {
  Q q{0.0f, 0.0f, 0.0f, 1.0f};
  V3 t{0.0f, 0.0f, 0.0f};
};

struct Rig {
  std::vector<int> parents;                   // effective (bones under the root), -1 = root
  std::vector<int> order;                     // parents before children
  std::vector<V3> delta;                      // own offset - reference offset (0 = unchanged)
  int hips = -1;
  float size = 1.0f;                          // own hips height / the reference's (rest pose)
  // ankle heights above the ground at rest (root frame), the reference skeleton's and the skater's
  // own: the animation places ankles for the reference's feet, the soles are what touch the ground
  std::array<float, 2> ankle_ref{0.0f, 0.0f}, ankle_own{0.0f, 0.0f};
  int truck_front = -1, truck_back = -1;      // the board (the deck between them): feet on it stay on it
  std::array<std::array<int, 3>, 2> legs{};   // {UpLeg, Leg, Foot} left, right
  bool feet = false;                          // hips + both legs found, each leg a direct chain
};

struct Result {
  V3 shift{0.0f, 0.0f, 0.0f};                 // hips moved by (root frame)
  float drop = 0.0f;                          // of it, extra lowering so a foot can reach
  std::array<float, 2> foot_error{0.0f, 0.0f};
};

// Root-frame worlds of every bone (the root itself at the origin, unrotated: the root carries the
// skater's motion, and his feet are placed relative to it).
inline void Worlds(const Rig& rig, const std::vector<Bone>& local, std::vector<Q>& wq, std::vector<V3>& wp) {
  const size_t n = local.size();
  wq.assign(n, Q{0.0f, 0.0f, 0.0f, 1.0f});
  wp.assign(n, V3{0.0f, 0.0f, 0.0f});
  for (int b : rig.order) {
    const int p = rig.parents[b];
    if (p < 0) continue;                      // the root: identity in its own frame
    wq[b] = QMul(wq[p], local[b].q);
    wp[b] = Add(wp[p], QRot(wq[p], local[b].t));
  }
}

// The pose's bones get their own offsets; with feet, the legs keep the animation's footing.
inline Result Adjust(const Rig& rig, std::vector<Bone>& local) {
  Result res;
  const size_t n = local.size();
  if (rig.parents.size() != n || rig.delta.size() != n) return res;
  std::vector<Q> aq;                          // the animation's (shared skeleton) worlds
  std::vector<V3> ap;
  if (rig.feet) Worlds(rig, local, aq, ap);
  for (size_t i = 0; i < n; ++i) local[i].t = Add(local[i].t, rig.delta[i]);
  if (!rig.feet) return res;
  std::vector<Q> wq;
  std::vector<V3> wp;
  Worlds(rig, local, wq, wp);

  // 1. like the miniskaters cheat: the hips' place scaled about the root (the ground under the
  // skater) by his size, so strides and stance shrink with him...
  const float sz = rig.size;
  const V3 up = {0.0f, 1.0f, 0.0f};
  V3 shift = Sub(Scale(ap[rig.hips], sz), wp[rig.hips]);
  // ...the feet too, but with their soles at the animation's heights up to standing on the deck
  // (on the ground and on the board), scaled above that (steps, tucks). Sole height = the ankle's
  // height over its rest height: own ankle = own rest ankle + that.
  constexpr float kContact = 0.15f;           // sole height standing on the deck, with margin
  std::array<V3, 2> target;
  for (int i = 0; i < 2; ++i) {
    const V3& f = ap[rig.legs[i][2]];
    float sole = f[1] - rig.ankle_ref[i];
    if (sole > kContact) sole = kContact + (sole - kContact) * sz;
    target[i] = {f[0] * sz, rig.ankle_own[i] + sole, f[2] * sz};
    // ...from his own hip joints: across the ground each foot moves as far as his hip joint is from
    // the reference's, so the legs keep the animation's direction (Shrek's wide hips pulled to the
    // reference's stance bent his legs inward)
    const V3 hip_off = Sub(Add(wp[rig.legs[i][0]], shift), Scale(ap[rig.legs[i][0]], sz));
    target[i][0] += hip_off[0];
    target[i][2] += hip_off[2];
  }
  // a foot the animation has on the deck stays on it (a wide stance would put it past the trucks)
  if (rig.truck_front >= 0 && rig.truck_back >= 0) {
    const V3 tf = ap[rig.truck_front], tb = ap[rig.truck_back];
    const V3 center = Scale(Add(tf, tb), 0.5f);
    const V3 axis = Unit(Sub(tf, tb));
    const V3 lat = Unit(Cross({0.0f, 1.0f, 0.0f}, axis));
    const V3 nrm = Cross(axis, lat);
    for (int i = 0; i < 2; ++i) {
      const V3 fa = Sub(ap[rig.legs[i][2]], center);
      const float a = Dot(fa, axis), l = Dot(fa, lat), h = Dot(fa, nrm);
      if (std::fabs(l) > 0.2f || std::fabs(a) > 0.55f || h < -0.05f || h > 0.35f) continue;
      const V3 d = Sub(target[i], center);
      const float na = std::clamp(Dot(d, axis), -0.40f, 0.40f), nl = std::clamp(Dot(d, lat), -0.09f, 0.09f);
      target[i] = Add(center, Add(Add(Scale(axis, na), Scale(lat, nl)), Scale(nrm, Dot(d, nrm))));
    }
  }
  // 2. lower the hips if a foot is out of the leg's reach
  float drop = 0.0f;
  for (int i = 0; i < 2; ++i) {
    const auto& leg = rig.legs[i];
    const V3 a = Add(wp[leg[0]], shift);
    const float reach = (Len(Sub(wp[leg[1]], wp[leg[0]])) + Len(Sub(wp[leg[2]], wp[leg[1]]))) * 0.999f;
    const V3 d = Sub(target[i], a);
    if (Len(d) <= reach) continue;
    // |d + x up| = reach, x >= 0 (the hip joint down by x)
    const float dy = Dot(d, up);
    const float disc = dy * dy - (Dot(d, d) - reach * reach);
    float x = -dy;
    if (disc >= 0.0f) {
      const float r = std::sqrt(disc);
      x = (-dy - r) > 0.0f ? (-dy - r) : (-dy + r);
    }
    drop = std::max(drop, std::max(0.0f, x));
  }
  shift = Sub(shift, Scale(up, drop));
  res.shift = shift;
  res.drop = drop;
  {
    const int hp = rig.parents[rig.hips];     // hips' local is in its parent's frame
    local[rig.hips].t = Add(local[rig.hips].t, hp >= 0 ? QRot(QConj(wq[hp]), shift) : shift);
  }
  // 3. each leg: knee by the law of cosines, in the plane the animation bent it in
  for (int k = 0; k < 2; ++k) {
    const auto& leg = rig.legs[k];
    const V3 a = Add(wp[leg[0]], shift), kc = Add(wp[leg[1]], shift), fc = Add(wp[leg[2]], shift);
    const float la = Len(Sub(kc, a)), lb = Len(Sub(fc, kc));
    const V3 goal = target[k];
    const V3 to_t = Sub(goal, a);
    float d = Len(to_t);
    if (la < 1e-4f || lb < 1e-4f || d < 1e-4f) continue;
    const V3 u = Scale(to_t, 1.0f / d);
    d = std::clamp(d, std::fabs(la - lb) + 1e-4f, la + lb - 1e-4f);
    const V3 ko = Sub(ap[leg[1]], ap[leg[0]]);
    V3 v = Sub(ko, Scale(u, Dot(ko, u)));
    if (Len(v) < 1e-5f) v = Sub(Sub(kc, a), Scale(u, Dot(Sub(kc, a), u)));
    v = Unit(v);
    const float along = (la * la - lb * lb + d * d) / (2.0f * d);
    const float height = std::sqrt(std::max(0.0f, la * la - along * along));
    const V3 knee = Add(a, Add(Scale(u, along), Scale(v, height)));
    const V3 foot = Add(a, Scale(u, d));
    const Q r1 = QFromTo(Sub(kc, a), Sub(knee, a));
    const Q up_q = QNorm(QMul(r1, wq[leg[0]]));
    const Q r2 = QFromTo(QRot(r1, Sub(fc, kc)), Sub(foot, knee));
    const Q knee_q = QNorm(QMul(r2, QMul(r1, wq[leg[1]])));
    const Q parent_q = wq[rig.parents[leg[0]]];   // the hips' side: rotations unchanged
    local[leg[0]].q = QNorm(QMul(QConj(parent_q), up_q));
    local[leg[1]].q = QNorm(QMul(QConj(up_q), knee_q));
    local[leg[2]].q = QNorm(QMul(QConj(knee_q), aq[leg[2]]));   // the foot as animated
    res.foot_error[k] = Len(Sub(foot, goal));
  }
  return res;
}

}  // namespace skate3::skater_proportions::pose
