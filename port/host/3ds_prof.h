/* Where the game's code spends its time on the console (PSP2I_PROFILE builds;
 * 3ds_prof.c). */
#ifndef PSP2I_3DS_PROF_H
#define PSP2I_3DS_PROF_H

#include <stdint.h>

/* the sampler thread on core 2; 0 if it runs */
int psp2i_prof_start(void);
/* once a second from time_report: drains the samples, and every 10 s writes
 * that window's table to sdmc:/psp2i/profile.txt */
void psp2i_prof_second(double game_ms, uint64_t frames, double draws_per_frame);

#endif
