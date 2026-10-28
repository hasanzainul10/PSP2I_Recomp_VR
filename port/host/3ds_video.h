/* 3ds_video.h: movies through the New 3DS's H.264 decoder (see 3ds_video.c). */
#ifndef PSP2I_3DS_VIDEO_H
#define PSP2I_3DS_VIDEO_H

/* Sets the runtime's video codec on a New 3DS. -1: none (movies play black
 * with sound, as before). */
int psp2i_video_3ds_init(void);

#endif
