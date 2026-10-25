# PSP2i New 3DS port: progress

Plan: [3DS_PLAN.md](3DS_PLAN.md). Branch `3ds_port`, worktree
`D:\work\PSP2I_Recomp_3DS`. Skills: `psp2i-3ds-build`, `psp2i-3ds-test`.
The user's console: **New 3DS XL, Luma3DS**.

## Decisions

- **2026-10-25 · The game ships as a `.cia`.** Its exheader
  (`port/3ds/psp2i.rsf`) asks for the New 3DS 124 MB mode, core 2, 804 MHz
  and L2, so the memory doesn't depend on how the user starts homebrew. On
  the user's console (Luma3DS) a `.3dsx` from the Homebrew Launcher got the
  same 124 MB and core 2, but in Azahar a `.3dsx` gets 96 MB, and other
  entry points (a game takeover) give the host title's memory. So: `.cia` for
  the game, and the `.3dsx` (pushed over ftpd/3dslink, no reinstall) is fine
  for development on this console. 178 MB was left out: it takes memory from
  the HOME menu.
- **2026-10-25 · Renderer: citro3d on PICA200**, through `psp_gpu_backend`.
  Unai (the PS1 software GPU from PCSX4ALL) was evaluated and rejected: no
  depth buffer, affine texturing, PS1 command set (3DS_PLAN.md section 3).
- **2026-10-25 · `3ds_port` was fast-forwarded to `vr-theater`** (a1a3787), so
  it has the HUD pass, `draw_hud`, `mono_is_left` and the stereo work.

## T0: setup (done 2026-10-25)

Done:

- Toolchain on D: MSYS2 (`D:\msys64`), devkitPro `3ds-dev` (devkitARM GCC
  16.1, libctru, citro3d, citro2d, 3dslink), MSYS2 CMake 4.4 + Ninja; makerom
  0.19, bannertool 1.2.3, Azahar 2126.2 under `D:\Tools`.
- `port/3ds/`: `CMakeLists.txt` (`psp2i_package` makes the `.3dsx` and the
  `.cia`), `psp2i.rsf`, `make_assets.py` (icon and banner from text),
  `build.sh`, and the check app `hello/`.
- Rules in `CLAUDE.md` ("3DS port") and the two skills.

Checked in **Azahar** (2026-10-25):

| | `.3dsx` | `.cia` |
|---|---|---|
| Boots, both screens draw, 3D slider read | yes | yes (installed with `azahar -i`, title `000400000F522000`) |
| `app_mem` | 98,304 KiB (96 MB) | **126,976 KiB (124 MB)** |
| libctru heap / linear | - | 93,808 KiB / 32,768 KiB |
| CPU clock, cores | not modelled (stubs) | not modelled (stubs) |

In Azahar, the core test (v2) gives core 1 0.30 of a core (its time limit
emulated) and core 2 1.02, and the clock ratio 1.00 (no speedup modelled).

Checked on the **console** (New 3DS XL, Luma3DS, 2026-10-25; check app v2,
copied with ftpd, the `.3dsx` started from the Homebrew Launcher, the `.cia`
installed with FBI):

| | `.3dsx` (HBL) | `.cia` |
|---|---|---|
| Boots, both screens, 3D slider | yes | yes |
| `app_mem` | **126,976 KiB (124 MB)** | **126,976 KiB (124 MB)** |
| libctru heap / linear | 94,056 / 32,768 KiB | 93,808 / 32,768 KiB |
| Clock: loop ticks, speedup off / on | 100.4 M / 33.4 M, **ratio 3.01** | 100.4 M / 33.4 M, **ratio 3.01** |
| Thread asked for core 1 | ran on cpu 2, 0.90 of core 0's work | ran on cpu 2, 1.00 |
| Thread asked for core 2 | ran on cpu 2, 0.93 | ran on cpu 2, 0.90 |
| Core 0's own work while the worker runs | 1.00 | 1.00 |

What this means for the game:

- **124 MB**, with ~94 MB of heap and 32 MB of linear memory after the
  small check app's code. The game's code comes out of the same region.
- **804 MHz + L2 work** (`osSetSpeedupEnable` from our title).
- **One extra full core** (cpu 2) runs in parallel with core 0 without
  slowing it: the place for the GE translation / citro3d worker or audio
  (3DS_PLAN.md T4). A thread asked onto core 1 also landed on cpu 2 here, so
  core 1 (the system core, 30% time limit) wasn't tested; don't plan on it.
- The first console run's `cpu ~321 MHz` / `core1=2` lines (v1) assumed
  2 cycles per loop pass and only read the core number; v2 replaced both.

Not checked: the HOME menu icon and banner (look at them on the console).

## Next: T1 (compile and fit)

- `split_gen.py` into small units for GCC; the runtime (`psprecomp/src`) and
  the generated code built for ARM11 with `-Os`, no LTO.
- `.text` size against the budget (3DS_PLAN.md section 2). With 93.8 MB of
  heap after the code is loaded, the code size decides whether the 32 MB
  guest RAM + caches fit.
