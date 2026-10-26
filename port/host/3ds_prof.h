/* Where the game's code spends its time on the console (PSP2I_PROFILE builds;
 * 3ds_prof.c). */
#ifndef PSP2I_3DS_PROF_H
#define PSP2I_3DS_PROF_H

#include <stdint.h>

/* the sampler thread on core 2; 0 if it runs */
int psp2i_prof_start(void);

/* The host's parts of a sample (psprecomp/src/hle/prof.h's psp_prof_where:
 * 0x30 present, 0x34 pad and APT, 0x38 the once-a-second report). */
#ifdef PSP2I_PROFILE
extern volatile uint32_t psp_prof_where;
#define PSP2I_PROF_MARK(w) (psp_prof_where = (w))
#define PSP2I_PROF_WHERE() psp_prof_where
#else
#define PSP2I_PROF_MARK(w) ((void)(w))
#define PSP2I_PROF_WHERE() 0u
#endif
/* once a second from time_report: drains the samples, and every 10 s writes
 * that window's table to sdmc:/psp2i/profile.txt */
void psp2i_prof_second(double game_ms, uint64_t frames, double draws_per_frame);

#endif
