// Native rw::physics solver job for retail (ported from sk83_na_m src/native/sk83_native_physics.cpp),
// replacing the recompiled functions when sk83_native_physics is on.
//
// The code is the hand-written decomp from REXGLUE/skate-physics-decomp (Skate 2 PDB layouts and
// names), compiled for guest memory (RWP_GUEST): big-endian views of the game's own data, 32-bit
// guest pointers, and the same simde/rex::ppc vector ops the recompiler emits.
//
// Proven against RETAIL's own recompiled functions (retail is an LTCG build): the retail copy of the
// decomp (src/native/decomp, with retail's three runtime rw::math vector globals 0x830bdb40 /
// 0x830bde50 / 0x830bde60) vs skate3recomp/generated on the skate-physics-decomp equivalence scenes
// with the retail xex image: islands, contacts, joints, drives (batch + builders + Jacobian_RQD),
// Horus flat + per island, DynamicUpdate, the three spies -- 2000 scenes each, every word identical
// (drive: NaN sign/payload pairs only, as in the dev proof). See tools/sk83_port/PERF_PORT_2026-11.md.
// Retail differences handled here:
//   - CreateSimulationIslandTables ignores its argument (dev's swapContactStacks): called with 0.
//   - Simulation::Horus_Pipeline and Simulation::BatchIntegrator are inlined into their caller:
//     their leaves (HorusPipeline, RigidBody::DynamicUpdate) are hooked and timed as those stages.
// The natives never write ctx registers, so callers keep their callee-saved state.
// Every stage is timed in both modes; "physics:" log lines every 10 s compare them.

#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define RWP_GUEST 1
#define RW_FMADDS(a, b, c) ((float)std::fma((double)(a), (double)(b), (double)(c)))   // recomp fmadds
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/rigidbody/host/xbox2/rwprigidbody.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpbatchdynamicupdate.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpsimulation.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpcontactbatchcompiler.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwphorus.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpcontactbatchspy.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpjointbatchspy.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpdrivebatchspy.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/types/private_quaternion.hpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpjointbatchcompiler.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpdrivebatchcompiler.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpjointjacobian.cpp"
#include "native/decomp/packages/rw/rwphysics_base/5.05.00/source/physics/core/solver/host/xbox2/rwpdrivejacobian.cpp"

// rwp::g_base is defined in skate3_anim_native.cpp

REXCVAR_DEFINE_BOOL(sk83_native_physics, true, "Skate 3",
                    "Run the physics solver job (rw::physics: island tables, contact/joint/drive compilers, "
                    "Horus iterations, dynamic update, spies) as native decompiled code instead of the "
                    "recompiled functions. Same results bit for bit; faster");
REXCVAR_DEFINE_STRING(sk83_native_physics_skip, "", "Skate 3",
                      "Stages that stay recompiled while sk83_native_physics is on, comma-separated: "
                      "islands,contacts,joints,drives,iter,iter-island,integrate,spy-c,spy-j,spy-d (or all)");
REXCVAR_DEFINE_BOOL(sk83_native_physics_verify, false, "Skate 3",
                    "DIAGNOSTIC: run the contact/joint/drive compilers both ways on the same input and "
                    "log any difference ('physics verify' lines); the native result is kept");
REXCVAR_DEFINE_BOOL(sk83_native_physics_stats, true, "Skate 3",
                    "Log per-stage physics solver times every 10 s (both modes)");

namespace {

using namespace rw::physics;

enum Stage
{
    kIslands, kContacts, kJoints, kDrives, kIterations, kIslandIterations, kDynamicUpdate,
    kSpyContacts, kSpyJoints, kSpyDrives, kStageCount
};
const char *const kStageNames[kStageCount] = {
    "islands", "contacts", "joints", "drives", "iter", "iter-island", "integrate",
    "spy-c", "spy-j", "spy-d"};

struct StageStats
{
    std::atomic<uint64_t> ns{0};
    std::atomic<uint64_t> calls{0};
};
std::array<StageStats, kStageCount> g_stats;
std::atomic<int64_t> g_next_log{0};
std::atomic<uint64_t> g_native_calls{0}, g_recomp_calls{0};

int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void MaybeLog(int64_t now)
{
    int64_t due = g_next_log.load(std::memory_order_relaxed);
    if (due == 0)
    {
        g_next_log.compare_exchange_strong(due, now + 10'000'000'000LL);
        return;
    }
    if (now < due || !g_next_log.compare_exchange_strong(due, now + 10'000'000'000LL))
        return;
    char line[512];
    int n = 0;
    uint64_t total = 0;
    for (int i = 0; i < kStageCount; i++)
    {
        uint64_t ns = g_stats[i].ns.exchange(0), c = g_stats[i].calls.exchange(0);
        total += ns;
        if (c)
            n += snprintf(line + n, sizeof(line) - n, " %s %.1f", kStageNames[i], double(ns) / double(c) / 1000.0);
    }
    uint64_t nat = g_native_calls.exchange(0), rec = g_recomp_calls.exchange(0);
    REXLOG_INFO("physics: {} | {:.2f} ms/s total | per call (us):{}",
                nat && rec ? "mixed" : nat ? "NATIVE" : "recompiled", total / 1e7, line);
}

struct StageTimer
{
    int stage;
    int64_t t0;
    explicit StageTimer(int s) : stage(s), t0(s >= 0 && REXCVAR_GET(sk83_native_physics_stats) ? NowNs() : 0) {}
    ~StageTimer()
    {
        if (!t0)
            return;
        int64_t t1 = NowNs();
        g_stats[stage].ns.fetch_add(uint64_t(t1 - t0), std::memory_order_relaxed);
        g_stats[stage].calls.fetch_add(1, std::memory_order_relaxed);
        MaybeLog(t1);
    }
};

#include "native/skate3_native_physics_hooks.inc"
