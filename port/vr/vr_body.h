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

#endif
