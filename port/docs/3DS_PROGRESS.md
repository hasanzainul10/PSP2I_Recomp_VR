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

## T1: compile and fit (2026-10-25)

Done:

- `port/gen` regenerated in the 3DS worktree (same EBOOT, hooks, fixes and
  allegrexrecomp as the main checkout: byte-identical, 88,317 functions).
- `split_gen.py` (the Android one) cuts it into **256 parts** (~850 KB of C
  each). One part: 3.7 s and ~110 KB of code at `-Os`; all 256 with 12 jobs
  take about 1.5 minutes on this PC.
- The runtime builds for ARM11 with `PSPRECOMP_3DS` (psprecomp branch `3ds`,
  4e0f86f): ARM32 fibers in `threadman.c` (r3-r11, lr, d8-d15), 1 MB fiber
  stacks (`PSPRECOMP_FIBER_STACK` to change), `SOCU_GetIPInfo` instead of
  getifaddrs, `<arpa/inet.h>` in `net_sock.c`. The IPv6 types the ad hoc
  code uses come from `port/3ds/net_compat.h`, force-included (soc:U is IPv4
  only: those sockets just fail to open; `FIONREAD` reads 0). Online, T8.
- `port/host/3ds_main.c`: the T1 host. It links the whole game (through
  `psp_recomp_register`), then measures the heap after the code, after the
  guest RAM (`psp_mem_init`) and after the dispatch tables. It doesn't start
  the game (T2). Report: `sdmc:/psp2i/report_game.txt`.

**Size (`-Os`, no LTO, devkitARM GCC 16.1):**

| | MB |
|---|---|
| `.text`, whole ELF | **27.99** |
| of which the game's 256 parts | 27.91 |
| of which the runtime | 0.25 |
| `.bss` linked in by the T1 host | 0.81 |
| runtime `.bss` not yet linked (pulled in by the HLE in T2) | ~10.9, nearly all `gpu.c`'s `g_vbuf` / `g_lbuf` / `g_ebuf` (3.5 MB each) |
| `.3dsx` / `.cia` file | 30.4 / 14.7 |

That's 28 MB of code, against the plan's 40-60 MB estimate and P3P3DS's 44 MB.

In **Azahar** (`.3dsx` started directly, 96 MB region): heap after the code
64,668 KiB; the guest RAM takes 34,832 KiB; `psp_recomp_register` 72 ms and
3,072 KiB of dispatch tables. Azahar doesn't enforce the heap's end (the
largest-block test reads the whole heap even after 38 MB were taken).

Budget for T2, from these numbers, in the 124 MB region (heap ≈ 93.8 MB
for the check app, minus 28.9 MB of code and `.bss` ≈ **65 MB**):

| | MB |
|---|---|
| guest RAM + VRAM | 34 |
| dispatch tables | 3 |
| `gpu.c` vertex buffers | 10.5 (to shrink in T3/T4: the 3DS draws far fewer vertices per frame than they hold) |
| PSP thread stacks (1 MB each, PSP2i ~10-20 threads) | 10-20 |
| **left for file cache, audio, the rest** | **~0-8**, more once the vertex buffers shrink |
| linear memory (textures, GPU buffers), separate | 32 |

Not checked yet, **on the console**: the fit check (heap after code, the
largest block after the guest RAM and the tables).

## Next: T2 (boot)

- Shrink the fixed buffers first if the console confirms the tight budget:
  the vertex buffers, thread stacks sized from what PSP2i creates.
- Files from the SD (`sdmc:/psp2i/`: the ISO, fonts, saves), the pad,
  the scheduler, the software rasterizer on the top screen.
