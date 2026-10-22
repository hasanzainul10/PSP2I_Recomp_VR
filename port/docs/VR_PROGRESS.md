# VR progress (handoff)

Where the VR mod stands, so any session or Claude account can continue. Update
this file at the end of every working session: what changed, what was checked,
what is next. The plan itself is [VR_PLAN.md](VR_PLAN.md); user-facing docs are
[VR.md](VR.md); reverse-engineering results go in [ADDRESS_MAP.md](ADDRESS_MAP.md).

_Last updated: 2026-10-22._

## Status by phase

| Phase | State |
|---|---|
| P0 RE tooling | **Done:** probe (F8) and ADDRESS_MAP.md. **Not run yet** in a 3D scene: no camera or player address found so far |
| P1 PCVR theater | **Done, partly checked:** session RUNNING on PS VR2 / SteamVR while the game plays. Picture in the headset and button mapping not yet checked by eye |
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
- `port/vr/vr_probe.c`: F8 (dev builds) writes `logs/vr_probe.txt`.
  `PSP2I_VR_PROBE_EVERY=N` logs the camera to the console.
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
- **No save yet:** the user was in character creation. Saves will land in
  `GameData\ms\PSP\SAVEDATA\NPJH50332`.
- **Headset:** the user's is a PS VR2 on SteamVR (the OpenXR runtime). Its Sense
  controllers also appear as SDL joysticks.

Build and run steps: the `psp2i-build-run` skill (`.claude/skills/`).

## Next steps

1. **Check P1 in the headset with the user:**
   - screen size and sharpness, colours (sRGB);
   - recenter; button mapping: Cross, Circle, D-pad mode, engine menu;
   - Circle/Cross swap.

   Fix what's off.
2. **P0 runs:** get into town and a field, F8 twice a few steps apart. Analyse
   `logs/vr_probe.txt` (skill `vr-probe`). Record the camera object and the
   player position in ADDRESS_MAP.md.
3. **Find the rest of what P2 needs:** the view-matrix builder (`PSP2I_GE_MATRIX_FLIP`), the
   frustum cull, and the bloom/blur pass signature (F12 dump).
4. **Start P2:**
   - extend the psprecomp backend contract (view-space vertices, through flag);
   - per-eye targets separate from VRAM;
   - HUD split to a quad;
   - see VR_PLAN.md "Architecture".

## Gotchas learned

- **No decryption in allegrexrecomp:** it can't decrypt `~PSP` EBOOTs (its
  `decrypt` command is unfinished). Use a plain ELF EBOOT.
- **`emit` needs `port/gen`** to exist already ("cannot write gen/recomp_funcs.h").
- **SteamVR start-up time:** the first OpenXR call can block for a long time
  while SteamVR starts. `vr_stop` waits 5 s and then gives up.
- **Menu bits:** `MENU_BIT` is 32 bits wide. The menu has 31 items now; adding
  more needs a wider `hidden` mask.
- **Through-mode draws aren't tagged** to the backends yet. P2 needs that
  (gpu.c `g_through`).
