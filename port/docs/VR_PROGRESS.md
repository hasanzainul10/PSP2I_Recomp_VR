# VR progress (handoff)

Where the VR mod stands, so any session or Claude account can continue. Update
this file at the end of every working session: what changed, what was checked,
what is next. The plan itself is [VR_PLAN.md](VR_PLAN.md); user-facing docs are
[VR.md](VR.md); reverse-engineering results go in [ADDRESS_MAP.md](ADDRESS_MAP.md).

_Last updated: 2026-10-22 (P3: wider view, wider cull, HUD pass seen from the head; checked in the headset)._

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
  leaning moves the view; recenter works.
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

**Next (in order):**
1. **The big HUD menus:** make them comfortable (see the options in the
   session: smaller or nearer, or turned to face the head).
2. **Sharpness:** try `render_scale=4`; the real fix is eye pictures taller
   than 480:272 (the vertical scale stretches pixels).
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
| P3 head-rate decoupling | **Head tracking works in the PS VR2 (2026-10-22):** each eye drawn from the head at the GE (`psp_gpu_stereo.eye_views`, one pose per frame by tag), a projection layer at that pose with the game's FOV, the horizon levelled. User: the world stays still, leaning and recenter work; 30 fps steady. Since then: a view of about 99 x 94° (border almost gone, but pixelated), the game's cull widened 8x (hook on `0x08D7D63C`), and the HUD pass seen from the head (labels, reticle and effects stay in the world; the HUD stays in front of the game camera). Still open: the big HUD menus seen at an angle, sharpness, the FramePacket replay at headset rate |
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
- Now: main repo `vr-theater` at the P3 head-tracking commit (after
  `5df9c1e`); psprecomp `vr-draw-observer` at `c8ee358` (wider eye views, the HUD pass from the eyes).
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
- **Menu bits:** `MENU_BIT` is 32 bits wide. The menu has 31 items now; adding
  more needs a wider `hidden` mask.
- **PSP2i's GE VIEW is always the identity:** the camera is multiplied into
  WORLD and the bone matrices on the CPU. Reading the camera from the GE gives
  nothing; use the RAM matrix at `0x08FA98A0`.
- **Through-mode draws aren't tagged** to the backends yet. P2 needs that
  (gpu.c `g_through`).
