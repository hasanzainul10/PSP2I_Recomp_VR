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
 * Costs one store per call and per return.
 *
 * A firmware call also sets psp2i_prof_imp to its import for as long as it
 * runs (saved and put back, so nested calls unwind), and the sampler charges
 * the import while it's set: the runtime calls game code back from inside
 * firmware (the GE's signal and finish callbacks), and without it the GE work
 * after a callback returned was charged to the callback (3DS_PROGRESS.md T4). */
#include "recomp_funcs.h"
#include <psprecomp/hle.h>                  /* declared before the macro below */

extern volatile uint32_t psp2i_prof_fn, psp2i_prof_imp;
/* outside a body (psp_scc_step's dispatch): no caller to name; a body's own
 * _entry parameter shadows this */
static const uint32_t _entry = 0;

#undef PSP_ENTER
#define PSP_ENTER(a) ((void)(psp2i_prof_fn = (a)))
#undef PSP_SP_CALL
#define PSP_SP_CALL(t, a, b) ((void)(psp2i_prof_fn = _entry))
#define psp_dispatch(t) (psp_dispatch(t), (void)(psp2i_prof_fn = _entry))
#define psp_hle_call(nid) do { const uint32_t _pi = psp2i_prof_imp; psp2i_prof_imp = psp2i_prof_fn; \
                               psp_hle_call(nid); psp2i_prof_imp = _pi; } while (0)
