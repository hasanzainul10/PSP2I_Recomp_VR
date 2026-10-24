/* The local player's skeleton, for VR arms and body (VR_PLAN M2 / P6).
 *
 * A skinned character is an animated model: 0x08A850A8(model) evaluates its
 * animation into node matrices (model space, at *(model+100)), then
 * 0x08A846DC(model, &palette, skeleton) builds the skinning palette from
 * them: palette[slot] = model+32 (model -> eye) x node x the node's inverse
 * bind matrix. A hook on 0x08A846DC sees the finished pose of each model in
 * model space before it is drawn, and can change it for the local player
 * only (nothing in the game's state changes: online play is untouched). */
#ifndef PSP2I_VR_BODY_H
#define PSP2I_VR_BODY_H

/* Registers the hook. Development builds: PSP2I_SKEL=FILE finds the local
 * player's model (by its address in the player object), writes its node
 * matrices twice a second and its skeleton records once to FILE, and logs
 * what it found every 5 s, with the character's other parts (the same model
 * comes three times a frame, one skeleton per mesh, all on its nodes).
 * PSP2I_SKEL_TEST=NODE,DEGREES[,keep] swings one node and those below it
 * (15 = the left shoulder: the arm, hand and weapon follow, headset-checked). */
void vr_body_init(void);

/* Arms (vr_arms=, with INPUT MODE = MOTION): the local player's arms reach
 * for the VR controllers (2-bone IK at the palette, drawn only; nothing in
 * the game's state changes). The host sets it each frame. */
void vr_body_arms(int on);

/* First person (vr_fp_hide_head=): your character's head (and hair) is
 * drawn shrunk to a point, so the eyes can be where its face is. The host
 * sets it each frame. */
void vr_body_head(int hide);

/* First person: the head joint (node 13, as animated) and the body's forward
 * direction, in the game camera's view space, from this frame's pose (while
 * the head is hidden). Returns 0 when there is none from the last few vblanks. */
int  vr_body_head_eye(float head[3], float fwd[3]);

/* The avatar (vr_avatar=, with the arms on): your character drawn from you,
 * as a VRChat avatar: the upper body in the skeleton's rest pose (no attack
 * or Photon Art moves it), the arms on the controllers, the whole body
 * turned to the gaze; the legs walk as the game animates them, and walking
 * left, right or back from the gaze draws the game's own R-strafe legs,
 * learned the first time you strafe with R that way (a few seconds) and kept in the
 * legs file. legs_still: the legs stand in the rest pose instead. Drawn
 * only; the host sets it each frame. */
void vr_body_avatar(int on, int legs_still);
/* Where the eyes look, in the game camera's view space (unit), each frame. */
void vr_body_gaze(const float fwd[3]);
/* The learned legs' file (loaded now, written when one is learned). */
void vr_body_legs_file(const char *path);

/* Dev: the body's palettes of the next frame to the log (with a VR capture). */
void vr_body_dump_palettes(void);

/* The hands' rotation: vr_body_hand_calibrate (both grips + both triggers)
 * takes, at the next pose, each hand's rotation relative to its controller
 * (hold the controllers as your character holds its hands); after it the
 * hands, and the weapon with them, turn with the controllers. The 18
 * floats (left, right: 3x3 row-major) are saved in the ini by the host. */
void vr_body_hand_calibrate(void);
int  vr_body_hand_cal_get(float cal[18]);         /* 1 when both hands are calibrated */
void vr_body_hand_cal_set(const float cal[18]);
int  vr_body_hand_cal_changed(void);              /* 1 once after a calibration */

#endif
