#pragma once
#include <algorithm>
#include <cmath>

namespace skate3::vr {
// Only filters the body reference yaw; never receives the HMD pose.
struct BodyYawFilter {
  bool valid = false;
  double yaw = 0.0;
  void Reset() { valid = false; }
  double Update(double x, double z, double dt, double seconds,
                double deadzone_degrees, double max_degrees_per_second) {
    constexpr double pi = 3.14159265358979323846;
    if (!std::isfinite(x) || !std::isfinite(z) || std::hypot(x,z) < 1e-4)
      return yaw;
    const double target = std::atan2(x,z);
    if (!valid) { valid = true; return yaw = target; }
    const double error = std::remainder(target-yaw, 2*pi);
    const double deadzone = std::clamp(deadzone_degrees,0.0,45.0)*pi/180.0;
    // Follow only the excess outside the dead zone, with no boundary snap.
    const double excess = std::copysign(std::max(0.0,std::abs(error)-deadzone),error);
    dt = std::clamp(dt,0.0,0.1); // never catch up an entire loading stall
    const double alpha = seconds > 0 ? -std::expm1(-dt/seconds) : 1.0;
    double step = excess*alpha;
    if (max_degrees_per_second > 0) {
      const double limit = max_degrees_per_second*pi/180.0*dt;
      step = std::clamp(step,-limit,limit);
    }
    yaw = std::remainder(yaw+step,2*pi);
    return yaw;
  }
};
} // namespace skate3::vr
