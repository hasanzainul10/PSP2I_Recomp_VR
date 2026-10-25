/* citro3d GE backend for the New 3DS (c3d_ge.c): the PSP's GE drawn on the
 * PICA200. Needs gfxInitDefault and C3D_Init first. */
#ifndef PSP2I_C3D_GE_H
#define PSP2I_C3D_GE_H

#include <stdint.h>
#include <stdio.h>

/* Install the backend (psp_gpu_set_backend) and the VRAM hook. 0 on success. */
int  c3d_ge_init(void);
/* The displayed framebuffer to the top screen (scaled to 400x240), from its
 * render target when the GPU drew it, else from emulated VRAM; ends the
 * citro3d frame. Once per vblank. 0 on success. */
int  c3d_ge_present(uint32_t addr, uint32_t stride, int fmt);
void c3d_ge_report(FILE *out);
/* The displayed framebuffer as a 480x272 PPM at `path` (from its render
 * target when the GPU drew it, else from emulated VRAM). 0 on success. */
int  c3d_ge_capture(uint32_t addr, uint32_t stride, int fmt, const char *path);

#endif
