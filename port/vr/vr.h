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
    int   stereo;       /* the screen is a stereo window (vr_submit_stereo): its width follows the game's FOV */
    /* Head tracking: the HUD panel (the game's HUD picture, the third part of
     * vr_submit_stereo_shared's texture), as PSUEx-VR places it: this far
     * ahead of the recentred head and this much below eye level, this wide,
     * turning only to follow the heading once the head has looked more than
     * hud_follow_deg away (0: it stays where recenter put it). */
    float hud_distance_m, hud_drop_m, hud_width_m, hud_follow_deg;
} vr_config;

#define VR_DISTANCE_DEFAULT 2.5f
#define VR_WIDTH_DEFAULT    3.0f
#define VR_SCALE_DEFAULT    2
#define VR_HUD_DISTANCE_DEFAULT 2.5f
#define VR_HUD_DROP_DEFAULT     0.1f
#define VR_HUD_FOLLOW_DEFAULT   35.0f

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

/* The picture to show: 0xAARRGGBB (alpha ignored), w x h: 480x272 or a
 * whole multiple up to 4x (the internal resolution). Copied. Give the
 * swapchain the same scale (vr_config.scale) to keep every pixel. */
void vr_submit_frame(const uint32_t *argb, int w, int h);

/* Phase 2, the stereo window: one picture per eye (same sizes as
 * vr_submit_frame), drawn by the game with each eye's offset. Each eye sees
 * its own picture on the same world-locked screen, which is then as wide as
 * the game's horizontal FOV at its distance (tan_half_fov_x: the game's
 * horizontal half-FOV tangent; 0 = keep the last one), so the screen is a
 * window into the game world. Copied. A later vr_submit_frame shows a mono
 * picture again.
 * Phase 3, head tracking: `head` is the id of the vr_head the eyes were drawn
 * with (0 = none). Then the pictures go to the headset as a projection layer
 * at that pose, with their FOV (tan_half_fov_x, and tan_half_fov_y: 0 = from
 * the picture's shape), instead of the window: the compositor turns them with
 * the head between game frames. */
void vr_submit_stereo(const uint32_t *left, const uint32_t *right, int w, int h, float tan_half_fov_x,
                      float tan_half_fov_y, uint32_t head);

/* The same on the GPU: `handle` is a DXGI shared handle (keyed mutex, key 0)
 * of an RGBA8 texture 3w x h holding left | right | HUD (premultiplied
 * alpha, transparent where there is no HUD), at the swapchain's size.
 * The VR thread copies it into the swapchain. If it can't (another GPU, a
 * size or format mismatch) vr_share_failed() turns 1 for good: then send
 * pixels with vr_submit_stereo. */
void vr_submit_stereo_shared(void *handle, int w, int h, float tan_half_fov_x, float tan_half_fov_y, uint32_t head);
int  vr_share_failed(void);

/* The port's overlay (engine menu, FPS counter: what it draws over the game)
 * as its own layer on the screen, 480x272 0xAARRGGBB with alpha 0 where it
 * draws nothing; NULL = none. Used with the shared stereo pictures, which
 * don't carry it. Copied; send it only when it changes. */
void vr_submit_overlay(const uint32_t *argb);

/* The headset's IPD in metres (the distance between the two eye poses), or
 * VR_IPD_DEFAULT until the session reports it. */
#define VR_IPD_DEFAULT 0.064f
float vr_ipd(void);

/* Phase 3, head tracking: the newest predicted head pose, relative to the
 * recentred one (vr_recenter: its position, and its heading with the floor
 * level), in the OpenXR convention (+x right, +y up, -z forward, metres).
 * The VR thread keeps the last few by id, so a picture drawn with one can be
 * shown at that pose (vr_submit_stereo*'s `head`). Returns 1 with a tracked
 * pose, 0 (and id 0) before the first one. */
typedef struct {
    uint32_t id;
    float    q[4];          /* orientation x, y, z, w */
    float    p[3];          /* position */
    float    eye[2][3];     /* the left and right eye in the head's space */
} vr_head;
int vr_head_get(vr_head *out);

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
