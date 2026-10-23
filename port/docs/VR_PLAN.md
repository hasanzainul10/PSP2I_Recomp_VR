# PSP2i Recomp VR: Plan

## Context

Goal: a full VR mod of the PSP2i recomp. OpenXR on Windows (PCVR: Link, SteamVR, Virtual Desktop) first, then Android standalone (Quest 2/3/3S, Pico 4/4U) from the same code. It needs 6DoF head tracking, first and third person, gamepad and motion controls, body IK, Meta and Pico body tracking, and SteamVR full-body trackers. Online play must keep working.

PSUEx-VR (D:\work\PSUEx\PSUVR) proved the features players want, but it had to hook a D3D9 binary from outside. This project owns the whole GE pipeline, so we can do much more:

- **Every vertex goes through our C code.** `psp_gpu_prim` in `psprecomp/src/hle/gpu.c` (lines 1364-1433) runs skinning, world, view and projection on the CPU. It keeps the game's `g_world`, `g_view`, `g_proj` and `g_bone` (gpu.c:59, 1830-1844). The backends get screen-space triangles through `psp_gpu_vertex` (`psprecomp/include/psprecomp/render.h:77`).
  - **Stereo:** we can replace the view and projection per eye. No guessing at which baked matrix holds the camera, which was the hard part in PSUEx.
  - **Body:** we can replace the local player's bone matrices before skinning, so IK and body tracking drive the real model, weapons included.
  - **Hiding geometry:** we can drop head and hair triangles by bone weight. No clip-plane tricks.
- **Game behaviour:** hooks (`port/hooks.txt` + `psp_hook_set`, as in `port/host/camera.c`) let us read and steer the game's camera, aim, input and timing.
- **Platforms:** D3D11 (`port/host/d3d11.c`) and GL/GLES 3 (`port/host/gl_ge.c`) backends already exist. Android, through SDL3 and GLES 3.0, already builds.

The blockers today:
- Render targets are a byte-exact 480x272 VRAM mirror (`rt_get`, d3d11.c:433).
- On Windows, frames go out through a CPU readback to a second device (`present.c`).
- Through-mode (2D) draws aren't flagged to the backend.
- Bloom and blur are screen-space passes that reuse the framebuffer (d3d11.c:423, 1005).
- None of the camera, player, culling or lock-on structures are mapped. Only `0x08A16794`/`0x08AFA530` for camera input and `0x08A3DE1C` for "is local player" are known.
- The hook table is capped at 64 (`psprecomp/src/dispatch.c:113`).

## Must-haves for immersion and enjoyment (the feature set)

**Comfort (non-negotiable)**
1. The head is tracked at the headset's rate (72/90/120 Hz) **no matter what the game's frame rate is**. We record each game frame's view-space geometry and redraw it with the newest head pose every headset frame. This is the biggest comfort win, and the CPU vertex pipeline makes it possible.
2. The world only moves from your stick or your head. Smooth and snap turn. The horizon stays level, with the game camera's pitch and roll removed. Optional vignette while moving.
3. Recenter. Seated and standing modes with height calibration. A world-scale setting.
4. Cutscene, camera-cut and loading handling: fade through black on cuts. In-engine cutscenes show in stereo from a fixed viewpoint, or on a "theater" screen (a setting). FMV (H.264) always plays on the theater screen.
5. Mirror the game's fixed-camera scenes onto a big virtual screen instead of forcing stereo where it breaks (some menus, the title screen).

**Presentation**
6. Per-eye supersampled render targets, separate from VRAM: resolution scale, MSAA, anisotropic filtering, no 16-bit quantise or dither.
7. Wider culling and a longer draw distance. Otherwise the game's frustum (sized for 480x272) pops objects at the HMD's wide FOV.
8. Bloom and glow redone per eye (or switchable off), not copied from one eye.
9. HUD split out of the scene, then made diegetic:
   - HP/PP and the palette on the left wrist.
   - Minimap on the wrist or a hip panel.
   - Lock-on reticle and damage numbers in the world.
   - Dialog and subtitles on a panel near the speaker.
   - Menus on a curved, body-locked panel with a laser pointer.
10. Optional texture-replacement pack hook (upscaled UI and fonts make the biggest difference in VR).

**Embodiment**
11. Full body in first person: head and hair hidden by bone weight, the rest of the body visible. A mirror (the player's room) to see yourself.
12. 3-point IK (head + hands) for arms, spine lean and crouch, plus procedural legs that follow the game's walk animation.
13. Body-tracking sources behind one humanoid-pose interface:
    - Meta `XR_FB_body_tracking` and `XR_META_body_tracking_full_body` (Quest 3/3S legs, standalone and Link).
    - Pico `XR_BD_body_tracking` (Pico Motion Trackers).
    - SteamVR trackers through `XR_HTCX_vive_tracker_interaction` (Vive, Tundra and SlimeVR as OpenXR trackers).
    - `XR_EXT_hand_tracking` for fingers. Finger curl from controllers through `XR_FB/EXT` touch-sensing as a fallback.
14. Walking around the room moves the character (leashed). Your physical crouch is honoured.

**Interaction (motion controls)**
15. Weapons sit on the controller pose: the hand bone is overridden, and two-handed and twin weapons are handled.
16. Melee by swinging. The game's Just Attack window becomes a haptic buzz plus a combo buffer, carried over from PSUEx-VR (`VRHands`, combo buffer).
17. Guns aim where the controller points (hook the game's aim direction). Grip + trigger fires the Photon Art. Bows are two-handed.
18. Techniques by gesture or slot. Optional voice casting (the PSUEx `VRSpeech` Moonshine sidecar on PC).
19. Guard and dodge gestures. Haptics for hits, damage, item pickup and Just Attack.
20. Laser-pointer UI, controller label cards and a first-run setup wizard (ported from PSUEx `VRUiPointer`, `VRControllerLabels`, wizard).
21. Gamepad mode always available. Camera mode (1st/3rd) and input mode (pad/motion) are independent settings.

**Online safety:** VR changes only what we draw locally (bone overrides, culling, camera) and what we press (a synthetic pad through `psp_ctrl_set`). Nothing new goes to other players. They see the normal game animations.

**Later and nice to have:** HRTF spatial audio for SAS voices (`psprecomp/src/hle/sascore.c`) once emitter positions are mapped. Fixed and eye-tracked foveation on Quest Pro, Quest 3 and Pico. Passthrough lobby mode.

## Architecture

```
game thread (fibers)                          render/XR thread
psp_gpu_prim ──view-space verts + state──▶ FramePacket (double-buffered)
  │  local-player bone override (vr_body)        │
  │  through-mode → HUD target                   ▼
hooks: camera/aim/cull/timing          vr_render: per eye (multiview) re-project
input: vr_input → psp_ctrl_set         with latest head pose → XR swapchains
       + camera_set_stick              + quad layers (HUD, menus, theater)
```

**Changes in `psprecomp`** (a separate repo, zmbkilla/psprecomp, so these go there as a PR or fork):
- Add a VR extension to the backend contract in `render.h`:
  - per-vertex `vx, vy, vz` view-space position (before `g_proj`)
  - `through`, `is_skinned` and draw-tag fields in `psp_gpu_state`
  - a `psp_gpu_vr_hooks` struct with callbacks:
    - `override_bones(const float* bones, int n)`
    - `pre_view()`: the camera override
    - `classify_draw()`: scene, HUD, bloom or skip
- Make the rest of `psp_gpu_prim`'s output (lighting, fog, w-clip) consistent with a view that differs from the game's. Fog and lighting stay as the game computed them. Re-projection only changes the view and projection.
- Turn `MAX_HOOKS` into a dynamic table. Expose a `g_through` / draw-sequence tag for each draw.

**New host module `port/vr/`** (plain C like the host code, with a small C++ unit only where a library needs it):
- `vr_xr.c`: OpenXR instance, session, spaces, swapchains, extensions. Graphics bindings for D3D11 (Windows), GL (Windows GL) and GLES (`XrGraphicsBindingOpenGLESAndroidKHR`).
- `vr_frame.c`: the FramePacket record/replay. It runs the XR wait/begin/end loop on its own thread, decoupled from `on_vblank` (main.c:1234, sdl_main.c:558).
- `vr_render_d3d11.c` and `vr_render_gl.c`: per-eye re-projection shaders. `SV_ViewID` instancing on D3D11, `GL_OVR_multiview2` on GLES. Per-eye bloom, HUD and quad-layer composition. They reuse `texture_view` and the texture cache from d3d11.c and gl_ge.c, so textures stay shared.
- `vr_camera.c`: third person (game camera as the anchor, head 6DoF on top, pitch removed) and first person (eye = head bone + HMD offset, body-yaw steering through the `0x08A16794` hook path in camera.c).
- `vr_input.c`: OpenXR actions and interaction profiles (Touch, Touch Plus/Pro, Pico 4, Index, Vive wands, PS VR2 Sense). It builds the synthetic pad merged in `ctrl_commit` (main.c:799) and SDL `on_vblank`. Gesture detection and haptics live here.
- `vr_body.c`: the humanoid pose from the sources in item 13, IK, retargeting to the PSP2i skeleton, and head-mesh hiding.
- `vr_hud.c`: through-mode draws go to an alpha HUD target. Region crops become wrist and world panels. The `menu.c` and `login.c` images become quad layers.
- `vr_settings.c`: a VR page in `menu.c` following the CONTROLS/TESTS pattern, persisted in `psp2i_display.ini` / `psp2i.ini`.

**Android:** keep `PSP2iActivity extends SDLActivity` for audio, files and input. Hand SDL's EGL display, config and context to OpenXR.
- Manifest: `com.oculus.intent.category.VR` / `org.khronos.openxr.intent.category.IMMERSIVE_HMD`, Pico metadata, and the OpenXR loader AAR (the Khronos unified loader works on Quest and Pico).
- GLES 3.1+ plus multiview. A separate `vr` build flavour so the phone APK is untouched.

## Phases (each ends playable)

**P0: Reverse engineering and tooling** (it unblocks everything else)
- **Camera and player mapping.** Map the camera object (position, target, FOV) and the player transform (position, yaw). Use `PSP2I_GE_MATRIX_FLIP` (ge.c:273), F12 dumps (`dump_draw`, gpu.c:1291) and `psp_mem_watch_write`.
- **Local-player draw.** Map the model draw for the local player (bone palette, skeleton bone index map: head, neck, spine, hands, feet) and the weapon attach points.
- **Remaining functions to find:** the game's frustum/LOD cull, the bloom/blur pass signature, the lock-on target, and the aim direction.
- **Write it down** in a new `port/docs/ADDRESS_MAP.md`, as PSUEx did.
- **Headset-free testing:** port the PSUEx test setup (SteamVR null HMD plus emulated controllers, scripted input).

**P1: PCVR theater.** OpenXR session on D3D11. The existing frame goes onto a large quad layer, with the menu as a layer too. This validates the XR loop, the input and the build. It is the fallback mode for every scene.

**P2: Stereo third person (PCVR).** Per-eye high-res targets, separate from VRAM. View-space re-projection, the HUD split to a panel, per-eye bloom, wider culling, comfort set 2-5, VR menu page.

### P2 approach (decided 2026-10-22)

Two ways to get two eyes were weighed:

- **Two game-side renders through SetViewMatrix** (`0x08D81D84`): run the
  game's scene build twice with a per-eye view. Rejected for now: the scene
  build is interleaved with game logic, effects and the HUD in one display
  list, so running it twice means re-entering game code mid-frame (side
  effects on animation timers, particles, the online state).
- **One game render, each draw replayed per eye at the GE** (chosen). In
  PSP2i every 3D draw already reaches the GE in eye space (the VIEW register is
  the identity; WORLD and the bones hold the camera). So after `psp_gpu_prim`
  has transformed a draw for the game's own (mono) picture, it moves each
  vertex in clip space for the eye and hands the draw again to the backend's
  `draw_eye(eye, ...)`. Skinning, lighting and texture work are done once.

Details of the replay:

- **Per-eye offset** (`psp_gpu_set_stereo`): for an eye at view-space x = c
  (±half IPD in game units), a vertex at distance `dist` moves by
  `c·(dist/D − 1)` in view x. That is the eye's own camera plus an off-axis
  shift, so points at distance D (the convergence distance) have no disparity
  and points at infinity are one IPD apart on the screen plane. Only
  perspective draws move (PROJ with a w that depends on z). Through-mode (2D)
  and orthographic draws, the HUD included, are the same in both eyes: they
  sit at screen depth.
- **Per-eye shadow targets** in the D3D11 backend: every render target that
  receives an eye draw gets two more textures (and its depth buffer two more
  depth textures) at the internal resolution. The mono target keeps VRAM
  coherence exactly as before; the eye copies follow it: an upload from VRAM
  or a GPU transfer between targets is repeated into them, and a draw that
  samples a render target samples the same eye's copy. So bloom, blur and the
  final copy to the display buffer run per eye with the right eye's picture.
- **To the headset:** at each flip the display target's two eye copies are
  read back with the mono picture (asynchronously) and handed to the VR thread
  (`vr_submit_stereo`). The theater shows them as a **stereo window**: two
  quad layers at the same world-locked pose, one per eye. The window's width
  matches the game's horizontal FOV at its distance, and the world scale
  (game units per metre) sets IPD and convergence so the window is a portal
  into the game world at the right size.

Why a window and not a projection layer yet: a projection layer needs the
game camera to follow the head, or the whole world turns with your head. That
is P3 (head tracking on the camera chain, `0x08AFA598`) plus reprojection.
The window is world-locked and comfortable now, and its pictures are exactly
what a projection layer will need later (per-eye targets).

Costs: the CPU repeats clipping and triangle setup per eye (not skinning or
lighting); the GPU draws everything three times; two more readbacks per frame.
Sharing the eye textures with the VR device instead of reading them back is
the next optimisation.

**P3: Head-rate decoupling.** FramePacket replay at headset rate on the render thread. Game logic stays at 30/60 (`framerate.c`).

### P3 approach (decided 2026-10-22)

Head tracking is applied at the GE, in the same per-eye replay as P2's
stereo, not by writing the game's camera (`0x08AFA598` / LookAt /
SetViewMatrix were the other options):

- **Render-only for sure:** no game memory is written. The orbit controller
  saves the camera's current eye each frame (ctrl +0xB4), so a head offset
  written there would feed back into the game's camera. The camera object's
  inverse and direction vectors (+0x50, +0xD0) may steer movement too.
- **Only the 3D scene turns:** the HUD pass stays flat (`flat_after_clear`),
  and the sky and every other perspective draw before it turn with the head.
  At SetViewMatrix the three calls per frame (scene, scene, the `6.31`
  camera) would have to be told apart.
- **One pose per frame, known exactly:** psprecomp `psp_gpu_stereo.eye_views`.
  At the first moved draw after a flip the host's `on_frame` (main.c
  `head_frame`) builds both eyes' view matrices from the newest
  `vr_head_get` pose and names it with a tag. The whole frame uses them, and
  d3d11.c keeps the tag with its eye pictures (`rt_entry.eye_tag`). The VR
  thread shows each picture as a projection layer at the pose it was drawn
  with, so the compositor's reprojection turns it with the head between game
  frames. That is the cheap first form of head-rate decoupling. The
  FramePacket replay at headset rate is still the later step.
- **The maths:** the vertices are in the game's clip space, so each eye is
  `PROJ · E_eye · PROJ⁻¹` per draw (gpu.c `stereo_kmat`). `E_eye` is the
  inverse of the eye's camera in the game camera's view space: the
  `vr_level` axes (heading only, from world up in the view matrix
  `0x08FA98A0`) times the head rotation, at the head's position plus the eye
  offset, times the world scale.

The cost of this choice: culling stays the game's, so it has to be widened
separately (below).

**A wider view, the cull and the HUD (2026-10-22, in the headset):**
- **Wider eyes:** `fov_scale` / `fov_scale_y` divide the eyes' clip x / y
  (`S · PROJ · E · PROJ⁻¹`). The layer's FOV is the game's times the same
  scales (main.c `eye_tan`, `eye_tan_y`). Defaults 1.6 / 2.6 (about
  99 x 94°). The picture keeps 480:272, so the vertical scale makes its
  pixels taller. User: the border is almost gone, but the picture is quite
  pixelated.
- **The cull:** the game's frustum planes are built by `0x08D7D63C` from the
  projection (ADDRESS_MAP.md "The frustum cull"). A hook runs it with the
  projection's x/y scales divided by `vr_cull_scale` (default 8) and puts
  them back, so only the cull planes widen. User: no gaps noticed. Cost: town
  stayed at 30 fps with the game thread 40-50% idle.
- **The HUD pass seen from the eyes** (`ST_HUD` in gpu.c): PSP2i draws the HUD
  and its world labels after the second clear of the 3D target, as flat
  perspective sheets already projected for the game camera. With
  `eye_views` they get the eye transform too, keeping each vertex's depth
  value (z / w) so the depth tests and draw order stay the game's. A point
  keeps its direction whatever its depth, so names, the reticle and location
  labels stay on what they label (user: correct), and technique effects stay
  in the world. The HUD widgets stay in front of the game camera (the body)
  at their 6.5-8.6 m. Non-perspective HUD-pass draws on the 3D target are
  shrunk to the game's view instead (`ST_SHRINK`). The final copy to the
  display buffer, on another target, is untouched.
- **Still odd:** big HUD-pass menus (the start menu, the mission menu, the
  map in a mission) are flat sheets as wide as the game's view at ~7 m. When
  the head turns they are seen at an angle (user: "they stay but angle
  changes when head move").

**P4: First person.** Head-bone camera, head hidden by bone weight, body-relative movement, lock-on in the world, the mirror.

**P5: Motion controls.** Actions and bindings, laser UI, weapon on hand, swing / Just Attack / guns / techniques, haptics, setup wizard.

### P5 roadmap (2026-10-23)

Where it starts: the VR controllers already work as a PSP pad (buttons and
sticks through OpenXR actions, `vr_xr_d3d11.c`), but no hand poses are read
and nothing is known yet about the skeleton's hand bones, the attack state
or the aim. The user's controllers are PS VR2 Sense on SteamVR. The model is
PSUEx-VR (`D:\work\PSUEx\docs\VR_GUIDE.md` "Motion controllers"). Each step
ends playable, and gamepad mode stays as it is (input mode pad / motion is
its own setting, apart from 1st / 3rd person). Online: hands and weapons are
bone overrides drawn locally; attacks are synthetic button presses. Nothing
else changes in what other players see.

| Step | What the player gets | Work | Reverse engineering needed (psp2i-hook-re / static-re / vr-probe) |
|---|---|---|---|
| **F1 Attack nudge (first person, pad and motion)** | After walking left, right or back the character faces where it walked; now an attack goes where you look: pressing an attack (or a swing, from M3) first turns the character to the view | On an attack press in first person, when the character's facing is more than ~30 degrees from the head's heading (`g_fp_yaw` + head yaw, as the walk uses), hold the stick briefly toward the head (turned as in `ctrl_commit`), then send the attack; skipped while locked on (the lock-on picks the target) | The player's facing angle (near player +0x4B0, open in ADDRESS_MAP: an angle that follows turning), and how many frames of stick the game needs to turn; whether a pushed stick during an attack's startup turns it (then the nudge and the press can go together) |
| **M0 Hands in the world** | Two hand markers where the controllers are, in 1st and 3rd person; INPUT MODE (PAD / MOTION) on the R3 > VR page | Grip and aim poses (`xrCreateActionSpace`) handed to the game thread with each head pose, in game units through the same level frame and world scale as the eyes; a marker drawn per eye (psprecomp: an extra draw after the scene); haptics output (`xrApplyHapticFeedback`); emulated controllers and a command file for headset-free tests (as PSUEx) | None |
| **M1 Pointer for the engine menu** | Point at the R3 menu with the right controller, trigger to click, stick to scroll | A ray against the HUD/menu quad, its hit turned into menu rows (`menu.c` knows them) | None (game menus keep the D-pad; their item rectangles are unknown) |
| **M2 Arms and weapon on the hands** | First person: your arms and the weapon follow the controllers; third person: the character's arms do | Override the arm bones with 2-bone IK to the controller poses before skinning; hand calibration (both grips + both triggers, as PSUEx) | The bone palette per skinned draw (`0x0955A990` region, ADDRESS_MAP), which indices are the shoulders, elbows, hands; where the CPU writes them (hook the writer, override after it). The weapon model: whether it hangs off the hand bone (then it follows for free) |
| **M3 Melee by swinging** | Swing to attack, grip + swing for the Photon Art; a buzz marks Just Attack timing and early swings are held until it | Swing detection (hand speed over a threshold, per hand) into the attack button; the combo buffer | The weapon type in hand; the attack state and the Just Attack window (hook the code that grades a Just Attack: count it per frame, read its timer); which PSP button is which palette action |
| **M4 Guns and aim** | Point the gun to aim, trigger fires, grip + trigger is the Photon Art | The controller ray turned into the game's aim direction | The aim (free aim / lock-on) direction the shot uses: hook it and override with the controller's ray (local: the shot direction other players see comes from the game's own sync, check that first) |
| **M5 Techniques** | Rod / wand: swing or trigger casts a slot, grip picks the other | Slot buttons from gestures | The technique slots and how a cast is chosen (the palette) |
| **M6 Guard, dodge, feedback** | Guard and dodge by gesture; buzz on hits, damage, item pickup | Gestures into buttons; haptic events | Hit / damage / pickup events (hook their handlers, count) |
| **M7 Setup and polish** | First-run wizard, controller label cards, settings for every choice above | PSUEx `VRUiPointer`, `VRControllerLabels`, wizard ported to the engine menu | None |

F1 needs no controllers and helps the gamepad first person of today, so it
can come first. Order of risk: M2 is the big unknown (bones), so its reverse engineering
(the hand bone indices and their writer) can start alongside M0. M3 needs
M2 only for looks: swing detection works on the poses alone, so a
"swing = attack button" prototype can follow M0 directly.

**P6: Body.** 3-point IK, then the Meta, Pico and SteamVR tracker sources, then calibration (T-pose, height, arm span) and finger tracking.

**P7: Quest and Pico.** GLES XR binding, multiview, manifest and flavour.
- Performance: a CPU budget for the recomp plus the single vertex transform on XR2, dynamic resolution, fixed foveation, 72/90 Hz.
- Pico and Quest controller profiles. A CI APK.

**P8: Polish.** Diegetic HUD items, texture packs, spatial audio, voice, the full matrix test (1st/3rd x pad/motion x PC/Quest/Pico).

## Critical files

- `psprecomp/src/hle/gpu.c`, `psprecomp/include/psprecomp/render.h`, `psprecomp/src/dispatch.c` (runtime: separate repo)
- `port/host/d3d11.c`, `port/host/gl_ge.c`: they share the texture cache with VR rendering
- `port/host/main.c` (`on_vblank`, `ctrl_commit`, renderer selection), `port/host/sdl_main.c` (EGL, Android loop)
- `port/host/camera.c`, `port/host/framerate.c`, `port/hooks.txt` (new VR hooks), `port/host/menu.c` (VR page)
- `port/CMakeLists.txt`, `port/android/` (CMake, Gradle, manifest, PSP2iActivity)
- Reuse from PSUEx: `D:\work\PSUEx\PSUVR\{OpenXRRuntime,VRBindings,VRHands,VRControllerLabels,VRUiPointer,VRSettings,VRControllerSim,VRSpeech}`. Logic and binding tables, not the D3D9 bridge.

## Verification

- **Each phase on PC:** run under SteamVR's null HMD with emulated controllers, plus the Meta Link runtime. F12 frame dumps compare per-eye matrices. A frame-time overlay (`VRPerf`-style) shows XR frame timing against game frame rate.
- **Regression:** with VR off, the flat D3D11/GL/Android builds are byte-identical in behaviour (`port/tests`, input replay through `rec_pad`/`replay_step`).
- **Online:** two clients over the relay, one in VR, checked from the flat client's view.
- **Quest and Pico:** sideload the `vr` flavour. Check OVR Metrics / Pico performance overlay at a steady 72 Hz in town and in a busy mission.
