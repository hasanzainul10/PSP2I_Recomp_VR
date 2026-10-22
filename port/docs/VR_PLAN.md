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

**P3: Head-rate decoupling.** FramePacket replay at headset rate on the render thread. Game logic stays at 30/60 (`framerate.c`).

**P4: First person.** Head-bone camera, head hidden by bone weight, body-relative movement, lock-on in the world, the mirror.

**P5: Motion controls.** Actions and bindings, laser UI, weapon on hand, swing / Just Attack / guns / techniques, haptics, setup wizard.

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
