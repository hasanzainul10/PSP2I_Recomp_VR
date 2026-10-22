# PSP2i Recomp VR: notes for Claude

A static recompilation of Phantasy Star Portable 2 Infinity (PSP, NPJH50332)
being turned into a VR game: OpenXR on Windows (PCVR) first, then Android
Quest / Pico. The user (hasanzainul10) also made PSUEx-VR (D:\work\PSUEx), and
its feature set and lessons are the reference.

## Start here

1. Read [port/docs/VR_PROGRESS.md](port/docs/VR_PROGRESS.md): the current state, branches and next steps.
2. Then read [port/docs/VR_PLAN.md](port/docs/VR_PLAN.md) (phases P0 to P8, the architecture) as needed.
3. Skills in `.claude/skills/`:
   - `psp2i-build-run`: regenerate, build and run.
   - `vr-probe`: reverse engineering with F8.
   - `vr-headset-test`: OpenXR checks.

## Layout

- `psprecomp/` is a git submodule (runtime and recompiler). It's a separate
  repo: the user's fork is hasanzainul10/psprecomp, as remote `fork` inside the
  submodule. Commit there first, then commit the new submodule pointer here.
- `port/host/`: the Windows host (`main.c`), the GE backends (`d3d11.c`,
  `gl_ge.c`), the engine menu (`menu.c`), camera, frame rate and online code.
- `port/vr/`: VR code (`vr_xr_d3d11.c` theater, `vr_probe.c`).
  `port/docs/`: the VR docs.
- `port/gen/` is generated from the game's EBOOT and git-ignored. `FinalBuild/`
  holds the user's game files and is git-ignored. Never commit anything from
  the game (EBOOT, ISO, extracted data, saves, captures): see `.gitignore`.

## Rules

- **Online:** keep online play working. VR may only change what this game draws
  locally (cameras, bones, culling, HUD placement) and what it presses (the
  synthetic pad through `psp_ctrl_set` / `camera_set_stick`). Never change game
  state that other players see.
- **VR off means no change:** builds without OpenXR headers, and runs with
  `vr=off`, behave exactly as before.
  - Guard VR code with `#ifdef PSP2I_VR`.
  - Dev-only tools go in `#ifndef PSP2I_PROD`.
- **Style:** match the code around you. Plain C11 in host and VR code, comments
  that explain why, `fprintf(stderr, "vr: ...")` log lines, settings in
  `psp2i_display.ini` with a comment line written by `display_save`.
- **Menu tests:** after changing `menu.c` / `menu.h`, run `test_menu` (all
  checks must pass) and keep the main page within 272 px.
- **Record what you find:** addresses go in `port/docs/ADDRESS_MAP.md` with
  their source and status. Progress goes in `port/docs/VR_PROGRESS.md` at the
  end of each session.
- **Report results as they are:** "session RUNNING" isn't "seen in the headset".
  Say what was and wasn't checked.
- **Git:**
  - Work on a feature branch and push to `origin` (the user's fork).
  - Never push to `upstream` (zmbkilla/PSP2I_Recomp) or to zmbkilla/psprecomp.
  - End commit messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Long jobs:** a full build or a 60 s+ game run goes in the background.
  Generated code is ~200 MB of C.
- **Headset:** the user's is a PS VR2 on SteamVR. Ask before changing SteamVR
  settings (for example, the null driver for headset-free tests).

## Tools

- **CMake:** `D:\work\Cmake\bin\cmake.exe` (on PATH).
- **MSVC:** `D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat`.
- **Python** with PIL, to turn `.ppm` captures into PNGs.
- **Debug switches in the game:**
  - Command line: `--overlay-shot VBLANK` (saves a frame), `--seconds N`, `--headless`.
  - Keys: F12 (GE frame dump), F8 (VR probe).
  - Environment: `PSP2I_GE_MATRIX_FLIP=N`, `PSPRECOMP_IO_TRACE=1`.
