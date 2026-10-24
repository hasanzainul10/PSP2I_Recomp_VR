# VR progress (handoff)

Where the VR mod stands, so any session or Claude account can continue. Update
this file at the end of every working session: what changed, what was checked,
what is next. The plan itself is [VR_PLAN.md](VR_PLAN.md); user-facing docs are
[VR.md](VR.md); reverse-engineering results go in [ADDRESS_MAP.md](ADDRESS_MAP.md).

_Last updated: 2026-10-24, skeleton mapping for arm IK (section below)._

## Skeleton mapped for arm IK (M2, 2026-10-24, latest)

The user's answers to the last handoff: the intro movie and the MOTION
controls are fine; manual aim in first person not tried yet. They chose to
**skip the menu D-pad hook** and start on IK.

- **Found from the GE side down** (all in ADDRESS_MAP "The player's
  skeleton"): bone commands are patched per model by `0x08D8D4E8`; the
  palette is built by `0x08D74E84` from node matrices; **`0x08A846DC`
  (model, &palette, skeleton)** is where the local player's pose (model
  space, before the camera) can be changed for IK, and it is hooked.
- **The local player's model** is `*(player+0x178)+0x8`: found by
  searching the player object for each model's address (the first run
  picked the nearest model and mixed party members in). 28 nodes, a clean
  humanoid tree from the skeleton records (parents agree with the poses):
  arms 14-18 and 19-23 (clavicle, shoulder, elbow, wrist, hand end), legs
  3-5 / 6-8, spine 9-11, neck 12, head 13.
- **Tools:** `port/vr/vr_body.c` (dev `PSP2I_SKEL=FILE`: logs the model,
  dumps its nodes twice a second and its records once), and
  `.claude/skills/psp2i-static-re/scripts/skeleton.py DUMP` (positions,
  inferred parents, the game's own tree, chains). Dumps in
  `port/testwork/skel/` (git-ignored).
- **Checked in the headset (user):** `PSP2I_SKEL_TEST=15,60` (node 15 and
  everything below it swung about model +z, 0..60 degrees every 4 s, put
  back after each palette) swings the **left** arm, and the hand and the
  weapon go with it. The parts log shows why: the player's model goes
  through `0x08A846DC` three times a frame (three skeletons, the body and
  two other meshes) and all three read the same node matrices. So IK only
  has to change the nodes, in each of the model's calls.
- **Not identified:** an 8-node model at container +0x10 with its own
  nodes, ~14.5 units from the body.

- **2-bone arm IK works in the headset** (user: "the arms follow the
  controllers"): `vr_body.c` `arms_pose` at `0x08A846DC`, on with INPUT
  MODE = MOTION and `vr_arms=on` (the default; ini only, no menu item yet).
  Shoulder 15 / 20 turns to a solved elbow (pole down, a little out and
  back), elbow 16 / 21 turns the wrist 17 / 22 onto the target; the nodes
  are put back after each palette (drawn only). Target = the controller's
  offset from the headset, from the model's eyes (first build, replaced below)
  (head node 13 + 6 cm up, 8 cm ahead), at the model's arm over 0.55 m
  (measured live: upper arm and forearm 2.2 units each, 8.0 units a metre:
  the node scales make them shorter than the records' 2.56).
- **Reach anchored at the shoulders** (2026-10-24): the controller's
  offset from your estimated shoulder (18 cm to the side, 22 cm below and
  8 cm behind the eyes, in the headset's heading) is applied from the
  model's shoulder joint, at the model's arm over 0.62 m (shoulder to grip).
  From the eyes (the first build) the reach was over 100% in 21 of 40 log
  samples; now 1 of 19 (`arms2.log`: 20-100%, median 77%). **User, in the
  headset: "the arms bend with mine now and straighten when I reach out."**
- **Hands and weapon turn with the controllers** (2026-10-24, user in the
  headset: "calibrated: the hands and weapon turn with the controllers").
  The hand bone's axes against a real grip can't be read from the game, so
  it is a calibration, as PSUEx-VR: **both grips + both triggers** (MOTION;
  the triggers don't attack while both grips are held) takes each hand's
  rotation in the controller's frame (`C^T H`, model axes, after the arm
  IK); after it the wrist 17 / 22 turns to `C x offset` each frame, the
  weapon with it. Saved as `vr_hand_cal=` (18 floats) in the ini; without
  it the hands keep the game's rotation. A buzz on both controllers says it
  took. Log: `vr: arms: hands calibrated (left yes, right yes)`.

- **The weapon in the IK hand** (2026-10-24, user in the headset: "after
  holding R once, my weapons stay in my hand without R", all their weapons).
  How it was found, as it took six runs:
  - The weapon is not part of the body's model, so the arm IK alone left it
    where the animation had it. The extra model in the player's container
    (+0x10) rides on the head (node 13: hair or headgear), not the hand.
  - Every matrix the game copies into a display list goes through
    `0x08D8D4E8` (skinned palettes and rigid world matrices, eye space): a
    hook there (`hook_bone_patch`) checks those near the hands. A thing's
    placement in an arm node's frame, `inv(body M x node) x M`, stays fixed
    only for the node it hangs on; once found, it is moved by what the IK did
    to that node, for the copy only. The body's own palettes are skipped.
  - Bug on the way: my pointer check rejected all of these objects (they
    live at `0x085xxxxx`, below `0x08800000`); widened to `0x08000000`.
  - Standing still, the head, the floating Rabbit and the scenery look as
    fixed to the forearm as the weapon (the first build dragged them along):
    a thing must drift clearly less against the arm than against the torso
    (nodes 1, 11, 13), with the evidence kept over its life, and sit within
    5 units of the node.
  - Weapons are in the hand only while R is held (lock-on, player +0x214;
    MOTION's left grip): the user saw them attached correctly then and not
    otherwise. So the evidence is kept per state; with R held, rigid to the
    hand and within 2.5 units is enough (the weapons sit 0.0-0.1 from node
    18 / 23); the grip learned with R is then kept without R, so the weapon
    stays in the hand. Each weapon is learned the first time R is held with
    it.
  - Dev logs: `vr: arms: the matrix at ... rides on body node N` / `let go`;
    with `PSP2I_SKEL` also the copy hook's counters and candidates.

- **The left hand uses the sub weapon** (2026-10-24, user in the headset:
  "left swing / trigger uses the sub weapon, and left grip + swing /
  trigger its PA"). A left swing or trigger holds R, waits for the game's R
  stance (player +0x214 != 0, at most 8 flips), presses Square (Triangle
  with the left grip) with R still held, and keeps R 20 flips more so a
  combo's next swing fires at once (host/main.c `g_sub_phase`). The stance
  came on within 0-6 flips in all 8 logged presses. The right hand is
  unchanged.

- **First person without your own head** (2026-10-24, user in the headset:
  "in first person the head and face are gone now"). Before, the eyes sat
  1.2 units ahead of the head with a 1.0 clip so the head stayed out of
  view (user: "it feels weird, the camera is on top of the head"). Now
  `vr_fp_hide_head=on` (default) hides the head and the eyes sit where the
  face is: 0.25 ahead, 0.25 clip (`vr_fp_forward` / `vr_fp_near` apply with
  it off). The head turned out to be four things, each found from a VR
  capture and its GE dump (left grip + left stick click now also takes the
  GE dump and logs the body's palettes):
  - hair and accessories: body meshes on nodes 12 / 13 only;
  - the face: body meshes blended between the head nodes and the chest /
    collarbones (10, 14): shrinking the head squashed it into a smear, so
    the head nodes are set to an all-zero matrix and psprecomp's GE drops
    any skinned draw with an all-zero bone (`psp_gpu_set_skin_zero_drop`);
  - an accessory model in the player's container (+0x10, 8 nodes, ~2.5
    units from node 13): its M shrunk at `0x08A846DC`;
  - the head itself: a **rigid** mesh (texture `0x09358BE0`, no weights),
    whose world matrix none of the palette paths builds (hooking all five
    callers of `0x08D74E54` shrank ~40 skeletons near the head and left
    it): dropped at the GE, any rigid draw whose world origin is within 3
    units of the head node (`psp_gpu_set_rigid_drop`, the point set each
    frame from the animated pose).
  Not checked: a weapon held against the face (may blink out at 3 units),
  party members standing inside you.

- **HUD eye-tied in first person, then flat blips: fixed** (2026-10-24,
  user in the headset: "head hiding on: no blips during attacks, head still
  gone, HUD lazy"). Both came after the head hiding and the weapon in the
  hand; found with psprecomp's per-frame reports (below), VR captures and a
  settings test, after several wrong guesses (a glow composite, menu-scene
  frames, split frames: those changes were tried and taken out, except the
  two noted as harmless below).
  - Eye-tied HUD: the HUD pass is found by a clear of the frame's 3D
    target; that target was the last one a 3D draw went to, and the glow
    pass draws into its buffer (`0x154000`) after the scene, so the clear of
    the scene's target went unseen and the whole HUD (40-67 sheets) was
    moved as 3D scene: fixed to the head in first person, where the game
    camera is the head. Common now because the weapon (often glowing) is in
    view in the hand. Fixed in psprecomp: once latched, only the scene's
    target is the 3D target.
  - That let an attack effect's mid-scene clear (colour + alpha, bits 3)
    start the HUD pass: the rest of the scene went flat onto the panel. The
    HUD pass now starts only at a clear with depth (the real one is bits 4).
  - Big attack flashes (~290 x 260 sprites from mid-screen) counted as
    full-screen passes (45% of the screen) and were drawn flat in both eyes:
    a full-screen pass must now start at the screen's corner, so they are
    effects placed in the world.
  - The remaining blips were the head hiding's rigid drop: within 3 units of
    the head it also caught what is placed at the camera itself (2.3 away in
    first person): screen-space effects and the sky. Proven by turning
    `vr_fp_hide_head` off (no blips). It now drops only perspective draws
    whose origin isn't at the camera.
  - Also kept: with glow off the glow chain's last full-screen add is
    dropped too (nothing of the glow is drawn then); the menu-scene check
    undoes first person's widening by its known factor as well.
  - Still seen in the logs: ~30 frames a run split across two display
    flips (the scene in one, the HUD in the next): their HUD is moved as
    scene for a frame or so. Not reported by the user; a carry-over of the
    frame state was tried and didn't catch them (the HUD clear has the
    colour bit too).
  - psprecomp prints three anomaly reports (`stereo: flip N: ...`): many
    same-in-both-eyes draws, HUD-depth sheets put in the world, 3D geometry
    on the HUD panel. Quiet in a good frame.

**Next for M2:**
1. Not checked: town with the weapon put away (a learned weapon would be
   drawn in the hand if the game still draws it there), twin and two-handed
   weapons (each learned on its own hand), the Rabbit with the arms on.
2. A VR > ARMS item on the engine menu (run `test_menu`), maybe with
   "calibrate hands" for players who don't know the chord.
3. First person: how the arms look in view (not judged yet); the weapon's
   hits stay the game's (only the drawing changes).
4. Rough edges to look for: the arms during attacks and Photon Arts (the
   game animates the arms; IK overrides them), two-handed and twin weapons
   (each hand to its own controller today), and lock-on.

## Start here (handoff 2026-10-23)

The user switches Claude accounts. Read `CLAUDE.md`, this section, then the
older "Handoff: start here (2026-10-22)" below for how to build and run.

**Branches:** `vr-theater` (main repo, last commit before this handoff
`cf02e54`) and `vr-draw-observer` (psprecomp, `5fc8a2c`), both pushed. The
side branch `vr-map-glow-fix` (minimap in first person, per-eye glow,
VR > GLOW) is **merged** into both. Before re-debugging anything the user
says "came back", check for unmerged side branches
(`git log vr-theater..<branch>` over local and remote, both repos).

**Done this session (all in the headset unless noted):**
- **F1 attack nudge** and **walk where you look** (earlier the same day).
- **M0 hands:** rings and rays at the controllers (compositor quad layers),
  hand poses in game units (`hands_frame`), haptics (`vr_haptic`). INPUT
  MODE (PAD / MOTION) on the R3 > VR page. Seen by the user.
- **M3 prototype, melee by swing:** a swing or a trigger pull = Square,
  with that hand's grip = Triangle (Photon Art), a buzz per swing. Swings
  registered in the logs, and the user said "it works". No Just Attack
  timing yet.
- **MOTION mapped as in PSUEx-VR** (VR.md "Controls", the user's pick of
  layout). Left grip alone = R (lock-on), right grip alone = L, right stick
  click = START, left stick click = manual aim (L + D-pad down). Both grips
  + Cross / Circle / Square / left click = recenter / first-third person /
  engine menu / left stick as the D-pad. All registered in the logs. Not
  yet judged by the user: how it feels, and manual aim in first person
  (the walk code may turn the aim).
- **The intro movie stuck with the headset on:** fixed in the log (all of
  its VRAM frames now reach the headset, at 480x272, converted outside the
  frame lock). The user hasn't said yet whether it plays properly.
- **Menu D-pad, automatic:** built and tried, unreliable ("randomly works
  and doesn't"), now **off by default** (`vr_menu_dpad=off`). The manual
  toggle (both grips + left stick click) stays. Details, and what is known
  about the game's menus, in "The game's 'a menu is open'" below and
  ADDRESS_MAP "Menus and tasks".

**Next (suggested order):**
1. Ask the user how the movie, the MOTION controls and manual aim feel.
2. Menu D-pad done properly: hook the game's input-library query that menus
   use for D-pad presses (with repeat; `0x08D5B844` is that library's
   repeat timer, already hooked in host/framerate.c) and let the left
   stick answer it. Then no "menu open" detection is needed. Find the
   query with `psp2i-static-re` (callers of the library) first, then one
   hook run.
3. M3 for real: Just Attack timing and the combo buffer (needs the
   attack state: VR_PLAN M3's reverse engineering).
4. M1 pointer for the engine menu, M2 arms (bones), M4 guns.

**How to work with the user (lessons from this session):**
- Give run instructions as numbered steps with "before / after" spelled
  out. The user can't see the log and was confused by label chords.
- Prefer measurements that need no user input (auto-armed watches, logs
  that diff by themselves) over labelling by the user.
- Narrow live log watches to the lines you act on; tell the user when the
  watch changes. Don't narrate every repeated line.
- Dev tools left in the build (dev builds only): the menu finder
  (`mf_*`, labels on both grips + Triangle or gamepad L3 held + R3), the
  `0x08EE2F70` change log and auto-armed write watch, the menu-path hooks
  and the task log (`hook_task_walk`, hooks.txt), the headset picture
  counters (`vr: pictures in 5 s`). They are noisy: remove or gate them
  once the menu D-pad is done.

## Minimap and enemy glow in the headset (2026-10-23)

- **Report (PS VR2, `vr-theater`):** "the map seems buggy and the glow from
  enemy somehow become separate from body". Two L3 captures in the Ruins
  (`vrshot_010170`, `vrshot_011460`) with GE dumps.
- **The map:** 12 of the minimap's 3D map draws were traced `world`, moved
  per eye with their scissor moved to `302,97-330,118`; the HUD picture's
  minimap was a flat sheet. `psp2i_hud_panel` puts the map on the panel by
  clip w > 1000, but in the Ruins the map's w is −1035..883. Fixed in
  psprecomp `stereo_classify`: a HUD-pass draw clipped to a window smaller
  than the screen (the minimap's `403,6-473,76`, the only one in 58 dumps)
  goes to the panel.
- **The glow:** the enemy's glow is `fx` draws (small orthographic sprites,
  w 1), placed at a fixed `fx_dist` (55, or the player's distance in first
  person) along the camera ray, while the body was ~106 units away: the two
  eyes saw the glow at another depth. The sprites carry the body's depth
  value, so `fx_vertex` now reads the distance from it through the latest
  scene draw's projection (`stereo_fx_latch` follows PROJ changes; the
  frame's first draw, the sky, has another depth row), with `fx_dist` as
  the fallback. Hit sparks and techniques get their own depth the same way.
- **Second report (first person, old build):** "basically any glowing
  effect", and "the minimap still behave like we are seeing it in first
  person, like it lay flat". The map's 3D draws had vertices behind their
  camera (w down to −1399) only in the first-person runs; in the 16:03 and
  16:12 dumps it was a correct top-down map. Cause: the minimap's camera
  (`094AFD40`, applied 3 times a frame after the scene walk) has the
  field's projection x scale 1.368, so `hook_apply_camera` put it at the
  head too, and `hook_set_projection` widened its projection. Fixed: only
  the frame's first field camera is the field camera; others are applied,
  and their projection set, as the game made them (`g_fp_other_cam`).
- **Minimap: confirmed fixed in the headset** (user, 2026-10-23).
- **The glow pass** (glowing parts drawn into `0x154000`, shrunk and
  blurred through `0x044000` / `0x066000`, added over the scene): it was the
  same picture in both eyes (0 px apart in a capture, against the body's
  3 px): d3d11 `texture_view` decoded the 512x512 glow texture from VRAM
  (the mono, left picture) as it is taller than the target's 272 rows.
  Fixed: a target with eye copies is always sampled from the GPU, each eye
  its own copy (a dev log line names any eye draw that still samples a
  target without its copy: none in the next run). The user still saw glows
  drift off enemies and weapons with it on, so **VR > GLOW** (`vr_glow=`,
  psprecomp `no_glow`: a frame's 3D draws into another target than its
  scene's are dropped) turns the glow pass off in the headset; with it off,
  enemies and weapons look right (user). **Off by default** (user's choice)
  until the drift below is fixed.
- **The door (user's annotated capture `vrshot_011413`, GLOW off):** the
  small light in the door is the door's own lamp and sits on it; its larger
  halo is the glow pass, so with GLOW off it is gone and with GLOW on it
  drifts like the rest. The red cluster in `vrshot_004157` is a weapon's
  spell (`fx` sprites), placed on its caster.

### Open: the glow pass drifts with GLOW on (next step)

State: branch `vr-map-glow-fix` (psprecomp's branch of the same name),
pushed to `origin`; built in the worktree
`.claude/worktrees/objective-davinci-e360ad`. The user confirmed the
minimap and that GLOW off looks right for enemies and weapons; GLOW is off
by default (their choice), and the branch is pushed to `origin`.

What is known about the drift (captures in `port/build/Release/captures`
of that worktree, logs `port/testwork/p3/vr_mapglow1..5.log`):
- At render scale 5 the game's blur hardly blurs: the glow in the eyes is
  a sharp copy of the glowing parts, displaced from them by 40-80 eye px
  (`vrshot_003647`: the NPC's yellow glow left of her, a green arc up-left
  of the shark's blade). The game's own picture of the frame after shows
  neither, so it is the eyes' copy, not the game's.
- Each eye samples its own copies through the whole chain (the dev log
  "eye N samples VRAM+... without its own copy" never fired), the frame
  latches once (one `latched:` per trace), and the chain's targets are all
  `stride x 512` with the same S x SY scale, so the copies line up.
- Many glow-buffer draws have vertices at or behind the game camera
  (clip w −25..2: the first-person player's own glowing parts) and
  `vrshot_007260` / `006782` show long yellow wedges in the sky: those are
  glow-buffer geometry, drawn stretched in the eyes.
- Next: trace the glow-buffer draws themselves (fb `0x154000`) per eye:
  their eye-space w after `g_st_k`, whether `g_st_k_ok` holds for their
  PROJ, and whether `near_w` clips them as it clips the body; compare one
  glow draw's eye box with the body draw it belongs to. `psp2i-hook-re` /
  the stereo trace (`st_trace_draw` lists only non-moved draws; add the
  glow target's moved draws).

## Handoff: start here (2026-10-22)

The user switches Claude accounts to save tokens. A new session should:

1. Read `CLAUDE.md`, then this file (this section, "Status by phase",
   "Next steps"), then VR_PLAN.md "P2 approach (decided 2026-10-22)".
2. Check out `vr-theater` (main repo) and `vr-draw-observer` (psprecomp
   submodule). Both are pushed; the working tree was clean at handoff.
3. Build with the `psp2i-build-run` skill. Headless runs **cannot** reach town
   (they stall in the intro movie, see Gotchas); 3D checks need a windowed
   run (tell the user first) or the headset.
4. To run in the headset for the user (they ask "run it for me"): from
   `port/build/Release`,
   `./psp2i.exe --root ../../../FinalBuild/GameData --eboot ../../../FinalBuild/EBOOT.BIN --vr --seconds 3600 > ../../testwork/p2/vr_stereoN.log 2>&1`
   in the background; wait for `vr: stereo on` in the log; the user plays
   and closes the game; then read `perf:` lines and the report
   (`d3d11 stereo ... shared on the GPU`). The game must be closed before a
   rebuild (the exe is locked while it runs).

**State at handoff:** P2's stereo window works and the user confirmed it in
the PS VR2 at 3x and at 60 fps in town ("3D looks right and it's steadier"):
59.7 fps average, min 55.9. Eye pictures go to the headset on the GPU
(shared texture); the overlay is its own layer; the mono draw is the left eye.

**P3 head tracking (2026-10-22, second session): works in the PS VR2.**
It's applied at the GE, not by hooking the camera (the reasons are in
VR_PLAN.md "P3 approach"). `vr_head=on` and `vr_level=on` are the defaults.
- **The user, in the headset:** the world stays still while the head turns;
  leaning moves the view. (Recenter was noted as working here, but the
  user later said it never worked for them: see "Recenter" below.)
- **The log** (`port/testwork/p3/vr_head1.log`, about 5 min, 30 fps cap): a
  steady 30 fps (each minute after the first averaged 30.0). 8493 eye
  pictures were shared on the GPU and none went through the CPU.
- **The black border around the game's FOV is the main complaint.** The
  user wants the view wider than the game's (more immersion).
- **Not judged yet:** the HUD at infinity, and objects missing to the
  side. The layer switched between projection and quad a few times; the
  user saw no flicker, but they took the headset off a few times. Check
  whether a frame without a pose sneaks in during play.

Without a headset, `--stereo` with `PSP2I_VR_FAKE_HEAD=yaw,pitch` (degrees)
draws the eyes from a turned head, for `--overlay-shot` captures. That needs
a windowed run that reaches town (the press script isn't recorded: write one
down when found).

**What the user wants next (2026-10-22), as in PSUEx-VR:**
- **The HUD body-locked:** the HUD, the avatar portrait when talking to
  NPCs, and similar 2D UI on a panel where today's border is (the game's
  FOV in front of the body). The panel moves with the body, not the eyes.
- **The 3D view wider than that panel:** expand beyond the game's FOV where
  possible.
- **World-space UI in the 3D world:** floating shop labels, chat boxes and
  character names stay at their place in the world (today they are in the
  HUD pass, flat).

**The wider view (2026-10-22, third part of the session): in the headset.**
Details are in VR_PLAN.md "A wider view, the cull and the HUD", and the cull
in ADDRESS_MAP.md.
- **Run 1** (1.6x across and up, cull 8x): only wider across; the user saw
  no gaps from the cull. HUD, reticle, location names and technique effects
  were still eye-locked.
- **Run 2** (`vr_fov_scale_y=2.6`, and the HUD pass seen from the eyes), user:
  - the border is almost gone (some is left), but the picture is quite
    pixelated;
  - names, labels and the reticle are correct; technique effects stay in
    the world;
  - the HUD is mostly correct, but the start menu, the mission menu and
    the map in a mission "stay but angle changes when head move".
- **Log** (`testwork/p3/vr_wide2.log`): 30 fps (29.9 average over 4 min),
  game thread 45-67% idle, 7697 pictures shared on the GPU. At 60 fps (run
  1) about 49 fps average: "not constant 60, but not bad".

**HUD and menus rebuilt the PSUEx-VR way (2026-10-22, late in the session).**
The earlier attempts (the whole HUD pass re-projected per eye, a head
follow frame, shrinking menus, levelling the game camera) gave skewed menus,
a HUD that followed the head up and down, and labels and effects out of
the world; the user found it overwhelming. Those were removed. Now:
- **HUD panel:** the HUD pass's HUD and menu layers go, as the game drew
  them, into their own transparent picture (psprecomp `draw_hud` ->
  d3d11.c `d3d_draw_hud`, premultiplied), shared with the eyes as the third
  part of the shared texture (left | right | HUD). The VR thread shows it on
  a flat quad 2.5 m ahead and 0.1 m below eye level (`vr_hud_distance`,
  `vr_hud_size` 0.7 of the game's view width), turning only to follow the
  heading once the head has looked more than 35° away (`vr_hud_follow`),
  easing in (PSUEx VRCamera's numbers). The desktop mirror has no HUD while
  head tracking is on.
- **Which layers go on the panel** (main.c `psp2i_hud_panel`, by the HUD
  sheet's clip w from dumps and captures): menus < 80, the field HUD
  84-85.99, the minimap frame 86.85-87.05, the menu background sheet 92 and
  the 3D map > 1000, plus any sheet covering over a fifth of the screen
  (NPC portraits, at w 86). Everything else stays in the world: labels
  (86, 88), the lock-on reticle (90).
- **Effects:** small orthographic scene-pass draws on the 3D target (hit
  sparks, techniques; w = 1) are placed in the world 55 units along the
  camera's ray (`ST_FX`). Not yet seen in a capture of this build.
- **Menu scenes** (continue, mission select, loading): the game's own
  picture on the flat screen; told apart by the frame's camera (identity
  view, off-centre projection, or a projection x scale other than the
  field's 1.3684). The projection is now the GE's of the latched draw (the
  RAM copy was stale: mission select was missed in the first run).
- **Dev tools:** F11 / `PSP2I_VR_SHOT_EVERY=N` captures (eyes + the HUD
  picture + a per-draw trace); `.claude/skills/psp2i-static-re/scripts/` and the `psp2i-static-re`
  skill.
- **Second captured run** (`testwork/p3/cap5`): technique and hit effects
  sit on the character in both eyes (60k effect draws placed); the
  lock-on target box ("Evil Shark LV1") and the minimap with its map are on
  the panel; menu scenes switched to the flat screen. The "Abandon
  mission?" dialog's white background sheet was in the world: it is a HUD
  layer at w 92, now on the panel (not rerun yet). No F11 captures came.
- **Checked:** one captured headset run (`testwork/p3/cap4`): town, NPC
  talk and the mission terminal show the HUD panel in the game's layout and
  a clean scene; mission select was head-tracked (fixed since, not rerun).
  The user has not judged this build yet.
- **Third run, the user in the headset:** the HUD, the minimap and the
  "Abandon mission?" options were **invisible**, though every capture had
  them. Cause: captures read the HUD picture directly, but the headset gets
  it through `share_eyes`, which only took it when its frame matched the
  eyes' frame. The eyes of frame N are shared at the vblank after their
  flip, when the game has usually begun frame N+1's HUD and cleared the one
  picture, so the frames never matched. Also, some NPC portraits stayed in
  the world (the shop portrait is a HUD sheet at w 86, the labels' layer),
  hiding the talk text and menu.
- **Fixed (commit `315939a`, psprecomp `8283d3a`):** d3d11.c keeps a ring of
  three HUD pictures (`HUD_RING`), each named by its frame; `share_eyes`
  takes the one of the eyes' frame (`hud_slot`). `hud_panel` now also gets
  the draw's share of the screen (psprecomp `stereo_classify` -> `st_bbox`),
  and any HUD-pass sheet over a fifth of the screen goes on the panel.
  Report line: `HUD panel draws; frames shared with their HUD N, without M`;
  each capture logs the HUD picture's coloured / alpha pixel counts.
- **Fourth run, the user in the headset: "all looks working now".** Log:
  4209 of 5119 frames reached the headset with their HUD; the other 910
  had none drawn (menu scenes, loading). Every capture's HUD picture had
  alpha. Asked after, the user confirmed the HUD panel's distance and
  follow, NPC talk (portrait and text) and the minimap's enemies all work.
  Still no F11 captures.
- **Fifth run (`testwork/p3/cap7`, `vr_f11.log`), the user in the headset:
  the lock-on reticle sits on the enemy, names stay put in the world, and
  technique effects stay in the world.** P3's HUD / world split is done.
  F11 still gave no captures (no "no stereo now" either): the key likely
  never reaches the game window while the headset is on. The timed
  captures agree: the reticle (w 90) placed over the shark in both eyes,
  labels (86 / 88) placed in the world (off view with the head turned 80°).

**P4 first person (started 2026-10-22, the user's choice after P3):**
- **Measured** (windowed `--stereo` runs, `fp_measure1/2.log`): player
  +0x4B0 is the feet; the head's top is 5.8 units above (GE probe
  `psp_gpu_stereo_probe`, psprecomp); ~3.4 game units per metre. The
  third-person `vr_world_scale` of 10 is ~3x that (a miniature world).
- **Built, not yet seen in the headset:** `vr_view=first` puts both eyes
  at the local player's head (feet + `vr_fp_eye` 5.3 along world up, seen
  through the view matrix), heading from the game camera (levelled), the
  head's movement and IPD at `vr_fp_scale` 3.4 units/m. Rendering only:
  the game camera is untouched. Gamepad L3 twice quickly toggles first /
  third (saved). The local player is now tracked in every build
  (gamelog.c `g_local_player`).
- **First headset run (`vr_fp1.log`): first person not reached.** ~25 s in
  the engine menu opened and VR was switched off there (`menu: VR off ...
  (saved)`, so the ini says vr=off: `--vr` overrides it); the game ran flat
  ~8 min, then crashed in the game's `0x08AC2910` (called from the update
  `0x08B821B8`, a jump to an untranslated address: `default_miss`), the
  same function as the 120 fps crash below, here at 30 fps. Not VR code on
  the stack. The motion controllers' stick clicks are bound after all
  (`bound to recenter: .../left_stick/click`): the earlier "no" was
  checked before the runtime's binding.
- **Second run (`vr_fp2.log`):** L3 twice turned first person on (30 fps).
  User: "I am inside the head". Fix (built, not run): the eyes
  `vr_fp_forward` 1.2 units (~35 cm) ahead of the head along the gaze, so
  the head stays behind them (the user asked for more than 0.6, for the
  run animation's lean).
- **Third run (`vr_fp3.log`):** user: "running, still see back of head"
  (the run animation throws the head ahead of eyes placed from the feet).
  Fix (built, not run): a first-person near clip, `vr_fp_near` 1.0 unit
  (~30 cm): psprecomp `psp_gpu_stereo.near_w` moves emit_tri's clip plane
  from w = W_EPS to it for the eyes' moved 3D draws, so whatever is that
  close in front of the eyes is cut, the head included.
- **Fourth run (`vr_fp4.log`, 30 fps):** user: "we can still see head
  albeit a bit better". Fix (built, not run): the eyes follow where the
  model's head really is across: each first-person frame the GE probe
  (now `this_frame`, with the centre of the vertices from `vr_fp_eye` - 1
  up) measures the head; the next frame turns it into world axes, keeps
  the level part, smooths it (half) and adds it to the feet. The height
  stays `vr_fp_eye` (no run bob). Dev log: `vr: fp head: ... across`.
- **Fifth and sixth runs (`vr_fp5/6.log`):** user: "that works" (the head
  is hidden), "maybe less shaky for comfort", and "the flat game sometimes
  appears floating on top of VR for a short moment". The head measured up
  to ~2.4 units across from the feet. The flat moments were all menu-scene
  switches at the game's loading transitions (area changes). Fixes (built,
  not run): the head follow moves a fifth of the way per frame (was half);
  a picture without the head (tag 0) goes to the flat screen only after
  a second of them, until then the headset keeps the last 3D picture.
- **Seventh run:** user: smoother, "a bit better"; asked for performance.
- **Profile at 60 fps (`vr_perf1.log`, ~24 min, town and a mission, first
  person, 1440x1360 eyes):** 57-59.8 fps per 30 s after the first minute
  (loading, PNG decodes: 33.7). d3d11 per frame: draws 1.03 ms, eye draws
  0.52, HUD 0.11, sharing 0.21, present 0.48 (~2.4 ms in all). GE 25-41%
  of the time (the GE emulation on the CPU plus that), idle 22-45%, so the
  game logic ~30-40%. The renderer isn't the limit; the dips (min 36-44
  fps) are spikes: mission scenes (GE 40%, idle 22%) and texture-pack PNG
  loads on the game thread.
- **Texture-pack PNGs off the game thread (user's pick of the spike
  fixes):** texreplace.c decodes them on 2 worker threads; the first draw
  queues the PNG and draws the original, and the backends (d3d11.c,
  gl_ge.c `rep_wait`) look the texture up again once `texreplace_pending`
  is 0. Checked headless at the title (the brand logos PNG: loaded by a
  worker, swapped in, clean exit); not yet seen in the headset.
- **User, after the profile run:** "the flat screen blips still occur";
  in a boss battle, after a cutscene the game went flat and stayed so (the
  first/third toggle didn't bring VR back) until the boss was beaten. The
  log (`vr_perf1.log`): the boss fight's projection x was 1.3036 / 1.3353
  against the field's 1.3684, and the menu check took any FOV 3% off for a
  menu; a close-up cutscene was 5.67. Fix (built, not run): another FOV is
  a menu only below 1.15 (mission select 0.98) or above 2.5; an unturned
  view with the field's centred projection is a loading transition
  (`g_menu_loading`), held on the last 3D picture up to 5 s (real menus:
  1 s).
- **User:** "the flat appears while moving" (less often), and "some
  techniques became invisible" since first person. Found: psprecomp ran
  `on_frame` on a copy and copied back only the eye views, tag and flat
  flag, so `near_w` (the first-person near clip) never took effect; fixed
  (it copies `near_w` and `fx_dist` back). Techniques: screen-space
  effects are placed `fx_dist` (55) along the game camera's rays, at the
  player, so in first person at the eyes: now the player's distance + 6
  units (`PSP2I_FP_FX_AHEAD`). The flat moments: suspected a full-screen
  HUD-pass effect taken by the panel's size rule (frac > 0.2); dev log
  `vr: a big HUD-pass sheet on the panel` added to catch it. Built, not run.
- **The flat moments while moving, found (`vr_blip2.log`):** big HUD-pass
  sheets at w 90-220, 20-209% of the screen, their depth changing frame to
  frame (things moving in the world drawn in the HUD pass), went to the
  panel by the size rule. Fix (built, not run): the size rule only below
  w 89 (the labels' depth, where portraits are).
- **Engine menu (R3) > VR, asked by the user:** HUD DISTANCE (1.0-5.0 m)
  and WORLD SIZE (20-300% of life size, 3.4 units/m = 100%, for the view
  in use: first person sets `vr_fp_scale`, third `vr_world_scale`), live
  and saved. test_menu: 452 checks, 0 failed. Not yet used in the headset.
- **Image quality (user: "any way we can make the game look better?"):**
  with VR on (from the start, `d3d11_set_quality`), `vr_aniso=16`: the
  game's textures get mipmaps (GenerateMips at upload) and linear samplers
  anisotropic filtering; `vr_sharpen=0.5`: the eyes go into the shared
  texture through a cross-shaped CAS pass (d3d11.c `SHARPEN_SRC`) instead
  of a copy. Headless with `--vr`: shader compiled, textures fine, clean
  exit; the sharpening pass itself not yet seen in the headset. Not done:
  MSAA (the game samples its render targets), render_scale=4 (try first:
  1920 x ~1800 eyes), DLSS (no motion vectors; not a fit).
- **Quality run (`vr_quality1.log`, ~15 min, 54-60 fps at 60):** the user
  tried WORLD SIZE (20-40%, third person) and HUD DISTANCE (1-5 m) from
  the menu: both applied. Then VR was switched off in the menu and the
  game crashed at once: `share_eyes_body` called into unmapped code
  (0x00007FFF...), the frame right after `vr_stop`. Fixes (built, not
  run): VR off turns eye sharing off before `vr_stop`; the modules the
  OpenXR runtime loads are pinned (`mods_pin_new`, after xrCreateSession)
  so hooks it leaves can't point at unloaded code.
- **Engine menu > VR, asked by the user:** SHARPEN (OFF, 10-100%) and
  TEXTURE FILTER (OFF, 2X-16X), live: `d3d11_set_quality` at run time
  flushes the samplers; the sharpening shader is made whenever VR is on at
  the start; mipmaps only when filtering was on at the start (the item
  shows RESTART otherwise). test_menu: 458 checks, 0 failed. Not yet
  used in the headset.
- **Draw distance / "the back is empty" (user):** the game culls with its
  camera, ~16 m behind the player in first person, and nothing behind a
  camera is in any frustum through it, so the side planes' widening can't
  help. The five frustum tests are hooked (hooks.txt, gen regenerated):
  with head tracking and `vr_cull_all=on` (default) they answer "inside",
  so everything loaded is drawn. Built; headless with VR off unchanged;
  not yet seen in the headset (frame rate to watch). Texture pop-in: the
  async PNG decode shows the original for a frame or two; with everything
  drawn, textures behind should be requested before they're looked at.
- **Shop tops missing (user, captures `testwork/p4/cap16`):** in first
  person the upper parts of the town's shops (roofs, ceilings) are not
  drawn: "missing since forever in first person, unless I move closer, or
  in third person move lower". Not the five frustum tests (hooked to
  "inside", still missing), not psprecomp (BJUMP never taken: the GE
  draws all it gets). Likely a screen-space check against the game
  camera's projection: other readers of `0x08FA97A0` are `0x08AFF088`,
  `0x08D79B08`, `0x08D79CE4` (also `0x08FA9780`), `0x08D8C66C`,
  `0x08D8D158`, `0x08D8DB5C`, `0x08D9260C`, `0x08D929F0`, `0x08D93DB4`
  (VFPU-heavy; not decoded). Next: hook them with counters and a
  per-function "visible" switch, find the one at a shop. Also fixed: the
  capture folder is made with its parents (`testwork/p4/...` captures
  were all lost before).
- **Shop tops, then "angles not viewable in the original game are not
  rendered" (user):** the game camera is now applied at the player's head
  in first person (`hook_apply_camera`, ApplyCamera `0x08AFD474`; captures
  `cap18`: the dome, lamp and shop tops drawn), and the 3D scene keeps its
  back faces (`vr_two_sided`, psprecomp `two_sided`). Node visibility
  (`0x08D7D270`, counted by `hook_node_vis`, `vr_nodes1.log`): ~5250 nodes
  per 5 s, all visible in both views (none hidden by flags or tests), so
  whatever is still missing is dropped elsewhere. The method is now the
  `psp2i-hook-re` skill.
- **Edges of the eyes' view, then the "transparent rectangle" (2026-10-23,
  user: both fixed, "it works now"):**
  - GE dumps first vs third person: the game left out pieces behind or
    outside its camera at the head. Fixes: the camera at the head takes the
    head's full turn (`hook_apply_camera`, `g_fp_cam_hr`); SetProjection
    (`0x08D7DA78`, hooked) widens a field-like projection in first person
    (eyes' view x 1.25), measured at the scene walk (x 0.684 / y 0.743);
    the eyes take the widening out only when the GE's projection is ours.
  - The headset's view is 123 x 106 degrees (PS VR2, `vr_view_tan`):
    `vr_fov_auto` sizes the eye pictures to it (scales 2.65 / 3.37).
  - The rectangle was the game's light-shaft and haze fans: untextured,
    vertices at clip z -1 (screen space, near plane). psprecomp drops them
    with eye views (`ST_DROP`, `st_screen_extruded`; ~10k a run).
- **Open:** hiding the player's own head (bone weights, or an eye-forward
  offset), body-relative movement (today the stick moves relative to the
  game camera, which trails behind), the cull behind the game camera.
- **Walk where you look (2026-10-23, user: "feels right").** In first person
  the view has its own heading (`g_fp_yaw`): it follows the game camera
  only while the right stick turns it, after a jump of over 0.5 rad (a cut
  or camera reset), at recenter and when first person starts, so the game
  camera trailing round no longer swings the view. The left stick is turned
  by (that heading + the head's yaw - the game camera's, from its eye and
  target +0xFC / +0x108) in `ctrl_commit`, re-sent each poll while held.
  Log `vr: fp walk` (dev builds, every 2 s): in `vr_walk1.log` the walk
  went where the head pointed within ~10 degrees in all 14 samples (head
  -121 to +48 degrees); the game moves relative to its camera's eye-target
  heading. Only the stick changes (online-safe).
- **Attack nudge (VR_PLAN F1, 2026-10-23, user: "attacks after strafing go
  where I look").** After walking left, right or back the character faced
  where it walked and attacks went that way. A dev per-frame trace
  (`PSP2I_PL_TRACE=FILE`: the pad as sent, the headings, 4 KB of the player
  object and 512 bytes of the camera; analysed offline) found the facing
  (player +0x70's rotation, forward row +0x90 / +0x98), how the stick turns
  it (20 degrees a frame; let go mid-turn, it snaps to the stick's heading),
  that attacks don't turn it, and R's lock-on state (+0x214); ADDRESS_MAP.
  `fp_nudge` (host/main.c, called in `ctrl_commit` before the walk turn):
  a new Square / Triangle press in first person with the facing more than
  30 degrees from the view is held back; the stick points ahead (turned to
  the head by the walk code) for at least 2 frames and until the facing
  turns the head's way (6 at most), is centred 2 frames (the snap), then the
  attack is pressed (at least 2 frames, even if let go). Not while R is
  held or +0x214 is set, and not within 1 s of the last attack (combos and
  Just Attacks keep their timing). Setting `vr_fp_nudge=on/off`. Log
  `vr: fp nudge: ...` (dev). Runs: `f1_nudge1.log` turned once right and
  once wrong (pressed while still walking back: the facing was already
  turning from the player's stick, so it ended after 1 frame and snapped
  the old way; fixed with the 2 frames and the nearer-the-view check);
  `f1_nudge2.log`: 8 Square nudges from 38-124 degrees off all ended
  within 24 degrees, 2 skipped while locked on. Triangle not pressed in
  first person since the fix. Circle is not nudged (it talks in town).
  Only the pad changes (online-safe).
- **M0 hands in the world (VR_PLAN P5, 2026-10-23): the user sees them in
  the headset.**
  - **Setting:** engine menu VR > INPUT MODE (PAD / MOTION), `vr_input=` in
    the ini. MOTION shows the hands and buzzes both controllers once
    (`menu: VR input motion`). The controllers' buttons stay the PSP pad
    either way.
  - **VR thread** (`vr_xr_d3d11.c`): grip and aim pose actions plus a
    vibration output per hand, bound at the same paths in every profile
    (`HANDS`), with action spaces. `track_hands` locates them each headset
    frame. `vr_hands_get` hands them to the game relative to the recentred
    head, the same space as `vr_head`. `vr_haptic(hand, amp, s)` buzzes.
  - **Markers:** quad layers drawn by the compositor on top of everything
    (`marker_layers`): a 4.5 cm ring at the grip facing the eyes (left cyan,
    right orange) and a 30 cm ray along the aim. They move at the
    headset's rate, not the game's, but have no depth against the scene.
    That's a choice for M0; the plan's "psprecomp extra draw" waits for when
    the hands need to hide behind things.
  - **Game units** (`hands_frame` in main.c, called from `head_frame`): each
    3D frame the hands go through the eyes' base, level frame and scale,
    then out through the view matrix into world positions, the grip's axes
    and the aim ray (`g_hand_w`, for M2 to M4). Dev log every ~2 s:
    `vr: hand left: world ...; from the feet X across, Y up`. Check: a hand
    at the hip in first person should be ~3 units up from the feet
    (3.4 units per metre).
  - **vr_test** shows the markers, prints the hands each second, and Cross /
    Square buzz the right / left controller: an M0 check without the game.
  - **Not done:** emulated controllers and a command file for tests without
    a headset (PSUEx has them).
  - **Headset runs** (`testwork/p5/vr_m0_1.log`, `vr_m0_2.log`): the user
    sees the rings and rays at the hands (run 2). In run 1 they vanished
    after a moment: an accidental gamepad L3, not the markers. Run 2's log
    (`vr: hand markers: ...`, each change of what is sent and why) had 4
    marker layers every frame from the switch to MOTION on, both hands
    tracked. SteamVR's PS VR2 Sense show up as the Touch profile; in PAD
    mode all four poses dropped together twice for a moment (controllers
    idle) and came back. The buzz on the switch was not confirmed.
    Hands logged 2.0-3.6 units above the feet and 3.2-4.7 across (first
    person, 3.8 units/m): far out, partly the eyes' 1.2-unit forward
    offset; check against the body when M2's arms need it.
- **M3 prototype, melee by swinging (2026-10-23): built, not yet tried in
  the headset.**
  - **Detection:** in the VR thread at the headset's rate, as PSUEx-VR does
    it. A grip moving faster than `vr_swing_speed` (default 1.8 m/s) for
    2 headset frames is one swing. It counts again only once the hand has
    slowed under 0.8 m/s. The speed is the runtime's velocity (a
    `XrSpaceVelocity` on the grip's locate), or the position change when the
    runtime gives none. Swings are latched in `vr_pad.swing[]` (1 a swing, 2
    with that hand's grip), with the top speed.
  - **The press:** in `poll_controllers`, Square for 3 game frames
    (`SWING_PRESS`), or Triangle (the palette's second, Photon Art, slot)
    with the grip held. Either hand counts. Each swing buzzes that hand.
    It goes through the pad, so `fp_nudge` turns the character to the view
    first (F1). There's no Just Attack timing or combo buffer yet: that
    needs the game's attack state (VR_PLAN M3's reverse engineering).
  - **Changed in MOTION mode:** the right grip no longer presses Select
    (it picks the Photon Art). The left grip still turns the left stick
    into the D-pad.
  - **Logs and tools:** dev log `vr: swing right 2.6 m/s: Square`.
    `vr_test` prints swings too. Not in the engine menu yet (ini only).
  - **Headset runs** (`vr_m3_1.log`, `vr_merge1.log`, the user: "it works"):
    24 swings from both hands, Square and Triangle, 1.8-3.1 m/s. The nudge
    turned the character first 7 times and stayed out while locked on.
- **MOTION mapped as in PSUEx-VR (2026-10-23, the user's pick of the
  layout): built, not yet tried in the headset.** In `input_update`; PAD
  mode is unchanged. Table in VR.md "Controls".
  - **Attacks:** a trigger attacks like a swing (Square). That hand's grip
    held with a swing or trigger gives Triangle (Photon Art).
  - **Grips and clicks:** a grip alone is a button after 150 ms: left R
    (lock-on, PSUEx-VR's LB), right L. A grip used for a Photon Art sends
    nothing until let go. The right stick click is START. The left stick
    click alone is PSP2i's manual aim (L + D-pad down for 120 ms; the user:
    the game aims with the left stick then).
  - **Both grips + a button** are the VR shortcuts: Cross recenters, Circle
    switches first / third person (`VR_HOST_RECENTER` / `VR_HOST_VIEW`, as
    the gamepad's L3), Square opens the engine menu, the left stick click
    toggles the left stick as the D-pad. Triggers do nothing during a chord
    (kept for the hand calibration).
  - **Open:** in first person the walk code turns the left stick to the
    head; in manual aim that may turn the aim too (not seen yet). Whether
    the same L + down leaves manual aim isn't known.
- **The intro movie stuck with the headset on (user, 2026-10-23): a likely
  cause fixed, not yet seen in the headset.**
  - **Evidence:** in `vr_m0_2.log` the movie started with the session
    FOCUSED, and the game went on only once the headset was off (the
    session SYNCHRONIZED, stereo off, then the title music). Windowed runs
    without the headset (`testwork/movie/`: plain, and `--stereo` with
    `PSP2I_VR_FAKE_HEAD`) play it alike: 29.8 fps, and the captures
    through the movie all differ. So it isn't the stereo drawing.
  - **The decision:** movie frames come from VRAM (`gpu_present` 0, about
    3700 in 75 s), so they reach the headset through the CPU
    (`vr_submit_frame`), not shared on the GPU. At render scale S the game
    sent the S-times picture (1920x1088 at 4x) every vblank. The VR thread
    converted it into its 5760x1632 upload while holding `g_frame_lock`,
    which each `vr_submit_frame` waits on. With the headset off the
    runtime asks for no frames, so no upload runs.
  - **Fix:** a VRAM frame goes at 480x272 (the S-times one is the same
    picture scaled up), and `upload()` copies the picture out under the
    lock and converts it after.
  - **Check:** dev log `vr: N pictures through the CPU in 10 s (WxH),
    conversion up to M ms`, every 10 s while they come. If the movie still
    sticks, that line and the frame rate tell whether this was the cause.
  - **Headset run** (`vr_menufind1.log`, dev counters `vr: pictures in 5
    s: ...` per branch): during the movie, all 300 vblanks per 5 s were
    VRAM frames and all were sent to the headset. Before the fix
    (`vr_movie1.log`) none were. The user hasn't yet said how it looks.
- **The game's "a menu is open" (for the left stick as the D-pad in menus,
  PSUEx-VR's auto switch): not found yet; a lead.**
  - **Method** (psp2i-hook-re): a dev menu finder labelled by the user
    (MOTION both grips + Triangle, or gamepad L3 held + R3). It samples
    user RAM every 10 game frames and scores each word: the share of menu
    samples that differ from its field value, minus the share of field
    samples that do. A strict "never the field value in a menu" rule lost
    every candidate at the second kind of menu. The framebuffer swap words
    (`0x04000000` / `0x04044000`) score high by chance and are noise.
  - **Best word: `0x08EE2F70`** (`vr_menufind3.log`, 15 flips): 0 in 375 of
    381 field samples, non-zero in 154 of 198 menu samples. Its change log
    (`vr_menuword1.log`) shows a counter, not a flag: 0..0x3F one step per
    game frame, wrapping, with pauses of 8-22 vblanks at 0 inside a menu.
    It is most likely the menu cursor's pulse animation. It started within
    ~1 s of 6 of 7 menus opening. It stops only at the end of its cycle,
    1-2 s after a menu closes. One menu (round 7) left it at 0 for 20 s
    while open. Nothing references the address directly. `0x088ADC34` and
    `0x088ADDC4` build the base `0x08EE2A60` (+0x510 = it).
  - **Which menus ran it** (the user): the title / continue menu, the start
    menu, an interior shop, the salon, the palette. Round 7 (0 for 20 s)
    is still unknown.
  - **Write watch and task log** (`vr_menuwatch1.log`, `vr_tasks1.log`,
    ADDRESS_MAP "Menus and tasks"): the counter is ticked by a UI animation
    update every frame. Its start was caught only for the title menu, whose
    own tasks (updates `0x089D06E8` / `0x089F0BBC` / `0x089DB0AC`) exist only
    while it is up. The field menus add no tasks: their open state sits
    inside always-running tasks and isn't mapped.
- **Left stick as the D-pad in menus (`vr_menu_dpad=on`, the user's pick
  "A for now"): built, not yet tried in the headset.** In
  `poll_controllers`, with VR on, for any left stick (controllers or
  gamepad). While the counter `0x08EE2F70` has changed within the last 30
  vblanks, the stick presses the D-pad (past half way) and is centred.
  Known flaw: it stays on for up to 1-2 s after a menu closes, until the
  counter's cycle ends. Start (closing the start menu) ends it at once,
  until the counter starts again. Log `vr: left stick is the D-pad / moves
  again`. VR off: unchanged (checked headless). The exact flag for field
  menus would remove the lag (open).
  - **Headset runs** (`vr_menudpad1/2.log`): the start menu worked; in a
    shop the counter ran only one cycle as it opened, then stayed 0 for the
    item list, so the stick stopped working. A second signal was added: the
    menu layer's share of the screen per frame (HUD-pass sheets at w < 80),
    over 30%. Measured in the field 0-11%, probably a shop 27-32%, loading
    106-210%. The user: "it randomly works and doesn't".
  - **Now off by default** (`vr_menu_dpad=off`, the user's choice). The
    manual toggle stays (MOTION both grips + left stick click). The
    in-menu flicker is fixed (60 vblanks, not 30).
  - **The way to make it exact** (not started): menus ask the game's input
    library for D-pad presses (with repeat; `0x08D5B844` is that library's
    repeat timer), while walking reads the analog stick. Hook the
    library's query and let the left stick answer it, so no "menu open"
    detection is needed.

**Next (in order):**
1. **Try the controller capture in the headset:** left grip + left stick
   click (added after the fifth run; built, not yet pressed in the headset).
   It latches in the VR thread (`g_shot`) and reaches `vrshot_request` through
   `VR_HOST_SHOT`; without the grip the click still recenters.
   **Recenter never worked for the user.** No log has a recenter after the
   automatic one at start, though SteamVR's generated binding maps the
   left stick click (PS VR2 `left_stick` click) to `recenter`. Also, a
   SteamVR "Reset view" moved LOCAL without the game taking the head again.
   Added: `REFERENCE_SPACE_CHANGE_PENDING` (LOCAL) recenters too
   (`vr: the runtime recentered`), a log line on each left stick click, and
   one at focus saying whether the stick clicks are bound. Built, not run:
   the next run's log tells whether the click arrives.
2. **Sharpness: taller eye pictures, built, not yet seen in the headset.**
   The user: "blurry and pixelated in VR". The eyes see 1.6x the game's
   view across but 2.6x up and down (about 99 x 94 degrees), on 1440x816:
   pixels 1.7x taller than wide. Now, with VR + stereo + head tracking and
   render_scale > 1, d3d11.c renders `g_scale_y` rows per PSP row (main.c
   picks it for the same pixels per degree both ways: 5 at 3x), so the eyes
   are 1440x1360; the VR swapchain follows (`vr_config.scale_y`). VRAM
   readback, the window and the S-times picture stay S x S; the transfer
   shader takes the scale down in `rng.w`. One start-up check:
   `d3d11: VR eye pictures 1440x1360`, swapchain 4320x1360 (`vr_sharp1.log`;
   the headset wasn't on, so no frames judged). Next: the user's verdict
   and the frame rate; then `render_scale=4` (1920 x about 1800) if wanted.
   **Headset run (`vr_sharp2.log`, `cap9`):** the user: "it's smooth", but
   "from time to time it freezes to flat for a moment". 24-28 fps (30
   cap; the run before ~29). Captures: 1440x1360 eyes, drawn right.
   The flat moments were mostly the timed captures: a capture sent its
   frame's eyes through the CPU instead of sharing them, and the headset
   showed it flat. Fixed (built, not run): the eyes stay shared and are
   also read back (`d3d11_eye_readback`). The run also had 3325 frames from
   VRAM (8 the run before): not explained yet (the intro movie?).
   **Stick clicks:** at focus SteamVR reported both stick-click actions
   unbound (`stick clicks bound: ... no, ... no`), and no click arrived,
   though the generated binding maps them. **Explained:** the user plays
   with a normal gamepad (no motion controls yet), and recenter was only on
   the VR controllers' left stick click. Added: gamepad L3 tapped recenters,
   held 1 s saves a VR capture (`vr: gamepad L3...` log lines); the engine
   menu was already on R3 / Guide. **Headset run (`vr_pad1.log`, `cap10`):**
   9 L3 taps each recentered, 4 L3 holds each saved a capture; 24-29 fps;
   195 frames from VRAM (3325 the run before). Two flat switches near
   captures: three of those captures were of the same full-screen HUD (a
   menu, which goes flat anyway), so whether captures still cause a flat
   moment isn't settled.
   The VR-controller binding log (`vr: bound to ...`, from the profile
   change) stays for when motion controllers are used.
3. Engine-menu items for `vr_stereo` and `vr_world_scale` (the menu has 31
   items and a 32-bit hidden mask: widen it first, see Gotchas). The world
   scale 10 units/m is a guess the user hasn't judged yet.
4. A cutscene / camera-cut fade, and the open items under "What exists" >
   stereo window.
5. Not VR: the 120 fps crash and the headless intro-movie stall.

**The `3ds_port` branch** is a placeholder (see Gotchas); don't work on it
unless the user asks.

## Status by phase

| Phase | State |
|---|---|
| P0 RE tooling | **Done, run in town and field:** the GE gets identity view matrices (camera folded into bones / world on the CPU). View matrix found in RAM at `0x08FA98A0`, player world position at player object +0x4B0 (2 reports each; writer not yet watched) |
| P1 PCVR theater | **Done, checked in the headset** by the user (2026-10-22): screen size, sharpness, colours, recenter and the button mapping in VR.md all right. Nothing to fix |
| P2 stereo third person | **Stereo window done and checked in the PS VR2 (2026-10-22):** each GE draw replayed per eye, both pictures on one world-locked screen, HUD at screen depth. User: "looks pretty good", "3D looks right and it's steadier". 3x at 60 fps in town: 59.7 fps average (min 55.9). Still open for P2: world-scale tuning, wider culling, menu items, cutscene handling |
| P3 head-rate decoupling | **Head tracking works in the PS VR2 (2026-10-22):** each eye drawn from the head at the GE (`psp_gpu_stereo.eye_views`, one pose per frame by tag), a projection layer at that pose with the game's FOV, the horizon levelled. User: the world stays still, leaning and recenter work; 30 fps steady. Since then: a view of about 99 x 94° (border almost gone, but pixelated), the game's cull widened 8x (hook on `0x08D7D63C`), and the HUD and menus the PSUEx-VR way: a body-following HUD panel (user: "all looks working now"), labels, reticle and effects in the world (user-confirmed), menu scenes on the flat screen. Still open: sharpness, the FramePacket replay at headset rate |
| P4 to P8 | Not started |

## Branches and PR

- Main repo: branch `vr-theater` on `origin` = hasanzainul10/PSP2I_Recomp_VR,
  PR https://github.com/hasanzainul10/PSP2I_Recomp_VR/pull/1 into `master`.
  `upstream` = zmbkilla/PSP2I_Recomp (never push there).
- psprecomp submodule: branch `vr-draw-observer` on hasanzainul10/psprecomp
  (remote `fork` in the submodule; its `origin` is zmbkilla/psprecomp).
  `.gitmodules` points at the fork. Commit `12430e8` adds
  `psp_gpu_set_draw_observer` (render.h, gpu.c); `09e9ba3` and `288a1c4` add
  the GE stereo replay (`psp_gpu_set_stereo`, `draw_eye`, `mono_is_left`);
  `088e6cf` the per-eye view matrices for head tracking (`eye_views`,
  `on_frame`, `psp_gpu_stereo_tag`).
- Now: main repo `vr-theater` at `315939a` (the HUD ring) plus this doc
  update; psprecomp `vr-draw-observer` at `8283d3a` (`hud_panel` gets the
  screen share).
- `3ds_port` (main repo, origin): a placeholder branch from `fd149a1`.

## What exists

- `port/vr/vr.h`, `port/vr/vr_xr_d3d11.c`: OpenXR theater on its own thread.
  The game hands the composed 480x272 picture (`vr_submit_frame`, from
  `on_vblank` after `compose()` in `port/host/main.c`). It reads the
  controllers as a PSP pad (`vr_pad_get`, merged in `poll_controllers` /
  `update_camera_stick`).
- Settings: `psp2i_display.ini` `vr=`, `vr_distance=`, `vr_width=`.
  `--vr` / `--no-vr`. Engine menu VR page (`menu.h` MENU_VR*, MFX_VR*).
- `port/cmake/FetchOpenXR.cmake`: loader 1.1.63, headers only, DLL copied.
  Compiled only when headers are fetched (`PSP2I_VR`).
- `port/vr/vr_probe.c`: F8 (dev builds) writes `logs/vr_probe.txt`. Since
  2026-10-22 it also searches RAM for the camera matrix: rigid 4x4 matrices
  that map a float triple of the local player object (`gamelog_local_player`)
  to the player model's eye-space position.
  `PSP2I_VR_PROBE_EVERY=N` logs the camera to the console.
  `PSP2I_VR_PROBE_WATCH=ADDR` (or `camera+OFF`, the active camera object)
  arms the write watch at each F8 (the first 32 writes with their game stacks, in the console and
  `game_log.txt`). F8 also dumps the active camera object.
- Texture packs (merged from `texture-packs`, 2026-10-22): `port/host/texreplace.c`,
  `texture_packs=` in `psp2i_display.ini`. The user's packs are extracted in
  `FinalBuild/texture_packs/{base,extra}`. The extra pack's `textures.ini` names
  `HighRes-UI/*.png` files the archive doesn't contain (a separate download),
  so those stay original.
  - **Keys checked against PPSSPP 1.17 itself (2026-10-22):** PPSSPP with
    "Save new textures" on the same ISO and save. The port's keys are
    PPSSPP's (title: 11 of 11 non-text textures; town and field: 181 of the
    port's misses have exactly PPSSPP's key). The packs cover only about 27% of
    what's on screen in town and the field (204 of 749 textures in PPSSPP too),
    mostly environments, so many models stay original in PPSSPP as well.
  - **One fix:** 512-tall swizzled CLUT4 textures (the title background, NPC
    portraits) were hashed at 512 rows. PPSSPP master does that, but packs
    from 1.17 and earlier keyed them at maxSeenV (272): psprecomp `gpu.c`
    now offers both keys.
  - **Text never matches:** the port draws the game's text with Windows fonts,
    so text atlases (palette key `48DCABF9`) differ from PPSSPP's.
  - **Dev tools:** `PSP2I_TEXREPLACE_LOG=1` (every miss);
    `PSP2I_TEXREPLACE_DUMP=DIR` saves each missed texture once as
    `<key>.ppm`, plus `<key>.bin` (GE registers, the CLUT, 512 KB of memory
    from the texture address) to recompute keys offline. Dumps go under
    `FinalBuild/` (game data; never commit them).
- **Internal resolution** (`render_scale=` 1..4 in `psp2i_display.ini`,
  `--render-scale N`; Direct3D 11 only, from the next start; default 1):
  - Render targets and depth buffers are S times the PSP's size. VRAM, dirty
    rows and scissors stay in PSP pixels.
  - Uploads repeat pixels; readbacks take the sample nearest each pixel's
    centre; GPU transfers between targets keep the sub-pixel position.
  - `d3d11_present_hires` hands the S-times frame to the window (present.c)
    and the headset. The VR swapchain gets the same scale, so 3x is
    1440x816. The overlay is still drawn at 480x272, and each pixel it
    changes covers its S x S block.
  - Checked: title capture at 3x is sharp; the same draws, readbacks and
    flips at 1x and 3x. The user: "much sharper" in the PS VR2 at 3x.
  - Cost: in a 1-minute town run at 3x, half the frames took 17 to 25 ms
    (the CPU-access readbacks are 9 times bigger); the average stayed about
    28 fps at the 30 cap. The user's ini: `render_scale=3`.
- **Stereo window (P2, 2026-10-22)**, approach in VR_PLAN.md "P2 approach":
  - psprecomp `psp_gpu_set_stereo` (gpu.c `hw_draw_eyes`): after the mono
    draw, perspective draws (PROJ[11] ≠ 0) move per eye in clip space:
    `c·(dist/D − 1)` along view x for an eye at x = c, D = convergence. The
    backend gets them through the new `psp_gpu_backend.draw_eye`.
    `flat_after_clear`: once the scene target is cleared a second time in a
    frame, the rest of the frame is flat (PSP2i's HUD pass: perspective quads
    64-86 units away after that clear, seen in a `--ge-dump`). Bloom/blur
    passes are orthographic, so they are never moved.
  - d3d11.c: eye copies of each target and depth buffer that takes an eye
    draw; uploads and `rt_xfer` are repeated into them; render-to-texture
    samples the same eye's copy (bloom per eye). `d3d11_present_eye` reads
    the display target's eye copies back with the mono frame.
  - vr_xr_d3d11.c: one swapchain, left | right; two quad layers (eye
    visibility LEFT / RIGHT) at the screen pose. Stereo: width =
    2 · distance · tan(half FOV x) (the game's 0.731), so the window shows
    the world at its own angular size. IPD from `xrLocateViews` (logged).
  - main.c: `vr_stereo=on` (default), `vr_world_scale=10.0` (game units per
    metre: a first guess; IPD = scale × headset IPD, convergence = scale ×
    screen distance). Stereo runs while VR is RUNNING with D3D11.
    The overlay (engine menu) is put on both eyes at screen depth.
  - Dev: `--stereo` (eyes without a headset), `--overlay-shot` then writes
    `overlay_N_L/_R.ppm`; `--ge-dump VBLANK` = F12 at that vblank.
  - Checked (windowed, scripted to town, 2x): both eyes complete (lighting,
    bloom, textures, HUD); the player (~55 units) is 9 px apart at 960 wide,
    as the formula gives (9.2), far walls 13-14 px; HUD identical in both
    eyes. 2x stereo averaged 29.8 fps over 150 s (min 20).
  - **Performance (headset, 3x):** the first headset run gave 2-12 fps: each
    vblank copied and converted three 1440x816 pictures on the game thread
    even without a new frame. Fixed (2171e61): one copy per game frame
    (`rt_entry.gen`), 8888 without tables, and no mono 3x picture in stereo
    (the window shows 480x272). Then: windowed 3x stereo 29.6 fps; in the
    headset the user found it smooth, the game log said 19.4 fps average
    (frames mostly 33-50+ ms busy, 16% idle). Next: hand the eye textures to
    the VR device on the GPU (shared textures) instead of read back +
    convert + upload, and replay only the eyes (skip the mono draws while the
    window isn't needed).
  - **GPU handoff (done, 2026-10-22):** d3d11.c copies both eye copies side
    by side into one texture with a DXGI shared handle and keyed mutex 0
    (`share_eyes`, `d3d11_eye_share`, `d3d11_present_eye_shared`); the VR
    thread opens it on its own device and `CopyResource`s it into the
    swapchain image (`upload_shared`). The swapchain prefers
    R8G8B8A8_UNORM_SRGB now (same family as the game's targets). If the open
    fails (another GPU, size or format) `vr_share_failed()` turns on and the
    CPU path is used. The port's overlay (FPS counter, engine menu) is its
    own quad layer with source alpha at screen depth (`vr_submit_overlay`),
    since the shared pictures don't carry it. Colour: the shared copy keeps
    8 bits per channel (the CPU path quantized to the 5551 display format).
  - Headset run with it (user: "looks right"): 1984 frames shared, 3 via the
    CPU. While stereo was on (first ~60 s, 30 fps target): 28.2 fps average,
    game thread 60% idle (13% on the CPU path), some frames still 3-4
    vblanks. Then the user set 60 fps in the menu and the session went to
    WAITING (stereo off) for the rest, so stereo at 60 fps and longer stereo
    play were not measured in that run.
  - **Stereo at 60 fps (headset, town, 2026-10-22):** the user saw 3D
    throughout, "fps varies". Log: 13810 frames shared over 5 min; per
    minute 50.3 / 56.4 / 53.5 / 59.9 fps average (min 30), game thread 18-58%
    idle. The session dropped to WAITING (not VISIBLE/FOCUSED) every 20-40 s
    while the user still saw 3D, and each drop turned stereo off for a
    moment. Fixed: stereo stays on until 3 s without RUNNING, and the log
    names each session state (`vr: session state ...`). Run in the headset
    since (below).
  - **Two pictures instead of three (2026-10-22):** `psp_gpu_stereo.mono_is_left`
    moves the game's own (mono) draw to the left eye's position, so
    `draw_eye` runs for the right eye only; d3d11.c `eye_picture()` takes the
    target itself as the left eye (`d3d11_set_mono_left`). VRAM and the
    desktop window then hold the left eye's picture (local only; online
    unaffected). Headset, town, 60 fps (user: "3D looks right and it's
    steadier"): 59.7 fps average, min 55.9 over a minute, game thread 42%
    idle; before 50-56 average with dips to 30.
  - Session states seen with SteamVR: FOCUSED -> VISIBLE -> SYNCHRONIZED and
    back (the dashboard / system overlay). Short dips keep stereo now; one
    over 3 s turned it off and on again as intended.
  - Not done: the window doesn't follow the head (P3: head tracking +
    projection layer); objects nearer than the screen distance come out of
    the window and may cross its edge; world-space name labels in the HUD
    pass are flat; culling not widened; eye copies go through the CPU (share
    the textures with the VR device next); no menu items for stereo yet.
- `port/tests/vr_test.c`: the theater with a test picture, without the game.
- Menu tests: `test_menu` (445 checks), including `test_vr_page`.

## Local game setup (not in git)

Everything is under `FinalBuild\` (git-ignored):
- **ISO:** `PSPo2 Infinity EN.iso`, the English fan patch v2 applied with xdelta3
  to the JP ISO (SHA256 d8ef48d7…591b5b). The JP ISO is moved to `FinalBuild\jp\`.
- **The JP disc can't be used:** its `MEDIA.FPB` is PGD-encrypted and psprecomp
  has no `sceIoIoctl 0x04100001`, so the game hangs on a black screen.
- **EBOOT:** `EBOOT.BIN` is the English ISO's EBOOT, already a plain ELF.
  `EBOOT_JP.BIN` is a PPSSPP dump of the JP one.
- **GameData:**
  - `flash\font`: the firmware fonts.
  - `ms\PSP\SAVEDATA\NPJH50332DL`: the DLC.
  - `ms\PSP\GAME\NPJH50332`: the extra Infinity Missions.
- **Save:** a character exists (saved 2026-10-22) in
  `GameData\ms\PSP\SAVEDATA\NPJH50332`. The game autosaves
  (for example after a quest). Start in the field is the game's own screenshot (JPGs in
  `ms\PICTURE\NPJH50332SS`), not a port bug.
- **Headset:** the user's is a PS VR2 on SteamVR (the OpenXR runtime). Its Sense
  controllers also appear as SDL joysticks.

Build and run steps: the `psp2i-build-run` skill (`.claude/skills/`).

## Next steps

1. **The camera chain is mapped** (ADDRESS_MAP.md, from "Who sets the view"
   down to "The orbit controller"):
   - orbit controller → **OrbitCamera_Apply `0x08AFA598`(ctrl, eye*, target*)**;
   - → SetEye `0x08AFD598` / SetTarget `0x08AFD5C0` (camera +0xFC / +0x108,
     dirty +0x130);
   - → UpdateCamera `0x08AFD3FC` → LookAt `0x08D8969C` (view at camera +0x10);
   - → ApplyCamera `0x08AFD474` → SetViewMatrix `0x08D81D84` (the global
     `0x08FA98A0`) → bones and world matrices on the CPU.

   The hook points: `0x08AFA598` for head tracking and first person (P3/P4),
   and `0x08D81D84` (or LookAt) for the per-eye view (P2). Not hooked yet; all
   decoded from the code plus write watches in one field. Still open: which
   camera the `0x08969314` SetViewMatrix call (translation x 6.31) is.
2. **Player yaw:** near player +0x4B0 (position), find the angle or rotation
   that follows turning (two F8 reports with a turn, or `--log-floats`).
3. **The 120 fps crash:** at the 120 fps target the game read a bad object
   pointer (`0x41920BD8`, a float) in `0x08AC2910` (from the main update
   `0x08B821B8`), about 40 s after entering an area (2026-10-22, first run). Not
   seen at `--fps 30`. Not VR code; not investigated.
4. **Find the rest of what P2 needs:** the frustum cull and the bloom/blur pass
   signature (F12 dump).
5. **P2 stereo window done** (above, and the handoff section's "Next").
   Done since: the headset check, 3x performance, eye textures shared with
   the VR device, two renders instead of three. The original notes: every 3D draw reaches the GE in eye space (identity VIEW;
   WORLD and bones are already model-view). So stereo means swapping the view
   in `0x08FA98A0`'s builder per eye (two game-side renders), or a per-eye
   offset applied in eye space at the GE (cheap, but only right for an IPD
   shift). See VR_PLAN.md "Architecture":
   - extend the psprecomp backend contract (through flag);
   - per-eye targets separate from VRAM;
   - HUD split to a quad.

## Gotchas learned

- **The `3ds_port` branch** (origin, made 2026-10-22 from vr-theater at
  fd149a1) is the user's placeholder for a later 3DS port with stereo 3D,
  separate from VR. It shares the per-eye GE replay. Nothing on it yet.
- **Headless runs stall in the intro movie** (flips stop at 2739, mpeg ring
  buffer loop), with or without audio (SDL dummy driver). Windowed runs with
  the same `--press` script reach town (usually; one windowed run stalled
  too). Use windowed runs (tell the user) for 3D checks.

- **No decryption in allegrexrecomp:** it can't decrypt `~PSP` EBOOTs (its
  `decrypt` command is unfinished). Use a plain ELF EBOOT.
- **`emit` needs `port/gen`** to exist already ("cannot write gen/recomp_funcs.h").
- **SteamVR start-up time:** the first OpenXR call can block for a long time
  while SteamVR starts. `vr_stop` waits 5 s and then gives up.
- **Menu bits:** `MENU_BIT` and `hidden` are 64 bits wide since the VR
  page got HUD DISTANCE and WORLD SIZE (33 items; 32 was the old limit).
- **PSP2i's GE VIEW is always the identity:** the camera is multiplied into
  WORLD and the bone matrices on the CPU. Reading the camera from the GE gives
  nothing; use the RAM matrix at `0x08FA98A0`.
- **Through-mode draws aren't tagged** to the backends yet. P2 needs that
  (gpu.c `g_through`).
