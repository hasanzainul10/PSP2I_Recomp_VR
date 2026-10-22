# VR progress (handoff)

Where the VR mod stands, so any session or Claude account can continue. Update
this file at the end of every working session: what changed, what was checked,
what is next. The plan itself is [VR_PLAN.md](VR_PLAN.md); user-facing docs are
[VR.md](VR.md); reverse-engineering results go in [ADDRESS_MAP.md](ADDRESS_MAP.md).

_Last updated: 2026-10-22._

## Status by phase

| Phase | State |
|---|---|
| P0 RE tooling | **Done, run in town and field:** the GE gets identity view matrices (camera folded into bones / world on the CPU). View matrix found in RAM at `0x08FA98A0`, player world position at player object +0x4B0 (2 reports each; writer not yet watched) |
| P1 PCVR theater | **Done, checked in the headset** by the user (2026-10-22): screen size, sharpness, colours, recenter and the button mapping in VR.md all right. Nothing to fix |
| P2 stereo third person | Not started |
| P3 to P8 | Not started |

## Branches and PR

- Main repo: branch `vr-theater` on `origin` = hasanzainul10/PSP2I_Recomp_VR,
  PR https://github.com/hasanzainul10/PSP2I_Recomp_VR/pull/1 into `master`.
  `upstream` = zmbkilla/PSP2I_Recomp (never push there).
- psprecomp submodule: branch `vr-draw-observer` on hasanzainul10/psprecomp
  (remote `fork` in the submodule; its `origin` is zmbkilla/psprecomp).
  `.gitmodules` points at the fork. Commit `12430e8` adds
  `psp_gpu_set_draw_observer` (render.h, gpu.c).

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
5. **Start P2:** every 3D draw reaches the GE in eye space (identity VIEW;
   WORLD and bones are already model-view). So stereo means swapping the view
   in `0x08FA98A0`'s builder per eye (two game-side renders), or a per-eye
   offset applied in eye space at the GE (cheap, but only right for an IPD
   shift). See VR_PLAN.md "Architecture":
   - extend the psprecomp backend contract (through flag);
   - per-eye targets separate from VRAM;
   - HUD split to a quad.

## Gotchas learned

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
