/* The game's code in a profiling build (PSP2I_PROFILE, port/3ds/CMakeLists.txt):
 * force-included before each recompiled file, it keeps psp2i_prof_fn on the
 * function the CPU is in, for the sampler in port/host/3ds_prof.c.
 *
 * The generated code already marks every function entry (PSP_ENTER, also in
 * the firmware import thunks) and every return from a direct call
 * (PSP_SP_CALL); off, both are ((void)0). Here an entry stores the function's
 * address and a return stores the caller's entry point back (_entry, which
 * every body takes), so a sample lands on the function whose own code runs,
 * memory helpers included, and firmware calls land on their import. Indirect
 * calls go through psp_dispatch, which gets the same store after it.
 * Costs two stores per call and one per return.
 *
 * Imports are called without a PSP_SP_CALL after them, so the thunk's
 * psp_hle_call puts the caller back itself (psp_prof_prev, kept by every
 * entry): without it the caller's code after a firmware call was charged to
 * the import. The runtime (PSPRECOMP_PROF, psprecomp/src/hle/prof.h) marks its
 * own work, GE lists and scheduler events, and puts psp_prof_fn back after the
 * game code it calls back (the GE's signal and finish callbacks). */
#include "recomp_funcs.h"
#include <psprecomp/hle.h>                  /* declared before the macro below */

extern volatile uint32_t psp_prof_fn, psp_prof_prev;
/* outside a body (psp_scc_step's dispatch): no caller to name; a body's own
 * _entry parameter shadows this */
static const uint32_t _entry = 0;

#undef PSP_ENTER
#define PSP_ENTER(a) ((void)(psp_prof_prev = psp_prof_fn, psp_prof_fn = (a)))
#undef PSP_SP_CALL
#define PSP_SP_CALL(t, a, b) ((void)(psp_prof_fn = _entry))
#define psp_dispatch(t) (psp_dispatch(t), (void)(psp_prof_fn = _entry))
/* in a thunk, right after its PSP_ENTER: psp_prof_prev is the caller */
#define psp_hle_call(nid) do { const uint32_t _pc = psp_prof_prev; psp_hle_call(nid); psp_prof_fn = _pc; } while (0)
