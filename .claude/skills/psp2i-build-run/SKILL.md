---
name: psp2i-build-run
description: Regenerate the recompiled game code, build psp2i.exe (with VR) and the tests, and run the game windowed or headless with frame captures. Use when asked to build, rebuild, regenerate gen/, run, test or screenshot the PSP2i port, or after changing hooks.txt, fixes.txt, psprecomp or host/VR code.
---

# Build and run the PSP2i port

All paths are relative to the repo root (`D:\work\PSP2I_Recomp_VR`). The
game files live in `FinalBuild/` (git-ignored); see `port/docs/VR_PROGRESS.md`
"Local game setup" if they are missing.

## 1. Regenerate `port/gen` (only after hooks.txt / fixes.txt / recompiler changes, or if gen/ is missing)

```bash
cmake -S psprecomp -B psprecomp/build && cmake --build psprecomp/build --config Release --target allegrexrecomp
mkdir -p port/gen
cd port && ../psprecomp/build/tools/allegrexrecomp/Release/allegrexrecomp.exe emit ../FinalBuild/EBOOT.BIN gen recomp --hooks hooks.txt --fixes fixes.txt
```

- `FinalBuild/EBOOT.BIN` must be a plain ELF (the English ISO's own EBOOT).
  allegrexrecomp cannot decrypt `~PSP` EBOOTs.
- Check the `fix:` lines: each shows the old word and the new one. A fix that
  doesn't apply means the EBOOT differs from the one `fixes.txt` was written for.

## 2. Build (run in the background: the generated C is about 200 MB)

```bash
mkdir -p port/testwork/tmp
export TEMP="$(cygpath -w port/testwork/tmp)" TMP="$(cygpath -w port/testwork/tmp)"
cmake -S port -B port/build          # first time only; fetches SDL3 and the OpenXR loader
cmake --build port/build --config Release --parallel --target psp2i vr_test test_menu
```

The outputs land in `port/build/Release/`: `psp2i.exe`, `vr_test.exe`,
`test_menu.exe`, `openxr_loader.dll`, `SDL3.dll` and `psp2i_atrac.dll`.

## 3. Tests

```bash
port/build/Release/test_menu.exe | tail -1      # expect "N checks, 0 failed"
```

## 4. Run

From `port/build/Release`:

```bash
./psp2i.exe --root ../../../FinalBuild/GameData --eboot ../../../FinalBuild/EBOOT.BIN [--vr|--no-vr]
```

- Headless smoke test: add `--headless --seconds 30 --no-audio`.
- Frames: `--capture-dir ../../testwork/shots --overlay-shot 2400` writes a
  `.ppm` at that vblank (60 per second). Convert it to PNG with
  `python -c "from PIL import Image; Image.open('x.ppm').save('x.png')"`, then
  look at it with Read.
- Logs: stdout and stderr (redirect to `port/testwork/<name>.log`); also
  `port/build/Release/logs/` (`game_log.txt`, `online_log.txt`, `vr_probe.txt`).
- At exit the game prints a report: display flips, d3d11 draws, firmware calls
  and threads. A black screen with few flips means it's stuck: look at what
  `user_main` waits on and at the `not implemented` lines.
- A windowed run opens a window on the user's desktop. Say so before starting
  one, and give it `--seconds`.
