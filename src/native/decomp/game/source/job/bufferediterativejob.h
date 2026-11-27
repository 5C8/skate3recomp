// RETAIL copy (skate3recomp/tools/sk83_port/remap_decomp.py): code addresses remapped dev -> retail (0); comments keep the dev addresses.
// UNRESOLVED (sentinel 0x5fff0000, faults if reached): 82ab71b0 82ab8c68 82ab8fc8 8336d01c
// game/source/job/bufferediterativejob.h (sk83 assert path): Sk8::BufferedIterativeJob, an
// EA::Jobs::Job (base at +0) whose queued work items are handed to the job scheduler in one go.
//   BufferedIterativeJob::Flush   sk83_na_m 0x82974c38 (0xa0)
//   BufferedIterativeJob::Wait    sk83_na_m 0x82974ce0 (0x6c)
// Verified with sk83_na_m --sk83_replay.
#pragma once
#include "rwp_guestcall.h"

namespace Sk8 {
#ifdef RWP_GUEST
using namespace ::rwp;
#endif
#if !defined(RWP_GUEST)
void Job_SetParams(unsigned int job, unsigned int a, unsigned int b, unsigned int c, unsigned int d);
void JobScheduler_AddJobs(unsigned int sched, unsigned int job, int n);
void Job_WaitOn(unsigned int job, unsigned int ctl, unsigned int arg, int timeout);
#endif
#define g_pJobManager RWP_GLOBAL(RWP_U32, 0x5fff0000)     // +8: its JobScheduler

// +0x350 u8: running; +0x358: queued item count; +0x35c: the items
inline void BufferedIterativeJob_Flush(unsigned int self)
{
    RWP_SAVEGPRLR(29);
    RWP_GUEST_FRAME(frame, 0x70);
    const unsigned int n = RWP_LD32(self + 0x358u);
    if (!n)
        return;
    RWP_ST8(self + 0x350u, 1u);
    RWP_CALL(0x5fff0000, Job_SetParams, self, RWP_LD32(self + 0x35cu), n, 0u, 0u);
    // (assert g_pJobManager, jobmanager.h line 130; its scheduler, line 42)
    RWP_CALL(0x5fff0000, JobScheduler_AddJobs, RWP_LD32((unsigned int)g_pJobManager + 8u), self, 1);
    RWP_ST32(self + 0x358u, 0u);
}

inline void BufferedIterativeJob_Wait(unsigned int self)
{
    RWP_GUEST_FRAME(frame, 0x60);
    if (!RWP_LD8(self + 0x350u) && RWP_LD32(self + 0x358u) != 0)
        BufferedIterativeJob_Flush(self);
    if (RWP_LD8(self + 0x350u))
    {
        RWP_CALL(0x5fff0000, Job_WaitOn, self, 0u, 0u, -1);
        RWP_ST8(self + 0x350u, 0u);
    }
}

}
