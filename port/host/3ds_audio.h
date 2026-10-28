/* 3ds_audio.h: sound out through ndsp (see 3ds_audio.c). */
#ifndef PSP2I_3DS_AUDIO_H
#define PSP2I_3DS_AUDIO_H

#include <stdio.h>

/* Starts ndsp and sets the runtime's audio sink. -1: silent (no DSP firmware,
 * or sdmc:/psp2i/nosound), the game runs as before. */
int  psp2i_audio_3ds_init(void);
/* One line: blocks played and dropped (for the 10 s log report). */
void psp2i_audio_3ds_report(FILE *out);
void psp2i_audio_3ds_exit(void);

#endif
