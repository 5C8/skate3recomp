// Presentation clock: an even interpolation clock for the realtime game.
//
// The sim runs fixed 1/60 s ticks on the main thread (wall clock, one tick
// per main-loop pass). The render thread asks
// DataSystem::RealTimeController::Implementation::Update(dt) which sim frame
// to show: it publishes a target frame (+12) and a blend factor (+16), and the
// presentation draws lerp(frame before target, target, alpha).
//
// Measured (tools/tick_camsig.py, 2026-11-13): the sim's states are perfectly
// even, but the guest clock is not an interpolation clock: target = newest
// frame the moment it is written, alpha = an accumulator phase unrelated to
// when that frame arrived (pinned at 1.0 in 65% of render frames).
//
// Replacement (sk83_present_clock): the original still runs for all its
// bookkeeping and its target (the newest frame) is KEPT (the skater's pose
// blend pairs the controller alpha with the newest two frames itself; a
// trailing target made it sawtooth). Only alpha is rewritten.
//
// MEASURED, not tuned: every frame's arrival time A_k (the first render
// update that sees it as newest) is recorded. Sim frames are produced on a
// steady wall-clock line k*tau (tau fitted from the arrivals), so
// j_k = A_k - k*tau is each frame's arrival jitter. The clock shows
//     P(now) = (now - Q) / tau   (frames)
// with Q = the late-target quantile of j over the last ~10 s: frame k is due
// on screen at k*tau + Q, which by construction is after it arrived for all
// but the target fraction of frames. Q is the smallest delay this machine's
// frame timing allows at that hold rate; it follows load/fps changes with a
// rate-limited slew (an inaudible <=1% speed change).
//
// The native smoother plays at exactly the clock's host time: poses are
// stamped with the host time they are due (shown frame time * tau + Q), so an
// unclamped frame lands exactly on the play point and an EARLY clamp (alpha 0,
// the newest frame arrived more than a frame before it is due) is interpolated
// across for free. Only a frame arriving after it is due holds.

// RETAIL port (2026-11-25) of sk83_na_m src/sk83_present_clock.cpp: store-time frame arrivals, the tick
// period from the controller's current value (+36), period-change restarts, the DoSimRestore camera fix and
// the precise sim tick timer. Retail addresses: RealTimeController::Update 82DBDB60, StoreFrame 82DBF098,
// DoSimRestore 827A0080, XBox2ExecutionMan::timerThread sub_82966930, SetWaitableTimer 82EE2248 (struct
// offsets checked against retail's listings: timer +8 handle / +16 due / +24 period, tracker +8 slot / +84
// 24-byte entries, restore entries 224 bytes with the no-blend byte at +212).
namespace skate3_guest_profiler {
void NoteSimTick();  // skate3_guest_profiler.cpp
}
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <immintrin.h>
#include <chrono>
#include <thread>

REXCVAR_DEFINE_BOOL(skate3_present_clock, true, "Skate 3",
                    "Even presentation clock: interpolate sim frames on a continuous clock "
                    "whose delay is measured from frame arrival times (instead of the guest's "
                    "jump-to-newest + stalling alpha). With the native smooth camera off it still blends frames itself");
REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_smooth_camera);
REXCVAR_DEFINE_DOUBLE(skate3_present_clock_late_pct, 0.5, "Skate 3",
                      "Share of sim frames (percent) allowed to arrive after they are due on "
                      "screen (each one is a short hold). The clock measures arrival timing "
                      "and sets the smallest delay that meets this. Lower = fewer holds, more "
                      "delay");

namespace skate3::native_scene {
extern std::atomic<double> g_camsig_deadline;  // native/skate3_native_diagnostics.cpp (retail has no tick trace: the tick pushes are stubs)
inline void CamSigPushTick(double, uint8_t, uint32_t, float, float) {}
inline void MaybeStartTickTrace(double) {}
}  // namespace skate3::native_scene

// Written by the controller update, read by the native smoother on the same
// (guest render) thread.
std::atomic<double> g_content_time{-1.0};  // host time the shown content is due
std::atomic<double> g_content_play{0.0};   // host time the clock was evaluated at
std::atomic<double> g_content_stamp{0.0};  // = play (staleness check)
std::atomic<double> g_tick_period{1.0 / 60.0};  // current sim tick period (s)

namespace skate3_present_clock {
// Due time (host seconds) of the content the guest presents this frame when
// the clock drives it, else -1. `*play` (if given) receives the host time the
// clock evaluated: the smoother's exact playback point.
double ContentTime(double now, double* play) {
  const double ct = g_content_time.load(std::memory_order_relaxed);
  if (ct < 0.0 || now - g_content_stamp.load(std::memory_order_relaxed) > 0.1) return -1.0;
  if (play) *play = g_content_play.load(std::memory_order_relaxed);
  return ct;
}
double ContentTime(double now) { return ContentTime(now, nullptr); }
// The sim's current tick period (seconds): 1/60 normally, longer in slow
// motion, shorter with a faster sim-rate override.
double TickPeriod() { return g_tick_period.load(std::memory_order_relaxed); }
}  // namespace skate3_present_clock

namespace {

constexpr uint32_t kNoFrame = 0x1FFFFFF;

// Exact host time each sim frame was stored (DataTracker::StoreFrame, the
// later of its trackers): the clock's arrival measure. The render thread only
// notices a frame at its next update (measured 1.7 ms later at p50, 3.6 ms at
// p99), and measuring arrival there added that render-loop granularity to the
// jitter the delay must cover. Written by the main thread, read by the render
// thread (seqlock-free: frame number published after the time).
struct StoredFrame {
  std::atomic<double> t{0.0};
  std::atomic<uint32_t> frame{0xFFFFFFFFu};
};
StoredFrame g_stored[64];

double StoredTime(uint32_t k, double now) {
  const StoredFrame& e = g_stored[k & 63u];
  if (e.frame.load(std::memory_order_acquire) != k) return now;
  const double t = e.t.load(std::memory_order_relaxed);
  return (t > 0.0 && t <= now && now - t < 0.25) ? t : now;
}
constexpr size_t kWindow = 600;     // arrivals kept (~10 s of sim frames)
constexpr size_t kMinFit = 60;      // before this, tau = nominal
constexpr double kHitch = 0.25;     // arrival gap that restarts the measurement

struct Arrival {
  uint32_t k;
  double a;
};

// Render thread only (the controller is updated from AppMan::RenderLoop).
struct ClockState {
  uint32_t impl = 0;
  bool valid = false;
  uint32_t newest = 0;
  uint32_t k_org = 0;     // origin frame: times are measured from it
  double tau = 0.0;       // fitted real sim frame period (s)
  double nominal = 0.0;   // the sim period the measurement started from
  double q = 0.0;         // applied delay offset: frame k due at k*tau + q
  double q_target = 0.0;  // measured quantile
  double line_med = 0.0;  // median arrival line: frame k arrives ~k*tau + line_med
  double next_fit = 0.0;
  std::vector<Arrival> win;  // ring, kWindow
  size_t head = 0, count = 0;
  // Stats for the report.
  double j_p50 = 0.0, j_p99 = 0.0, j_max = 0.0;
  uint64_t outliers = 0;
  uint64_t late = 0, early = 0, frames = 0, arrivals = 0, late_arrivals = 0;
  uint64_t cam_prev = 0, cam_flagged = 0;  // camera restores pinned to the previous frame
  uint64_t restarts = 0, fit_rejects = 0;  // diagnostics
  float ctl32 = 0.0f, ctl36 = 0.0f;        // controller +32 / +36 at the last update
  double last_report = 0.0;
};
ClockState g_clock;

// The alpha this controller update wrote (render thread), for the camera
// restore below; cleared whenever the clock is not driving.
bool g_alpha_valid = false;
float g_alpha_written = 0.0f;

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

float LoadF(uint8_t* base, uint32_t a) { return std::bit_cast<float>(REX_LOAD_U32(a)); }
void StoreF(uint8_t* base, uint32_t a, float v) { REX_STORE_U32(a, std::bit_cast<uint32_t>(v)); }

double Line(const ClockState& s, uint32_t k) {
  return double(int64_t(k) - int64_t(s.k_org)) * s.tau;
}

void Restart(ClockState& s, uint32_t impl, uint32_t newest, double now, double nominal) {
  ++s.restarts;
  s.impl = impl;
  s.valid = true;
  s.newest = newest;
  s.k_org = newest;
  s.tau = nominal;
  s.nominal = nominal;
  s.win.assign(kWindow, Arrival{});
  s.head = s.count = 0;
  // Until measured: due one frame after arrival (the old fixed-lead regime).
  s.q = now + nominal;
  s.q_target = s.q;
  s.line_med = now;
  s.next_fit = now + 0.25;
}

void PushArrival(ClockState& s, uint32_t k, double a) {
  s.win[s.head] = Arrival{k, a};
  s.head = (s.head + 1) % kWindow;
  s.count = std::min(s.count + 1, kWindow);
}

// Fit tau (least squares slope of arrival vs frame number) and the jitter
// quantile Q over the window.
void Fit(ClockState& s, double nominal, double late_frac) {
  if (s.count < 8) return;
  const size_t n = s.count;
  const size_t first = (s.head + kWindow - n) % kWindow;
  const uint32_t k0 = s.win[first].k;
  const double a0 = s.win[first].a;
  // Hitch frames are left out of the slope fit: one 40-90 ms hitch skewed
  // the least-squares slope 3% (measured: tau 16.09 ms, 59% late arrivals
  // for the 10 s the hitch stayed in the window). Outliers = more than two
  // frames off the median under the current tau.
  if (n >= kMinFit) {
    std::vector<double> j0(n);
    for (size_t i = 0; i < n; ++i) {
      const Arrival& r = s.win[(first + i) % kWindow];
      j0[i] = (r.a - a0) - double(r.k - k0) * s.tau;
    }
    std::vector<double> sorted = j0;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    const double med0 = sorted[n / 2];
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    double used = 0;
    for (size_t i = 0; i < n; ++i) {
      if (std::fabs(j0[i] - med0) > 2.0 * s.tau) continue;
      const Arrival& r = s.win[(first + i) % kWindow];
      const double x = double(r.k - k0), y = r.a - a0;
      sx += x; sy += y; sxx += x * x; sxy += x * y;
      used += 1.0;
    }
    const double den = used * sxx - sx * sx;
    if (den > 0.0) {
      const double slope = (used * sxy - sx * sy) / den;
      // The sim runs on its own wall clock; accept only a sane fit.
      if (std::fabs(slope / nominal - 1.0) >= 0.05) {
        ++s.fit_rejects;
      }
      if (std::fabs(slope / nominal - 1.0) < 0.05) {
        // Keep the due time of the newest frame continuous across the change.
        s.q += Line(s, s.newest) - double(int64_t(s.newest) - int64_t(s.k_org)) * slope;
        s.tau = slope;
      }
    }
  }
  std::vector<double> j(n);
  for (size_t i = 0; i < n; ++i) {
    const Arrival& r = s.win[(first + i) % kWindow];
    j[i] = (r.a - a0) - double(r.k - k0) * s.tau;
  }
  std::sort(j.begin(), j.end());
  const double med = j[n / 2];
  // Outliers (hitches: more than two frames past the median) hold no matter
  // what the delay is; letting them set Q would add their size to every frame.
  size_t m = n;
  while (m > 1 && j[m - 1] > med + 2.0 * s.tau) --m;
  s.outliers = n - m;
  const size_t qi = std::min(m - 1, size_t(std::floor(double(m) * (1.0 - late_frac))));
  // Frame k is NEEDED from the moment the shown time passes k-1 (the guest
  // blends k-1 -> k), i.e. at Line(k) - tau + q: q = jitter quantile + tau.
  s.q_target = a0 - Line(s, k0) + j[qi] + s.tau;
  s.line_med = a0 - Line(s, k0) + med;
  s.j_p50 = med - j[0];
  s.j_p99 = j[std::min(m - 1, size_t(double(m) * 0.99))] - j[0];
  s.j_max = j[m - 1] - j[0];
}

}  // namespace

extern "C" REX_FUNC(rex_DataSystem_RealTimeController_Implementation_Update_QAAXM_Z_82DBDB60) {
  const uint32_t impl = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const bool on = REXCVAR_GET(skate3_present_clock);
  const bool mine = on && g_clock.valid && g_clock.impl == impl;
  g_alpha_valid = false;
  if (mine) {
    // The original asserts if the target did not move and its alpha fell
    // below the previous one (+24); ours is not its own, so start it from 0.
    StoreF(base, impl + 24, 0.0f);
  }
  __imp__rex_DataSystem_RealTimeController_Implementation_Update_QAAXM_Z_82DBDB60(ctx, base);

  const double now = Now();
  skate3::native_scene::MaybeStartTickTrace(now);
  const bool recording =
      now < skate3::native_scene::g_camsig_deadline.load(std::memory_order_relaxed);
  auto log_update = [&] {
    if (recording) {
      skate3::native_scene::CamSigPushTick(now, 3, REX_LOAD_U32(impl + 12),
                                           LoadF(base, impl + 16), float(dt));
    }
  };

  if (!on || !(dt > 0.0)) {
    if (!on) {
      g_clock.valid = false;
      g_content_time.store(-1.0, std::memory_order_relaxed);
    }
    log_update();
    return;
  }
  // After the original, +12 is the newest written frame (it always skips to it).
  const uint32_t newest = REX_LOAD_U32(impl + 12);
  // The CURRENT sim tick period (+36): the game's sim-rate override and slow
  // motion change it (kSimRateValue 120 -> 1/120 s); +32 stays the nominal
  // 1/60. Using +32 ran the clock at 60 Hz against 120 Hz (or 30 Hz) frames.
  const float cur_period = LoadF(base, impl + 36);
  const float nominal_f = (cur_period > 0.0005f && cur_period < 0.5f) ? cur_period
                                                                      : LoadF(base, impl + 32);
  if (newest == kNoFrame || !(nominal_f > 0.0f)) {
    g_clock.valid = false;
    g_content_time.store(-1.0, std::memory_order_relaxed);
    log_update();
    return;
  }
  const double nominal = double(nominal_f);
  ClockState& s = g_clock;
  g_tick_period.store(nominal, std::memory_order_relaxed);
  s.ctl32 = nominal_f;
  s.ctl36 = LoadF(base, impl + 36);
  const double late_frac =
      std::clamp(double(REXCVAR_GET(skate3_present_clock_late_pct)), 0.05, 20.0) * 0.01;

  if (!mine || newest < s.newest || newest - s.newest > 30 ||
      std::fabs(nominal / s.nominal - 1.0) > 0.02) {
    // ... or the sim rate changed (slow motion, the sim-rate override).
    // New controller (entering realtime) or frame numbers reset/jumped.
    Restart(s, impl, newest, now, nominal);
    PushArrival(s, newest, StoredTime(newest, now));
  } else if (newest != s.newest) {
    // Arrival of every frame since the last update (skipped ones share it).
    const double last_a = s.count ? s.win[(s.head + kWindow - 1) % kWindow].a : now;
    if (now - last_a > std::max(kHitch, 4.0 * nominal)) {
      // Pause/load: the sim's line moved; measure afresh.
      Restart(s, impl, newest, now, nominal);
      PushArrival(s, newest, StoredTime(newest, now));
    } else {
      for (uint32_t k = s.newest + 1; k <= newest; ++k) {
        const double arrived = StoredTime(k, now);
        PushArrival(s, k, arrived);
        ++s.arrivals;
        // Arrived after it was due = the clock had to hold for it.
        const double due = Line(s, k) - s.tau + s.q;
        if (arrived > due) {
          ++s.late_arrivals;
          // ABSORB the lateness: the shown time held at the previous frame
          // while this one was missing; moving the clock back by exactly
          // that much continues the motion from the held point instead of
          // jumping forward to catch up (measured: late arrivals come in
          // bursts when the arrival jitter shifts, 5-10% of a 10 s window,
          // faster than the quantile refit follows; each was a freeze AND
          // a jump). The larger delay also covers the rest of the burst and
          // slews back down. Capped at one frame: a longer gap is a hitch,
          // whose lateness should not persist as seconds of extra delay.
          s.q += std::min(arrived - due, s.tau);
        }
      }
      s.newest = newest;
    }
  }

  if (now >= s.next_fit) {
    Fit(s, nominal, late_frac);
    s.next_fit = now + 0.25;
  }
  // Slew the applied delay toward the measured one: at most 1% playback
  // speed change; snap only when far off (first fit, load).
  {
    const double d = s.q_target - s.q;
    if (std::fabs(d) > 0.1) {
      s.q = s.q_target;
    } else {
      const double step = 0.01 * std::min(dt, 0.05);
      s.q += std::clamp(d, -step, step);
    }
  }

  // Presentation time in frames, straight from host time.
  const double p = double(s.k_org) + (now - s.q) / s.tau;
  const double a_cont = p - (double(newest) - 1.0);
  const bool late = a_cont > 1.0;   // due frame not here yet: hold on newest
  const bool early = a_cont < 0.0;  // newest-1 is still in the future
  // The guest shows EXACT sim frames only (alpha 0 = newest-1, 1 = newest);
  // the native smoother interpolates between them at the play point. A
  // guest-side blend made every pose's content time depend on the guest
  // applying this alpha at exactly this update; any system that posed a
  // render update late (or blended differently) then carried a stamp a few
  // ms off: measured as the skater flickering ~1 cm at speed. An exact
  // frame's identity only changes every sim frame, so a pose captured one
  // update late still pairs with the right stamp. Shown frame: the one the
  // play point is heading to (newest when P is within its blend span).
  // Without the native smoother nothing would interpolate the exact frames
  // (60 Hz steps): the guest then blends at the continuous clock itself.
  const bool exact = REXCVAR_GET(skate3_native_render_scene) &&
                     REXCVAR_GET(skate3_native_render_scene_smooth_camera);
  const double a = exact ? (early ? 0.0 : 1.0) : std::clamp(a_cont, 0.0, 1.0);
  const float alpha = float(a);

  // The shown frame's due host time.
  g_content_time.store(Line(s, newest) + (a - 1.0) * s.tau + s.q, std::memory_order_relaxed);
  g_content_play.store(now, std::memory_order_relaxed);
  g_content_stamp.store(now, std::memory_order_relaxed);

  StoreF(base, impl + 16, alpha);
  StoreF(base, impl + 24, alpha);
  g_alpha_valid = true;
  g_alpha_written = alpha;

  ++s.frames;
  if (late) ++s.late;
  if (early) ++s.early;
  if (recording) {
    skate3::native_scene::CamSigPushTick(now, 3, newest, alpha, float(dt));
    // kind 5: newest written frame, lead (newest - P), applied delay (ms).
    skate3::native_scene::CamSigPushTick(now, 5, newest, float(double(newest) - p),
                                         float((Line(s, newest) + s.q - now) * 1e3));
  }
  if (now - s.last_report > 10.0) {
    if (s.last_report > 0.0 && s.frames) {
      // delay = how long after its arrival a typical frame is due on screen.
      REXLOG_INFO(
          "present-clock: delay {:.1f} ms (target {:.1f}) | arrival jitter p50 {:.1f} p99 {:.1f} "
          "max {:.1f} ms, {} hitches | late arrivals {} of {} ({:.2f}%, target {:.2f}%) | "
          "render frames late {} early {} of {} | tau {:.4f} ms | camera prev-frame restores {} "
          "({} with no-blend cameras) | ctl +32 {:.5f} +36 {:.5f} restarts {} fit rejects {}",
          (s.q - s.line_med) * 1e3, (s.q_target - s.line_med) * 1e3, s.j_p50 * 1e3, s.j_p99 * 1e3, s.j_max * 1e3, s.outliers, s.late_arrivals,
          s.arrivals, s.arrivals ? 100.0 * double(s.late_arrivals) / double(s.arrivals) : 0.0,
          late_frac * 100.0, s.late, s.early, s.frames, s.tau * 1e3, s.cam_prev, s.cam_flagged,
          s.ctl32, s.ctl36, s.restarts, s.fit_rejects);
    }
    s.last_report = now;
    s.late = s.early = s.frames = s.arrivals = s.late_arrivals = 0;
    s.cam_prev = s.cam_flagged = 0;
    s.restarts = s.fit_rejects = 0;
  }
}

// DataSystem::DataTracker::StoreFrame: the sim finished writing a frame. Its
// number is in the tracker entry the store fills (slot +8 before the call;
// entries are 24 bytes at +84, number = word +16 >> 7, as GetFrameNumber
// reads it). Timestamped for the latency test (kind 7).
extern "C" REX_FUNC(rex_DataSystem_DataTracker_StoreFrame_AAAXPAVFrameHeader_2_Z_82DBF098) {
  const uint32_t tracker = ctx.r3.u32;
  const uint32_t slot = REX_LOAD_U32(tracker + 8);
  __imp__rex_DataSystem_DataTracker_StoreFrame_AAAXPAVFrameHeader_2_Z_82DBF098(ctx, base);
  ::skate3_guest_profiler::NoteSimTick();
  const double now = Now();
  if (slot < REX_LOAD_U32(tracker + 0)) {
    const uint32_t frame = REX_LOAD_U32(REX_LOAD_U32(tracker + 84) + 24 * slot + 16) >> 7;
    StoredFrame& e = g_stored[frame & 63u];
    if (e.frame.load(std::memory_order_relaxed) != frame) {
      e.frame.store(0xFFFFFFFFu, std::memory_order_relaxed);
    }
    e.t.store(now, std::memory_order_relaxed);  // the later tracker's store wins
    e.frame.store(frame, std::memory_order_release);
    if (now < skate3::native_scene::g_camsig_deadline.load(std::memory_order_relaxed)) {
      skate3::native_scene::CamSigPushTick(now, 7, frame, float(tracker & 0xFFFFFF), 0.0f);
    }
  }
}

// Sk8::CameraPresentation::DoSimRestore(RestoreData const&, float alpha): the
// render thread's camera = blend of the previous and newest sim frame's camera
// states (RestoreData items: [0] = newest frame, [1] = previous; each
// {data, frame}). It does NOT always use alpha: a camera entry whose no-blend
// byte (+212 of its 224-byte state) is set restores at a fixed 1.0 (the newest
// frame; 0.0 only with the RestoreData+48 top bit), and a global switch copies
// the newest frame outright. With the game's clock that matched (its target is
// the newest frame, alpha mostly pinned at 1.0). With the even presentation
// clock in exact mode, alpha 0 means "show exactly the previous frame" (the
// newest one arrived early) and every other system does; such cameras still
// showed the newest, one sim frame ahead of the world and stamped with the
// previous frame's content time. Right after any hitch the clock restarts on
// that boundary (about half the frames early until the delay settles), so it
// was most visible after a respawn: the camera was off until the skater landed.
// Fix: when the clock drove alpha to exactly 0, restore the previous frame on
// every path by handing the original (previous, previous).
extern "C" REX_FUNC(rex_Sk8_CameraPresentation_DoSimRestore_AAAXABURestoreData_IRestoreable_DataSystem_M_Z_827A0080) {
  const uint32_t rd = ctx.r4.u32;
  const uint32_t begin = REX_LOAD_U32(rd + 0);
  const uint32_t end = REX_LOAD_U32(rd + 4);
  const bool pin_prev = g_alpha_valid && g_alpha_written == 0.0f && ctx.f1.f64 == 0.0 &&
                        begin != 0 && end >= begin + 16;
  if (!pin_prev) {
    __imp__rex_Sk8_CameraPresentation_DoSimRestore_AAAXABURestoreData_IRestoreable_DataSystem_M_Z_827A0080(ctx, base);
    return;
  }
  const uint32_t cur_data = REX_LOAD_U32(begin + 0);
  const uint32_t cur_frame = REX_LOAD_U32(begin + 4);
  const uint32_t prev_data = REX_LOAD_U32(begin + 8);
  const uint32_t prev_frame = REX_LOAD_U32(begin + 12);
  if (prev_data == 0) {
    __imp__rex_Sk8_CameraPresentation_DoSimRestore_AAAXABURestoreData_IRestoreable_DataSystem_M_Z_827A0080(ctx, base);
    return;
  }
  ++g_clock.cam_prev;
  // Diagnostic: did the newest frame hold a no-blend camera (the case the
  // original got wrong)? Same layout walk as the original.
  if (cur_data != 0) {
    const uint32_t hdr = (cur_data + 3) & ~3u;
    const uint32_t n = REX_LOAD_U32(hdr);
    const uint32_t entries = (hdr + 19) & ~15u;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
      if (REX_LOAD_U8(entries + i * 224 + 212) != 0) {
        ++g_clock.cam_flagged;
        break;
      }
    }
  }
  REX_STORE_U32(begin + 0, prev_data);
  REX_STORE_U32(begin + 4, prev_frame);
  __imp__rex_Sk8_CameraPresentation_DoSimRestore_AAAXABURestoreData_IRestoreable_DataSystem_M_Z_827A0080(ctx, base);
  REX_STORE_U32(begin + 0, cur_data);
  REX_STORE_U32(begin + 4, cur_frame);
}

// ---- Precise sim tick timer (skate3_precise_sim_timer) ------------------------
// The main loop starts one sim tick per wake from XBox2ExecutionMan::Wait, and
// the game's timer thread (XBox2ExecutionMan::timerThread) paces those wakes by
// re-arming a RELATIVE one-shot waitable timer after its own bookkeeping, with a
// 60-sample average-lateness correction. Measured (tick trace, Downtown): wakes
// spread 8.8 ms around a 60 Hz line while the sim compute only spreads 2.8 ms,
// so the presentation clock had to delay frames ~22-28 ms to cover the jitter.
// Here the timer thread's SetWaitableTimer blocks on the host until the next
// point of an exact line (high-resolution host timer + a short spin), then arms
// the guest timer to fire immediately: wakes land on the line and the game's
// own correction sees ~zero lateness.
REXCVAR_DEFINE_BOOL(skate3_precise_sim_timer, true, "Skate 3",
                    "Pace the sim tick timer on an exact 60 Hz host line (lower, steadier "
                    "input latency with the even presentation clock)");

namespace {
std::atomic<uint32_t> g_exman{0};  // XBox2ExecutionMan* (timerThread's this)

struct PreciseTimer {
#ifdef _WIN32
  HANDLE timer = nullptr;
#endif
  double next = 0.0;  // next deadline (host seconds)
  uint64_t ticks = 0, resyncs = 0;
  double spin_total = 0.0, last_report = 0.0;
};
PreciseTimer g_pt;  // timer thread only
}  // namespace

extern "C" REX_FUNC(sub_82966930) {
  g_exman.store(ctx.r3.u32, std::memory_order_relaxed);
  __imp__sub_82966930(ctx, base);
}

extern "C" REX_FUNC(rex_SetWaitableTimer_82EE2248) {
  const uint32_t exman = g_exman.load(std::memory_order_relaxed);
  const uint32_t due_ptr = ctx.r4.u32;
  if (!REXCVAR_GET(skate3_precise_sim_timer) || exman == 0 || ctx.r3.u32 != REX_LOAD_U32(exman + 8) ||
      due_ptr == 0 || ctx.r5.u32 != 0) {
    __imp__rex_SetWaitableTimer_82EE2248(ctx, base);
    return;
  }
  // Period from the game's own setting (XBox2ExecutionMan +24, 100 ns units).
  const uint64_t period_100ns = REX_LOAD_U64(exman + 24);
  const double period = (period_100ns > 10000 && period_100ns < 10000000)
                            ? double(period_100ns) * 1e-7
                            : 1.0 / 60.0;
  PreciseTimer& pt = g_pt;
#ifdef _WIN32
  if (pt.timer == nullptr) {
    pt.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
  }
#endif
  double now = Now();
  pt.next += period;
  if (pt.next < now - period || pt.next > now + 2.0 * period) {
    pt.next = now + period;  // first call, hitch or pause: restart the line
    ++pt.resyncs;
  }
  // Sleep to ~0.6 ms before the deadline, then spin the rest.
  const double sleep_s = pt.next - now - 0.0006;
#ifdef _WIN32
  if (sleep_s > 0.0 && pt.timer != nullptr) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(sleep_s * 1e7);
    if (SetWaitableTimer(pt.timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(pt.timer, 100);
    }
  }
#else
  if (sleep_s > 0.0) {
    std::this_thread::sleep_for(std::chrono::duration<double>(std::min(sleep_s, 0.1)));
  }
#endif
  const double spin_t0 = Now();
  while ((now = Now()) < pt.next) {
    _mm_pause();
  }
  pt.spin_total += now - spin_t0;
  ++pt.ticks;
  // Arm the guest timer to fire right away (relative 100 ns).
  REX_STORE_U64(due_ptr, uint64_t(int64_t(-1)));
  __imp__rex_SetWaitableTimer_82EE2248(ctx, base);
  if (now - pt.last_report > 10.0) {
    if (pt.last_report > 0.0 && pt.ticks) {
      REXLOG_INFO("precise sim timer: {} ticks, period {:.4f} ms, spin {:.3f} ms/tick, {} resyncs",
                  pt.ticks, period * 1e3, pt.spin_total * 1e3 / double(pt.ticks), pt.resyncs);
    }
    pt.last_report = now;
    pt.ticks = pt.resyncs = 0;
    pt.spin_total = 0.0;
  }
}
