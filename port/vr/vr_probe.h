/* The VR probe: reverse-engineering help for the VR camera and body work
 * (phase 0), built on the GE's draw observer (psp_gpu_set_draw_observer).
 *
 * Every frame it notes the view matrices the 3D draws used (the one with the
 * most vertices is taken as the scene camera) and the skinned draws (the
 * characters). On request (F8 in development builds) it writes a report to
 * logs/vr_probe.txt:
 *   - the scene camera: position, forward, field of view, its matrices;
 *   - where guest RAM holds that camera position and that view matrix (the
 *     game's camera object is near them);
 *   - the skinned draws nearest to where the camera looks (the player is
 *     usually the closest one in third person), with their world positions,
 *     and where guest RAM holds each position (the player object).
 * Taking two reports a few steps apart and keeping the addresses whose values
 * followed narrows the candidates down quickly.
 *
 * PSP2I_VR_PROBE_EVERY=N also logs the camera every N frames to the console. */
#ifndef PSP2I_VR_PROBE_H
#define PSP2I_VR_PROBE_H

void vr_probe_init(const char *report_path);   /* registers the observer */
void vr_probe_vblank(void);                     /* once per vblank: periodic log, pending report */
void vr_probe_request(void);                    /* write a report at the next vblank */

/* The scene camera of the last finished frame (world space, GE units).
 * Returns 0 until a 3D frame was seen. fovy in degrees. */
int vr_probe_camera(float pos[3], float fwd[3], float *fovy_deg);

#endif /* PSP2I_VR_PROBE_H */
