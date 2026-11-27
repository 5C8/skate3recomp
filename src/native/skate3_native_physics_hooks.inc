// Hook section of skate3_native_physics.cpp (included at its end). Retail addresses; retail
// inlined Simulation::Horus_Pipeline and Simulation::BatchIntegrator into their caller, so their
// leaves (HorusPipeline, RigidBody::DynamicUpdate) are hooked and timed as those stages.

template <class T> T *Host(uint8_t *base, uint32_t addr) { return (T *)(base + addr); }

// ---- per-stage switch: sk83_native_physics_skip lists stages that stay recompiled ----------
bool StageNative(int gate)
{
    if (!REXCVAR_GET(sk83_native_physics)) return false;
    if (gate < 0) return true;
    if (gate >= kStageCount) return false;
    const std::string skip = REXCVAR_GET(sk83_native_physics_skip);
    if (skip.empty()) return true;
    if (skip == "all") return false;
    const std::string name = kStageNames[gate];
    size_t p = 0;
    while (p <= skip.size())
    {
        size_t e = skip.find(',', p);
        if (e == std::string::npos) e = skip.size();
        if (skip.compare(p, e - p, name) == 0 && name.size() == e - p) return false;
        p = e + 1;
    }
    return true;
}

// ---- verify (sk83_native_physics_verify) ----------------------------------------------------
// The recompiled stage runs and its result stays in the game. The native code then recomputes
// the same output into scratch memory (never into game state: the compilers run concurrently on
// several job threads and share the Simulation), and the two are compared word by word.
std::atomic<int64_t> g_verify_next_log[kStageCount];
std::atomic<uint64_t> g_verify_calls[kStageCount], g_verify_bad[kStageCount], g_verify_items[kStageCount];

struct DiffLog
{
    uint64_t bad = 0;
    char text[1024];
    int n = 0;
    // a, b: big-endian bytes (recomp, native); item = jacobian index
    void Compare(const uint8_t *a, const uint8_t *b, uint32_t size, uint32_t item)
    {
        for (uint32_t k = 0; k + 4 <= size; k += 4)
        {
            uint32_t x, y;
            memcpy(&x, a + k, 4);
            memcpy(&y, b + k, 4);
            if (x == y) continue;
            x = __builtin_bswap32(x);
            y = __builtin_bswap32(y);
            const bool nanX = (x & 0x7f800000u) == 0x7f800000u && (x & 0x7fffffu);
            const bool nanY = (y & 0x7f800000u) == 0x7f800000u && (y & 0x7fffffu);
            if (nanX && nanY) continue;
            if (bad++ < 10 && n < (int)sizeof(text) - 96)
                n += snprintf(text + n, sizeof(text) - n, " [#%u +0x%x: recomp %08x native %08x]", item, k, x, y);
        }
    }
};

void VerifyReport(int stage, const DiffLog &d, uint32_t items)
{
    g_verify_calls[stage].fetch_add(1, std::memory_order_relaxed);
    g_verify_items[stage].fetch_add(items, std::memory_order_relaxed);
    if (d.bad) g_verify_bad[stage].fetch_add(1, std::memory_order_relaxed);
    const int64_t now = NowNs();
    int64_t due = g_verify_next_log[stage].load(std::memory_order_relaxed);
    if (now < due) return;
    if (!d.bad && now < due + 8'000'000'000LL) return;
    if (!g_verify_next_log[stage].compare_exchange_strong(due, now + 2'000'000'000LL)) return;
    const uint64_t c = g_verify_calls[stage].exchange(0), b = g_verify_bad[stage].exchange(0),
                   it = g_verify_items[stage].exchange(0);
    if (d.bad)
        REXLOG_WARN("physics verify {}: {} of {} calls differ ({} jacobians checked); this call {} words:{}",
                    kStageNames[stage], b, c, it, d.bad, d.text);
    else
        REXLOG_INFO("physics verify {}: {} of {} calls differ ({} jacobians checked)", kStageNames[stage], b, c, it);
}

// joints / drives: rebuild each jacobian the recompiled compiler wrote into a host scratch slot
template <class Jac, class Obj>
void VerifyBuilders(int stage, uint8_t *base, Simulation *sim, uint32_t stack, uint32_t count)
{
    // The scratch jacobian must be GUEST memory: the builders address their own storage as guest
    // addresses (__dcbz128 clears g_base + (addr & ~127)); a host buffer there truncates to a stray
    // guest address. One 128-aligned slot per job thread.
    thread_local uint32_t gscratch = 0;
    if (!gscratch)
    {
        auto *mem = rex::system::kernel_memory();
        gscratch = mem ? mem->SystemHeapAlloc(0x180, 128) : 0;
        if (!gscratch) return;
    }
    uint8_t *scratch = base + gscratch;
    DiffLog d;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t jac = stack + i * 0x180;
        const uint32_t obj = __builtin_bswap32(*(const uint32_t *)(base + jac + 0xcc));   // m_joint / m_drive
        memset(scratch, 0, 0x180);
        ((Jac *)scratch)->Build(*Host<const Obj>(base, obj), sim);
        d.Compare(base + jac, scratch, 0x180, i);
    }
    VerifyReport(stage, d, count);
}

// contacts: the compiler works in place, so its input is copied below the guest stack first and
// replayed there natively against a copy of the Simulation
struct ContactReplay
{
    uint32_t simCopy = 0, stackCopy = 0, count = 0;
};
ContactReplay ContactSnapshot(uint8_t *base, PPCContext &ctx, Simulation *sim)
{
    ContactReplay r;
    r.count = sim->m_CT_Count;
    const uint32_t bytes = r.count * 0x100;
    if (bytes > 0x10000) return ContactReplay{};
    r.stackCopy = (ctx.r1.u32 - 0x2000 - bytes) & ~127u;
    r.simCopy = (r.stackCopy - 0x300) & ~127u;
    memcpy(base + r.simCopy, sim, 0x300);
    memcpy(base + r.stackCopy, base + RWP_ADDR_OF((RWP_PTR(ContactJacobian))sim->m_CJ_Stack), bytes);
    Host<Simulation>(base, r.simCopy)->m_CJ_Stack = RWP_FROM_ADDR(ContactJacobian, r.stackCopy);
    return r;
}
void ContactReplayCheck(uint8_t *base, Simulation *sim, const ContactReplay &r)
{
    if (!r.stackCopy) return;
    Host<Simulation>(base, r.simCopy)->ContactBatchBuild();
    DiffLog d;
    const uint32_t real = RWP_ADDR_OF((RWP_PTR(ContactJacobian))sim->m_CJ_Stack);
    for (uint32_t i = 0; i < r.count; i++)
        d.Compare(base + real + i * 0x100, base + r.stackCopy + i * 0x100, 0x100, i);
    VerifyReport(kContacts, d, r.count);
}

}  // namespace

// NATIVE(...) runs the decompiled function; the recompiled one runs otherwise. GATE = the
// stage whose switch decides (leaf functions follow their batch stage).
#define RWP_HOOK(NAME, ADDR, STAGE, GATE, NATIVE)                                             \
    extern "C" REX_FUNC(NAME##_##ADDR)                                                        \
    {                                                                                         \
        StageTimer timer(STAGE);                                                              \
        if (StageNative(GATE))                                                                \
        {                                                                                     \
            if (STAGE >= 0) g_native_calls.fetch_add(1, std::memory_order_relaxed);           \
            rwp::g_base = base;                                                               \
            NATIVE;                                                                           \
        }                                                                                     \
        else                                                                                  \
        {                                                                                     \
            if (STAGE >= 0) g_recomp_calls.fetch_add(1, std::memory_order_relaxed);           \
            __imp__##NAME##_##ADDR(ctx, base);                                                \
        }                                                                                     \
    }

#define SIM Host<Simulation>(base, ctx.r3.u32)

// The verified compilers: with verify on, the recompiled stage runs (the game keeps its output)
// and the native one is checked against it in scratch memory.
#define RWP_HOOK_VERIFIED(NAME, ADDR, STAGE, NATIVE, CHECK_BEFORE, CHECK_AFTER)               \
    extern "C" REX_FUNC(NAME##_##ADDR)                                                        \
    {                                                                                         \
        StageTimer timer(STAGE);                                                              \
        rwp::g_base = base;                                                                   \
        if (REXCVAR_GET(sk83_native_physics) && REXCVAR_GET(sk83_native_physics_verify))      \
        {                                                                                     \
            Simulation *sim = SIM;                                                            \
            CHECK_BEFORE;                                                                     \
            g_recomp_calls.fetch_add(1, std::memory_order_relaxed);                           \
            __imp__##NAME##_##ADDR(ctx, base);                                                \
            CHECK_AFTER;                                                                      \
        }                                                                                     \
        else if (StageNative(STAGE))                                                          \
        {                                                                                     \
            g_native_calls.fetch_add(1, std::memory_order_relaxed);                           \
            NATIVE;                                                                           \
        }                                                                                     \
        else                                                                                  \
        {                                                                                     \
            g_recomp_calls.fetch_add(1, std::memory_order_relaxed);                           \
            __imp__##NAME##_##ADDR(ctx, base);                                                \
        }                                                                                     \
    }

RWP_HOOK(rex_rw_physics_Simulation_CreateSimulationIslandTables_QAAXI_Z, 82AE5358, kIslands, kIslands,
         SIM->CreateSimulationIslandTables(0))   // retail has no swapContactStacks argument
RWP_HOOK_VERIFIED(rex_rw_physics_Simulation_ContactBatchBuild_QAAXXZ, 82AE10C8, kContacts, SIM->ContactBatchBuild(),
                  const ContactReplay replay = ContactSnapshot(base, ctx, sim),
                  ContactReplayCheck(base, sim, replay))
RWP_HOOK_VERIFIED(rex_rw_physics_Simulation_JointBatchBuild_QAAXXZ, 82AE39D0, kJoints, SIM->JointBatchBuild(),
                  (void)0,
                  (VerifyBuilders<JointJacobian, Joint>(kJoints, base, sim,
                                                        RWP_ADDR_OF((RWP_PTR(JointJacobian))sim->m_JJ_Stack),
                                                        sim->m_JT_Count)))
RWP_HOOK_VERIFIED(rex_rw_physics_Simulation_DriveBatchBuild_QAAXXZ, 82AE1778, kDrives, SIM->DriveBatchBuild(),
                  (void)0,
                  (VerifyBuilders<DriveJacobian, Drive>(kDrives, base, sim,
                                                        RWP_ADDR_OF((RWP_PTR(DriveJacobian))sim->m_DJ_Stack),
                                                        sim->m_DR_Count)))
RWP_HOOK(rex_rw_physics_Simulation_HorusPipelineIslandUsingIndirectJacobians_QAAXI_Z, 82AE30F0, kIslandIterations,
         kIslandIterations, SIM->HorusPipelineIslandUsingIndirectJacobians(ctx.r4.u32))
RWP_HOOK(rex_rw_physics_Simulation_SpyContactJacobians_QAAXXZ, 82AE1608, kSpyContacts, kSpyContacts,
         SIM->SpyContactJacobians())
RWP_HOOK(rex_rw_physics_Simulation_SpyJointJacobians_QAAXXZ, 82AE3A68, kSpyJoints, kSpyJoints, SIM->SpyJointJacobians())
RWP_HOOK(rex_rw_physics_Simulation_SpyDriveJacobians_QAAXXZ, 82AE1810, kSpyDrives, kSpyDrives, SIM->SpyDriveJacobians())
// leaves, for the recompiled callers (not timed; they follow their batch stage's switch, and stay
// recompiled while verifying so the reference really is the recompiled code)
#define LEAF_GATE(stage) (REXCVAR_GET(sk83_native_physics_verify) ? kStageCount : (stage))
RWP_HOOK(rex_rw_physics_HorusPipeline_YAXIPAVContactJacobian_12_IPAVJointJacobian_12_IPAVDriveJacobian_12_I_Z, 82AE27D0,
         kIterations, kIterations,
         HorusPipeline(ctx.r3.u32, RWP_FROM_ADDR(ContactJacobian, ctx.r4.u32), ctx.r5.u32,
                       RWP_FROM_ADDR(JointJacobian, ctx.r6.u32), ctx.r7.u32,
                       RWP_FROM_ADDR(DriveJacobian, ctx.r8.u32), ctx.r9.u32))
RWP_HOOK(rex_rw_physics_RigidBody_DynamicUpdate_AAAXXZ, 82AE6590, kDynamicUpdate, kDynamicUpdate,
         Host<RigidBody>(base, ctx.r3.u32)->DynamicUpdate())
RWP_HOOK(rex_rw_physics_JointJacobian_Build_AAAXABVJoint_23_PAVSimulation_23_Z, 82AE3BC8, -1, LEAF_GATE(kJoints),
         Host<JointJacobian>(base, ctx.r3.u32)->Build(*Host<const Joint>(base, ctx.r4.u32), Host<Simulation>(base, ctx.r5.u32)))
RWP_HOOK(rex_rw_physics_DriveJacobian_Build_AAAXABVDrive_23_PAVSimulation_23_Z, 82AE1AE8, -1, LEAF_GATE(kDrives),
         Host<DriveJacobian>(base, ctx.r3.u32)->Build(*Host<const Drive>(base, ctx.r4.u32), Host<Simulation>(base, ctx.r5.u32)))
// Jacobian_RQD::Create serves both compilers: native only when both are
RWP_HOOK(rex_rw_physics_Jacobian_RQD_Create_QAAXABVQuaternion_vpu_math_3_0_Z, 82AE0DB0, -1,
         (!REXCVAR_GET(sk83_native_physics_verify) && StageNative(kJoints) && StageNative(kDrives) ? -1 : kStageCount),
         Host<Jacobian_RQD>(base, ctx.r3.u32)->Create(rwp::ldv(base + ctx.r4.u32), rwp::ldv(base + ctx.r5.u32)))
