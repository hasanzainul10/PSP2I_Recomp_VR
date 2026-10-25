---
name: psp2i-3ds-build
description: Build the PSP2i New 3DS port (port/3ds) with devkitPro inside MSYS2 - the .3dsx and the .cia, the HOME menu icon and banner, and the .text size budget line. Use when asked to build, rebuild or package anything for 3DS, after changing port/3ds, psp2i.rsf or 3DS code in psprecomp/host, or when the 3DS toolchain seems broken or missing.
---

# Build the 3DS port

Plan: `port/docs/3DS_PLAN.md`. Progress: `port/docs/3DS_PROGRESS.md`. The 3DS
work lives on branch `3ds_port`, in the worktree `D:\work\PSP2I_Recomp_3DS`.

## Toolchain (all on D:)

| What | Where |
|---|---|
| MSYS2 | `D:\msys64` |
| devkitPro (devkitARM GCC 16, libctru, citro3d, citro2d, picasso, 3dslink, smdhtool, 3dsxtool, tex3ds) | `D:\msys64\opt\devkitpro` (`/opt/devkitpro` inside MSYS2) |
| CMake + Ninja for the 3DS build | MSYS2 packages (`cmake`, `ninja`) |
| makerom 0.19 | `D:\Tools\makerom\makerom.exe` |
| bannertool 1.2.3 | `D:\Tools\bannertool\bannertool-1.2.3-windows\bannertool.exe` |
| Python with PIL (icon/banner) | the Windows Python; `build.sh` finds it, or set `PSP2I_PYTHON` |

Check it's all there:

```bash
ls /d/msys64/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc.exe /d/Tools/makerom/makerom.exe
```

Missing? Reinstall with the script kept at `D:\Tools\dl\dkp_setup.sh` (MSYS2
pacman, the devkitPro key and repos, then `3ds-dev 3dslink make ninja`).
- pkg.devkitpro.org answers **403 to curl's default user agent**; pacman's
  works. Use `-A "pacman/6.1.0 ..."` for any manual download.
- The first `pacman -Syu` on a fresh MSYS2 closes itself (a core update):
  run the script again.

## Build

From Git Bash (or PowerShell, same path):

```bash
MSYSTEM=MSYS /d/msys64/usr/bin/bash.exe -l /d/work/PSP2I_Recomp_3DS/port/3ds/build.sh          # Release
MSYSTEM=MSYS /d/msys64/usr/bin/bash.exe -l /d/work/PSP2I_Recomp_3DS/port/3ds/build.sh Debug
```

- Pass the script's **absolute** path: a login shell starts in `~`.
- Output in `port/3ds/build/` (git-ignored): `<target>.elf`, `.3dsx`,
  `.smdh`, `.cia`, and `assets/` (icon.png, banner.png/.wav/.bnr).
- The last lines are the budget line, `3ds: <target>.elf .text N MB`, and
  the file sizes. Quote the `.text` size whenever code size changed.
- When the game target joins (T1), the build takes long: run it in the
  background.

## What's in port/3ds

- `CMakeLists.txt`: the targets. `psp2i_package(target title description
  unique_id product_code)` makes the `.3dsx` and the `.cia` for a target.
  Each app needs its own unique ID (the title ID's low word is
  `unique_id << 8`: 0xF5220 → `000400000F522000`).
- `psp2i.rsf`: the `.cia` exheader. **124 MB memory mode** (SystemModeExt),
  **core 2** (CanAccessCore2), 804 MHz + L2, SD access, and the services list
  (at most 34 entries, includes `mvd:STD` for movies and `soc:U` for online).
  Don't take 178 MB: it starves the HOME menu.
- `make_assets.py`: icon and banner drawn from text. Never put game art in.
- `hello/`: the T0 check app (screens, memory, clock, cores; see
  `psp2i-3ds-test`).

## Rules

- `.3dsx` vs `.cia`: the **game is a `.cia`** (its exheader guarantees
  124 MB and core 2; the game needs ~100-135 MB). On the user's console a
  `.3dsx` from the Homebrew Launcher gets the same, so it's fine for
  development pushes; in Azahar a `.3dsx` gets 96 MB.
- A build of the game contains code generated from the user's EBOOT: never
  commit, publish or attach the `.3dsx` / `.cia` / `.elf` (copying it to
  the user's own console is fine).
- Host code for 3DS is under `PSP2I_3DS`, runtime code under
  `PSPRECOMP_3DS`; the PC, Android and VR builds must not change.
