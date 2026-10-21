/* VR through OpenXR.
 *
 * Phase 1, the theater: the game's finished 480x272 picture (the frame plus the
 * port's overlays, what the window shows) on a large screen floating in front
 * of the player, and the motion controllers as a PSP pad. It runs with either
 * renderer, since it takes the composed picture, not the GE's targets.
 *
 * The OpenXR session lives on its own thread at the headset's rate; the game
 * hands it pictures (vr_submit_frame) and reads the controllers (vr_pad_get)
 * once per vblank. The headset never waits for the game: between game frames
 * the compositor keeps showing the last picture, world-locked.
 *
 * openxr_loader.dll is loaded at run time: without it, or without a runtime or
 * a headset, vr_start reports why (vr_status) and the game runs flat. */
#ifndef PSP2I_VR_H
#define PSP2I_VR_H

#include <stdint.h>

typedef struct {
    float distance_m;   /* the screen's distance from the head at recenter */
    float width_m;      /* the screen's width (the height follows 480:272) */
    int   scale;        /* swapchain pixels per game pixel, 1..4 (nearest: sharp under the compositor's filter) */
} vr_config;

#define VR_DISTANCE_DEFAULT 2.5f
#define VR_WIDTH_DEFAULT    3.0f
#define VR_SCALE_DEFAULT    2

enum {
    VR_OFF,          /* not started */
    VR_NO_LOADER,    /* openxr_loader.dll missing */
    VR_NO_RUNTIME,   /* no OpenXR runtime installed or it refused (no D3D11 support) */
    VR_NO_HEADSET,   /* the runtime has no headset yet (retried every few seconds) */
    VR_WAITING,      /* session made, waiting for the runtime to show it */
    VR_RUNNING,      /* frames go to the headset */
    VR_FAILED,       /* an error ended the session (see the log) */
};
const char *vr_status_name(int status);   /* "OFF", "NO HEADSET", ... */

int  vr_start(const char *exe_dir, const vr_config *cfg);   /* 0: the VR thread started */
void vr_stop(void);                                         /* ends the session, waits for the thread */
int  vr_status(void);                                       /* VR_* */
void vr_configure(const vr_config *cfg);                    /* distance / width now; scale at the next start */
void vr_recenter(void);                                     /* the screen in front of the head again */

/* The picture to show: 0xAARRGGBB (alpha ignored), w x h. Copied; cheap. */
void vr_submit_frame(const uint32_t *argb, int w, int h);

/* The controllers as a PSP pad. buttons: PSP_CTRL_* bits; sticks 0..255, 128
 * centre (left: the analog stick, right: the camera). host: VR_HOST_* actions
 * for the port itself. Returns 1 while the controllers are live (the session
 * has focus), else 0 with everything released. */
typedef struct {
    uint32_t buttons;
    uint8_t  ax, ay, rx, ry;
    uint32_t host;
} vr_pad;
enum { VR_HOST_MENU = 1 << 0 };   /* the engine menu's toggle (right stick click) */
int  vr_pad_get(vr_pad *out);
void vr_set_swap_circle_cross(int on);

#endif /* PSP2I_VR_H */
