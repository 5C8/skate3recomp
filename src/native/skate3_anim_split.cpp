// Split animation fetch for retail (ported from sk83_na_m src/native/sk83_native_anim.cpp, split
// part only; the native PoseWithMatMult / sqtBlend / extract decomps are NOT ported: retail's
// LTCG compile reorders some of their multiply-adds, so they need retail-specific proofs first).
//
// Retail Andale::Vector_ExtractPackedNVBR = sub_82D1DE08 (dev 0x82FEB8B0): same arguments (r3
// ExtractType records, r4 count, r5 VBRDecompressor), same 8176-byte frame, same codec callees
// (InitPerAnim 82E88FA8, TwoConsec 82E89B80, DecompressFrame 82E89A60, TrajectoryCalculation,
// XMemCpy; GetCurrentProcessorNumber 82EE2110 = the codec slot). The decompressor layout used
// below (slots +9264 x 64, quant tables +9648 x 2052, size 0x55d0) is the one retail's own codec
// listings use. Parts always run the RECOMPILED retail extract; sk83_anim_split_verify re-runs
// every split fetch unsplit and compares, which proves the split against retail's own function.
// No living-world-job pre-wake hook here (sk83_anim_split_prewake_us defaults to 0 in both).
#include "generated/skate3_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <immintrin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#define WIN32_LEAN_AND_MEAN
#include <immintrin.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>


REXCVAR_DEFINE_BOOL(sk83_anim_split_fetch, true, "Skate 3",
                    "Split the big animation fetch (all pedestrians in one Vector_ExtractPackedNVBR call) "
                    "between the calling job thread and helper threads on the unused codec slot 4");
// Both default to 0 (2026-11-17): spinning helpers slowed the render thread more than the wake
// latency they hide -- interleaved Downtown timelines, spin 1000/300 vs 0/0: frame median
// 3719 -> 3401 us, render span after the LW wait -9%, LW job itself no slower.
REXCVAR_DEFINE_INT32(sk83_anim_split_prewake_us, 0, "Skate 3",
                     "How long the fetch-split helpers spin after the living-world job starts (us) "
                     "before sleeping");
REXCVAR_DEFINE_INT32(sk83_anim_split_spin_us, 0, "Skate 3",
                     "How long the fetch-split helpers keep spinning after each split fetch (us) "
                     "before sleeping (spinning costs the render thread more than it saves)");
REXCVAR_DEFINE_INT32(sk83_anim_split_ways, 6, "Skate 3",
                     "How many parts sk83_anim_split_fetch cuts the big animation fetch into (2-4)");
// Proven 2026-11-17 against retail's recompiled extract on identical private decompressor state:
// 40k fetches, 1.84G output words, 0 differences (fixture sanity: 62% of the words were written).
// Default OFF on retail anyway: retail's LTCG extract (no asserts, its codec calls native) already
// costs the same per call as the native one (65-98 us with the split, both ways), and the A/B
// showed no gain.
REXCVAR_DEFINE_BOOL(sk83_native_anim_extract, false, "Skate 3",
                    "Native Andale::Vector_ExtractPackedNVBR (the animation fetch) instead of retail's "
                    "recompiled one (sk83_native_anim_extract_verify re-checks it).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_native_anim_extract_verify, false, "Skate 3",
                    "DIAGNOSTIC: before every fetch, run retail's recompiled extract and the native one on "
                    "two identical private decompressor copies and compare every output word (the real "
                    "call then runs normally).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(sk83_anim_split_verify, false, "Skate 3",
                    "DIAGNOSTIC: re-run every split fetch unsplit and compare the decoded bones (logs counts)");

namespace {

enum { kPose, kSqtBlend, kExtract, kCount };
const char *const kNames[kCount] = {"PoseWithMatMult", "ACSsqtBlend_job", "ExtractPackedNVBR"};
struct Stat
{
    std::atomic<uint64_t> ns{0}, calls{0}, native{0};
};
std::array<Stat, kCount> g_stats;
std::atomic<int64_t> g_next_log{0};

int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Account(int which, int64_t t0, bool native)
{
    const int64_t t1 = NowNs();
    g_stats[which].ns.fetch_add(uint64_t(t1 - t0), std::memory_order_relaxed);
    g_stats[which].calls.fetch_add(1, std::memory_order_relaxed);
    if (native) g_stats[which].native.fetch_add(1, std::memory_order_relaxed);
    int64_t due = g_next_log.load(std::memory_order_relaxed);
    if (due == 0) { g_next_log.compare_exchange_strong(due, t1 + 10'000'000'000LL); return; }
    if (t1 < due || !g_next_log.compare_exchange_strong(due, t1 + 10'000'000'000LL)) return;
    char line[384];
    int n = 0;
    for (int i = 0; i < kCount; i++)
    {
        const uint64_t ns = g_stats[i].ns.exchange(0), c = g_stats[i].calls.exchange(0),
                       nat = g_stats[i].native.exchange(0);
        if (c)
            n += snprintf(line + n, sizeof(line) - n, " | %s %s %.2f us/call %.2f ms/s", kNames[i],
                          nat == c ? "NATIVE" : nat ? "mixed" : "recompiled", double(ns) / double(c) / 1000.0,
                          double(ns) / 1e7);
    }
    if (n) REXLOG_INFO("anim:{}", line);
}

}  // namespace


void Skate3NativeExtract(PPCContext &c, uint8_t *base, uint32_t types, uint32_t n, uint32_t dec);  // skate3_anim_native.cpp

namespace {

// One extract over [types, types + n) on the given context (the native decomp or the recompiled
// function, per sk83_native_anim_extract).
void RunExtract(PPCContext &c, uint8_t *base, uint32_t types, uint32_t n, uint32_t dec)
{
    if (REXCVAR_GET(sk83_native_anim_extract))
    {
        Skate3NativeExtract(c, base, types, n, dec);
        return;
    }
    c.r3.u64 = types;
    c.r4.u64 = n;
    c.r5.u64 = dec;
    __imp__sub_82D1DE08(c, base);
}

// ---- Split fetch -------------------------------------------------------------------------------
//
// The living-world presentation job fetches every pedestrian's animation in ONE
// Vector_ExtractPackedNVBR call (~150 ExtractType records over ~25 characters, ~230 us), twice a
// frame, and the render thread waits on it while the job threads sit idle. A record only depends
// on earlier records that share its SQT buffer (the offset-0 record resets the buffer, extraOut
// copies the whole buffer), and those come as one contiguous run, so the call splits at run
// boundaries into up to kMaxWays parts: the caller decodes the first, helper host threads the rest.
//
// The VBR codec keeps its per-call state in per-processor slots of the decompressor
// (mWorkBuffers[tid] at +9264 + tid * 64 and the buffers it points to, the quant table at
// +9648 + tid * 2052), tid = GetCurrentProcessorNumber = the guest PCR byte +268; everything else
// in the decompressor is only read (InitPerAnim / DecompressFrame / the block decoders write
// nothing outside their slot). The game's threads run on processors 0, 1, 2, 3 and 5, so slot 4
// is free: every helper runs with a PCR copy whose processor byte says 4. Helper 0 uses the real
// decompressor's slot 4; the others each use a private copy of the decompressor whose slot 4
// points at private scratch, with the read-only part refreshed from the real one before each use.
// Which slot a call lands on is already arbitrary in the game (the fetch job runs on whichever
// job thread picks it up), so a slot's leftover state between calls is nothing new either.
constexpr uint8_t kHelperTid = 4;
constexpr uint32_t kHelperStack = 0x10000;
constexpr uint32_t kMinSplit = 32;     // records; the small fetches are not worth a handoff
constexpr uint32_t kMinPart = 4;       // records per part
constexpr int kMaxWays = 8;
constexpr int kMaxHelpers = kMaxWays - 1;
// Decompressor layout (VBRDecompressor, size 0x55d0)
constexpr uint32_t kDecSize = 0x55d0, kSlots = 9264, kQuant = 9648, kQuantEnd = 9648 + 6 * 2052;
constexpr uint32_t kSlot4 = kSlots + kHelperTid * 64;
// Slot 4 scratch of a private copy: header object (+0), normalized data (+0x1c) with the block
// scratch (+4) after it, const data (+0x20), isConstant flags (+0x24). Sizes are generous.
constexpr uint32_t kHdrSize = 0x40, kNormSize = 0x8000, kConstSize = 0x1000, kIsConstSize = 0x400;

struct HelperWork
{
    PPCContext ctx;
    uint32_t types, n, dec;
};

struct alignas(64) Helper
{
    std::atomic<uint32_t> seq{0};    // bumped by the caller to post work
    std::atomic<uint32_t> done{0};   // set to the seq when the helper finished it
    std::atomic<uint32_t> wake{0};   // bumped by every post and pre-wake; the helper sleeps on it
    HelperWork work;
    uint32_t stack = 0, pcr = 0;
    uint32_t dec_copy = 0;           // 0: use the real decompressor (helper 0)
};

Helper g_helpers[kMaxHelpers];
int g_num_helpers = 0;
std::atomic<bool> g_split_busy{false};          // one split at a time (other fetches run unsplit)
uint8_t *g_helper_base = nullptr;
std::atomic<int> g_split_state{0};              // 0 unknown, 1 ready, -1 disabled
std::atomic<int64_t> g_helper_awake_until{0};   // NowNs() deadline the helpers keep spinning to

// Wakes helpers [0, count) (all when count < 0); only the ones given a part need to run.
void WakeHelpers(int64_t until, int count = -1)
{
    g_helper_awake_until.store(until, std::memory_order_relaxed);
    const int n = count < 0 ? g_num_helpers : std::min(count, g_num_helpers);
    for (int i = 0; i < n; ++i)
    {
        g_helpers[i].wake.fetch_add(1, std::memory_order_release);
        g_helpers[i].wake.notify_one();
    }
}

void HelperMain(Helper *h)
{
    uint8_t *const base = g_helper_base;
    uint32_t seen = 0;
    for (;;)
    {
        // Spin while a living-world job is running (it posts 2-3 fetches within ~1 ms; see the
        // sub_82948e50 hook) and briefly after each fetch, else sleep until posted.
        const uint32_t wake = h->wake.load(std::memory_order_acquire);
        uint32_t seq = h->seq.load(std::memory_order_acquire);
        for (int spin = 0; seq == seen; ++spin)
        {
            if ((spin & 63) == 0 && NowNs() > g_helper_awake_until.load(std::memory_order_relaxed))
                break;
            _mm_pause();
            seq = h->seq.load(std::memory_order_acquire);
        }
        if (seq == seen)
        {
            h->wake.wait(wake, std::memory_order_acquire);
            continue;
        }
        seen = seq;
        HelperWork &w = h->work;
        uint32_t dec = w.dec;
        if (h->dec_copy)
        {
            // Refresh the read-only part of the private copy; its slot 4 and quant tables stay.
            std::memcpy(base + h->dec_copy, base + dec, kSlots);
            std::memcpy(base + h->dec_copy + kQuantEnd, base + dec + kQuantEnd, kDecSize - kQuantEnd);
            dec = h->dec_copy;
        }
        _mm_setcsr(w.ctx.fpscr.csr);   // the guest thread's FPU/VMX mode (FTZ/DAZ, rounding)
        RunExtract(w.ctx, base, w.types, w.n, dec);
        h->done.store(seq, std::memory_order_release);
    }
}

// A private decompressor copy for helpers 1+: the real one's bytes, slot 4 pointing at fresh
// scratch with the same internal layout as the real slot 4 (block scratch inside the
// normalized-data region at the same offset).
uint32_t MakeDecCopy(uint8_t *base, uint32_t dec)
{
    // Retail places the slot's block scratch (+4) BEFORE its normalized-data buffer (+0x1c),
    // 0x2d30 bytes apart (dev: after it, inside it); keep whatever relative placement the real
    // slot 4 has, in one private region.
    auto *mem = rex::system::kernel_memory();
    const uint32_t norm = REX_LOAD_U32(dec + kSlot4 + 0x1c), blk = REX_LOAD_U32(dec + kSlot4 + 4);
    if (!mem || norm == 0 || blk == 0) return 0;
    const uint32_t lo = std::min(norm, blk);
    const uint32_t gap = std::max(norm, blk) - lo;
    if (gap >= 0x10000) return 0;
    const uint32_t span = gap + kNormSize;
    const uint32_t copy = mem->SystemHeapAlloc(kDecSize, 0x1000);
    const uint32_t hdr = mem->SystemHeapAlloc(kHdrSize, 0x80);
    const uint32_t nbuf = mem->SystemHeapAlloc(span, 0x1000);
    const uint32_t cbuf = mem->SystemHeapAlloc(kConstSize, 0x1000);
    const uint32_t ibuf = mem->SystemHeapAlloc(kIsConstSize, 0x1000);
    if (!copy || !hdr || !nbuf || !cbuf || !ibuf) return 0;
    std::memcpy(base + copy, base + dec, kDecSize);
    std::memset(base + hdr, 0, kHdrSize);   // header cache: no animation initialised yet
    std::memset(base + nbuf, 0, span);
    std::memset(base + cbuf, 0, kConstSize);
    std::memset(base + ibuf, 0, kIsConstSize);
    REX_STORE_U32(copy + kSlot4 + 0x00, hdr);
    REX_STORE_U32(copy + kSlot4 + 0x04, nbuf + (blk - lo));
    REX_STORE_U32(copy + kSlot4 + 0x1c, nbuf + (norm - lo));
    REX_STORE_U32(copy + kSlot4 + 0x20, cbuf);
    REX_STORE_U32(copy + kSlot4 + 0x24, ibuf);
    return copy;
}

bool SplitReady(uint8_t *base, uint32_t dec)
{
    int st = g_split_state.load(std::memory_order_acquire);
    if (st != 0) return st > 0;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    st = g_split_state.load();
    if (st != 0) return st > 0;
    // The helpers' codec slot must be set up (header object + scratch pointers).
    const uint32_t wb = dec + kSlot4;
    if (REX_LOAD_U32(wb + 0) == 0 || REX_LOAD_U32(wb + 4) == 0)
    {
        REXLOG_WARN("anim: split fetch disabled, decompressor slot {} not set up", kHelperTid);
        g_split_state.store(-1);
        return false;
    }
    for (int s = 0; s < 6; ++s)
        REXLOG_INFO("anim: decompressor {:08x} slot {}: hdr {:08x} +4 {:08x} +1c {:08x} +20 {:08x} +24 {:08x}",
                    dec, s, REX_LOAD_U32(dec + kSlots + s * 64), REX_LOAD_U32(dec + kSlots + s * 64 + 4),
                    REX_LOAD_U32(dec + kSlots + s * 64 + 0x1c), REX_LOAD_U32(dec + kSlots + s * 64 + 0x20),
                    REX_LOAD_U32(dec + kSlots + s * 64 + 0x24));
    auto *mem = rex::system::kernel_memory();
    g_helper_base = base;
    int n = 0;
    for (; n < kMaxHelpers; ++n)
    {
        Helper &h = g_helpers[n];
        h.stack = mem ? mem->SystemHeapAlloc(kHelperStack, 0x1000) : 0;
        h.pcr = mem ? mem->SystemHeapAlloc(0x1000, 0x1000) : 0;
        h.dec_copy = MakeDecCopy(base, dec);  // retail: every helper (slot 4 is a game thread's)
        if (!h.stack || !h.pcr || !h.dec_copy) break;
    }
    if (n == 0)
    {
        g_split_state.store(-1);
        return false;
    }
    g_num_helpers = n;
    for (int i = 0; i < n; ++i) std::thread(HelperMain, &g_helpers[i]).detach();
    REXLOG_INFO("anim: split fetch up to {} ways ({} helpers on codec slot {})", n + 1, n, kHelperTid);
    g_split_state.store(1, std::memory_order_release);
    return true;
}

// Cuts the records into up to `ways` parts at SQT-buffer run boundaries near the even split
// points. Returns the part count (1 = do not split); cuts[k] is where part k starts (cuts[0] = 0,
// cuts[parts] = n). Every buffer's records must land in one part, and an extraOut that names a
// buffer must be in that buffer's part.
int SplitPoints(uint8_t *base, uint32_t types, uint32_t n, int ways, uint32_t *cuts)
{
    if (n > 512 || ways < 2) return 1;
    uint32_t buf[512];
    for (uint32_t i = 0; i < n; ++i) buf[i] = REX_LOAD_U32(types + i * 0x40 + 0x20);
    int parts = 0;
    cuts[0] = 0;
    for (int k = 1; k < ways; ++k)
    {
        uint32_t c = std::max<uint32_t>(uint32_t(uint64_t(n) * k / ways), cuts[parts] + kMinPart);
        while (c < n && buf[c] == buf[c - 1]) ++c;
        if (c + kMinPart > n) break;
        cuts[++parts] = c;
    }
    cuts[++parts] = n;
    if (parts < 2) return 1;
    // Runs: (buffer, part). A buffer may repeat only within its own part.
    uint32_t run_buf[512], run_part[512];
    uint32_t nruns = 0;
    for (int p = 0; p < parts; ++p)
        for (uint32_t i = cuts[p]; i < cuts[p + 1]; ++i)
            if (i == cuts[p] || buf[i] != buf[i - 1])
            {
                run_buf[nruns] = buf[i];
                run_part[nruns++] = uint32_t(p);
            }
    for (uint32_t a = 0; a < nruns; ++a)
        for (uint32_t b = a + 1; b < nruns; ++b)
            if (run_buf[a] == run_buf[b] && run_part[a] != run_part[b]) return 1;
    for (int p = 0; p < parts; ++p)
        for (uint32_t i = cuts[p]; i < cuts[p + 1]; ++i)
        {
            const uint32_t eo = REX_LOAD_U32(types + i * 0x40 + 0x24);
            if (eo == 0) continue;
            for (uint32_t r = 0; r < nruns; ++r)
                if (run_buf[r] == eo && run_part[r] != uint32_t(p)) return 1;
        }
    return parts;
}


// ---- native extract verify (sk83_native_anim_extract_verify) -----------------------------------
// Two private decompressor copies A and B (MakeDecCopy: their slot 4 points at private buffers).
// Before a check, A's read-only part is refreshed from the real decompressor and B becomes an exact
// copy of A (decompressor bytes, slot-4 scalars and its buffers' contents). Retail's recompiled
// extract then runs on A and the native one on B, both through a PCR copy naming slot 4, from the
// same output contents (snapshot / restore); every output word is compared. The game's own codec
// slots are never touched, and the real call runs afterwards as usual.
struct VCopy {
    uint32_t dec = 0, hdr = 0, nbase = 0, nspan = 0, cbuf = 0, ibuf = 0, blk_off = 0, norm_off = 0;
};
bool VCopyInit(uint8_t *base, uint32_t real_dec, VCopy &v)
{
    v.dec = MakeDecCopy(base, real_dec);
    if (!v.dec) return false;
    v.hdr = REX_LOAD_U32(v.dec + kSlot4 + 0x00);
    const uint32_t blk = REX_LOAD_U32(v.dec + kSlot4 + 0x04), norm = REX_LOAD_U32(v.dec + kSlot4 + 0x1c);
    v.nbase = std::min(blk, norm);
    v.blk_off = blk - v.nbase;
    v.norm_off = norm - v.nbase;
    v.nspan = std::max(blk, norm) - v.nbase + kNormSize;
    v.cbuf = REX_LOAD_U32(v.dec + kSlot4 + 0x20);
    v.ibuf = REX_LOAD_U32(v.dec + kSlot4 + 0x24);
    return true;
}
void VCopySync(uint8_t *base, const VCopy &a, const VCopy &b)
{
    std::memcpy(base + b.dec, base + a.dec, kDecSize);
    REX_STORE_U32(b.dec + kSlot4 + 0x00, b.hdr);
    REX_STORE_U32(b.dec + kSlot4 + 0x04, b.nbase + b.blk_off);
    REX_STORE_U32(b.dec + kSlot4 + 0x1c, b.nbase + b.norm_off);
    REX_STORE_U32(b.dec + kSlot4 + 0x20, b.cbuf);
    REX_STORE_U32(b.dec + kSlot4 + 0x24, b.ibuf);
    std::memcpy(base + b.hdr, base + a.hdr, kHdrSize);
    std::memcpy(base + b.nbase, base + a.nbase, a.nspan);
    std::memcpy(base + b.cbuf, base + a.cbuf, kConstSize);
    std::memcpy(base + b.ibuf, base + a.ibuf, kIsConstSize);
    // the header cache (W+0 -> hdr) holds pointers into shared animation data: copied as is
}

void VerifyNativeExtract(PPCContext &ctx, uint8_t *base, uint32_t types, uint32_t n, uint32_t dec)
{
    static std::mutex mu;
    static VCopy va, vb;
    static uint32_t pcr = 0;
    static bool ready = false, failed = false;
    static std::atomic<uint64_t> s_calls{0}, s_bad_calls{0}, s_bad_words{0}, s_words{0}, s_changed{0};
    std::lock_guard<std::mutex> lock(mu);
    if (failed || n == 0 || n > 512 || dec >= 0xE0000000u) return;
    if (!ready)
    {
        auto *mem = rex::system::kernel_memory();
        pcr = mem ? mem->SystemHeapAlloc(0x1000, 0x1000) : 0;
        if (!pcr || !VCopyInit(base, dec, va) || !VCopyInit(base, dec, vb))
        {
            failed = true;
            REXLOG_WARN("anim: native extract verify could not set up its decompressor copies");
            return;
        }
        ready = true;
    }
    // A: refresh the read-only part from the real decompressor (as the split helpers do); B = A.
    std::memcpy(base + va.dec, base + dec, kSlots);
    std::memcpy(base + va.dec + kQuantEnd, base + dec + kQuantEnd, kDecSize - kQuantEnd);
    VCopySync(base, va, vb);
    // Outputs: every record's SQT buffer and extraOut (numBones SQTs of 0x30 bytes).
    std::vector<uint32_t> addr, len;
    std::vector<uint8_t> before;
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint32_t rec = types + i * 0x40;
        const uint32_t sz = uint32_t(REX_LOAD_U16(rec + 0x3c)) * 0x30;
        for (uint32_t b : {REX_LOAD_U32(rec + 0x20), REX_LOAD_U32(rec + 0x24)})
        {
            if (b == 0 || sz == 0 || b >= 0xE0000000u) continue;
            addr.push_back(b);
            len.push_back(sz);
            before.insert(before.end(), base + b, base + b + sz);
        }
    }
    auto snap = [&](std::vector<uint8_t> &out) {
        out.clear();
        for (size_t k = 0; k < addr.size(); ++k) out.insert(out.end(), base + addr[k], base + addr[k] + len[k]);
    };
    auto restore = [&]() {
        size_t o = 0;
        for (size_t k = 0; k < addr.size(); ++k) { std::memcpy(base + addr[k], before.data() + o, len[k]); o += len[k]; }
    };
    std::memcpy(base + pcr, base + ctx.r13.u32, 0x400);
    base[pcr + 268] = kHelperTid;
    std::vector<uint8_t> g, m;
    PPCContext ca = ctx;
    ca.r13.u64 = pcr;
    ca.r3.u64 = types;
    ca.r4.u64 = n;
    ca.r5.u64 = va.dec;
    __imp__sub_82D1DE08(ca, base);
    snap(g);
    restore();
    PPCContext cb = ctx;
    cb.r13.u64 = pcr;
    Skate3NativeExtract(cb, base, types, n, vb.dec);
    snap(m);
    restore();
    uint64_t bad = 0;
    size_t first = SIZE_MAX;
    for (size_t w = 0; w + 4 <= g.size(); w += 4)
        if (std::memcmp(&g[w], &m[w], 4) != 0) { if (first == SIZE_MAX) first = w; ++bad; }
    const uint64_t c = s_calls.fetch_add(1) + 1;
    s_words += g.size() / 4;
    for (size_t w = 0; w + 4 <= g.size(); w += 4)
        if (std::memcmp(&g[w], &before[w], 4) != 0) ++s_changed;  // fixture sanity: the run really wrote
    if (bad)
    {
        s_bad_words += bad;
        if (s_bad_calls.fetch_add(1) < 10)
        {
            auto be = [](const std::vector<uint8_t> &v, size_t at) { return (uint32_t(v[at]) << 24) | (uint32_t(v[at + 1]) << 16) | (uint32_t(v[at + 2]) << 8) | v[at + 3]; };
            REXLOG_WARN("anim: native extract verify DIFF n {}: {} words, first at +{}: recompiled {:08X} native {:08X}",
                        n, bad, first, be(g, first), be(m, first));
        }
    }
    if ((c & 255) == 0)
        REXLOG_INFO("anim: native extract verify {} calls, {} with differences ({} of {} words; {} words "
                    "changed by the decode)", c, s_bad_calls.load(), s_bad_words.load(), s_words.load(),
                    s_changed.load());
}

}  // namespace

extern "C" REX_FUNC(sub_82D1DE08)
{
    const int64_t t0 = NowNs();
    const uint32_t types = ctx.r3.u32, n = ctx.r4.u32, dec = ctx.r5.u32;
    const bool native = REXCVAR_GET(sk83_native_anim_extract);
    if (REXCVAR_GET(sk83_native_anim_extract_verify))
        VerifyNativeExtract(ctx, base, types, n, dec);
    // RETAIL: a game thread (guest thread 10) runs on processor 4 and uses the codec's slot 4, so
    // unlike dev every helper works on a private decompressor copy; the real one's slots are never
    // touched by a helper, and that thread's fetches need no special handling.
    uint32_t cuts[kMaxWays + 1];
    int parts = 1;
    bool expected = false;
    const int ways = std::clamp(REXCVAR_GET(sk83_anim_split_ways), 1, kMaxWays);
    if (n >= kMinSplit && ways > 1 && dec < 0xE0000000u && REXCVAR_GET(sk83_anim_split_fetch) && SplitReady(base, dec) &&
        (parts = SplitPoints(base, types, n, std::min(ways, g_num_helpers + 1), cuts)) > 1 &&
        g_split_busy.compare_exchange_strong(expected, true) &&
        (g_split_state.load() > 0 || (g_split_busy.store(false), parts = 1, false)))
    {
        // Post parts 1..: the caller's context with the helper's own guest stack and a PCR copy
        // that names the helpers' codec slot.
        uint32_t seqs[kMaxHelpers];
        for (int p = 1; p < parts; ++p)
        {
            Helper &h = g_helpers[p - 1];
            std::memcpy(base + h.pcr, base + ctx.r13.u32, 0x400);
            base[h.pcr + 268] = kHelperTid;
            HelperWork &w = h.work;
            w.ctx = ctx;
            w.ctx.r1.u64 = h.stack + kHelperStack - 0x100;
            w.ctx.r13.u64 = h.pcr;
            w.types = types + cuts[p] * 0x40;
            w.n = cuts[p + 1] - cuts[p];
            w.dec = dec;
            seqs[p - 1] = h.seq.fetch_add(1, std::memory_order_release) + 1;
        }
        WakeHelpers(NowNs() + int64_t(std::clamp(REXCVAR_GET(sk83_anim_split_spin_us), 0, 5000)) * 1000, parts - 1);
        RunExtract(ctx, base, types, cuts[1], dec);
        for (int p = 1; p < parts; ++p)
            while (g_helpers[p - 1].done.load(std::memory_order_acquire) != seqs[p - 1]) _mm_pause();
        g_split_busy.store(false, std::memory_order_release);
        if (REXCVAR_GET(sk83_anim_split_verify))
        {
            // Snapshot every record's output range, decode again unsplit, compare.
            static std::atomic<uint64_t> s_calls{0}, s_bad_calls{0}, s_bad_words{0}, s_words{0}, s_parts{0};
            std::vector<uint32_t> addr, len;
            std::vector<uint8_t> snap;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t rec = types + i * 0x40;
                const uint32_t b = REX_LOAD_U32(rec + 0x20), sz = uint32_t(REX_LOAD_U16(rec + 0x3c)) * 0x30;
                addr.push_back(b);
                len.push_back(sz);
                snap.insert(snap.end(), base + b, base + b + sz);
                if (const uint32_t eo = REX_LOAD_U32(rec + 0x24))
                {
                    addr.push_back(eo);
                    len.push_back(sz);
                    snap.insert(snap.end(), base + eo, base + eo + sz);
                }
            }
            RunExtract(ctx, base, types, n, dec);
            size_t off = 0;
            uint64_t bad = 0, words = 0;
            for (size_t k = 0; k < addr.size(); ++k)
            {
                for (uint32_t w = 0; w < len[k]; w += 4, ++words)
                    if (std::memcmp(base + addr[k] + w, snap.data() + off + w, 4) != 0) ++bad;
                off += len[k];
            }
            const uint64_t c = s_calls.fetch_add(1) + 1;
            s_words += words;
            s_parts += uint64_t(parts);
            if (bad) { s_bad_calls++; s_bad_words += bad; }
            if ((c & 255) == 0)
                REXLOG_INFO("anim: split verify {} calls ({:.2f} parts avg), {} with differences ({} of {} words)",
                            c, double(s_parts.load()) / double(c), s_bad_calls.load(), s_bad_words.load(),
                            s_words.load());
        }
    }
    else
    {
        RunExtract(ctx, base, types, n, dec);
    }
    Account(kExtract, t0, native);
}
