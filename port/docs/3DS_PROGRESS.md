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
3,072 KiB of dispatch tables.

On the **console** (New 3DS XL, `.3dsx` from the Homebrew Launcher,
2026-10-25):

| | KiB |
|---|---|
| APPLICATION region | 126,976 |
| heap after the code (`__ctru_heap_size`) | **64,676** |
| used after the guest RAM + VRAM (`psp_mem_init`) | 34,849 |
| used after the dispatch tables (`psp_recomp_register`, 92 ms) | 37,921 |
| **heap left** | **26,755** |
| linear memory free | 31,193 |

The first build's "largest block" lines read the whole heap (64,612 KiB)
in both places: GCC had removed the probe's malloc+free pair and assumed
success. Fixed (the pointer goes through a volatile, and the ELF now calls
malloc there); heap minus used is the real figure.

Budget for T2, from these numbers, in the 124 MB region (heap after the
code **63.2 MB**, measured):

| | MB |
|---|---|
| guest RAM + VRAM | 34 |
| dispatch tables | 3 |
| `gpu.c` vertex buffers | 10.5 (to shrink in T3/T4: the 3DS draws far fewer vertices per frame than they hold) |
| PSP thread stacks (1 MB each, PSP2i ~10-20 threads) | 10-20 |
| **left for file cache, audio, the rest** | **~6-16** (26.1 measured before the vertex buffers and stacks), more once the vertex buffers shrink |
| linear memory (textures, GPU buffers), separate | 32 (libctru's default split; `__ctru_linear_heap_size` can move memory between the two) |

**T1 verdict: the game fits.** Its code, the PSP's memory and the dispatch
tables load on the console with 26 MB of heap and 31 MB of linear memory
to spare. T2 has to keep the runtime's fixed buffers small.

## T2: boot (2026-10-25)

**Checked in Azahar by the user:** the game boots from the SD card, shows
the translation disclaimer, offers a save file (can't make one: see
below), plays the opening movie and reaches the **title screen**, at about
**0.1 fps**. Not run on the console: the decision (with the user) is to
test in Azahar until the game runs better. The fonts, the DLC and a game
build are on the console's SD, the ISO is not.

Done:

- `port/host/3ds_main.c`, the T2 host: the ISO from `sdmc:/psp2i/game.iso`
  (or any `*.iso` there), `GameData/` beside it (fonts, memory stick), the
  ELF loaded as in `sdl_main.c`, the HLE, the scheduler. Each vblank: the
  pad (A Circle, B Cross, Y Square, X Triangle, L/ZL L, R/ZR R, Circle Pad
  the stick, C-stick the camera through `camera.c`) and the PSP's
  framebuffer from emulated VRAM to the top screen (480x272 picked down to
  400x240). The bottom screen: the log's lines and the fps. Log
  `sdmc:/psp2i/psp2i_log.txt`, report `report_game.txt`.
- psprecomp `3ds` 690be56: `gpu.c`'s three 3.5 MB vertex buffers grow on
  demand on the 3DS; the game's `.bss` is **1.23 MB** with the whole HLE
  (it would be ~11.7 MB). `.text` 28.25 MB.
- Host stacks per thread, from what the thread asks the PSP for: 32 KB or
  more gets 4 MB, the rest 512 KB. Measured at the title screen:

  | thread | PSP stack | host stack peak |
  |---|---|---|
  | user_main | 48 KB | 1,135 KB |
  | SOUND_THREAD | 12 KB | 8 KB |
  | fio_th | 4 KB | 3 KB |

  5 MB of stacks in all. The PSP's stack size doesn't predict the host's
  (user_main 24x): with 1 MB for everyone, user_main overflowed into the
  scheduler's stack and the next switch jumped to 0; with 8 MB for
  everyone, the heap ran out before `fio_th` existed and the game waited at
  the disclaimer for files forever. Each stack has a 4 KB guard checked at
  every switch: an overflow now stops with the thread's name.

Found, not fixed:

- **Saves:** the PC's save is encrypted (`savecrypt.c`, Windows AES only),
  so it reads as garbage here and the game can't save over it. A portable
  AES in `savecrypt.c` (T7) makes saves move between PC, PSP and 3DS. The
  console copy leaves that save out.
- **Music:** ATRAC3plus has no decoder here ("playback is silent"), T4.
- Firmware calls 0xA1336091 and 0x8218B4DD are unimplemented (also on PC).

## T3: citro3d renderer (2026-10-25, in progress)

**Checked in Azahar (screenshots of its window):** the translation
disclaimer and the save dialog draw on the PICA200 exactly as on PC (text,
layout, orientation), at **28-30 fps** where the software rasterizer gave
0.1. The dialog waits for a button; scenes after it are not checked yet.
Not run on the console.

`port/host/c3d_ge.c` (+ `c3d_ge.h`, `c3d_ge.v.pica`): gl_ge.c's design on
citro3d. Render targets per (VRAM offset, stride, format), seeded from
VRAM, read back only when the CPU touches them; textures decoded by the
runtime and cached by `tex_key` + a per-frame hash; TEV for the texture
functions; alpha test, depth test, blending mapped (the GE's and the PICA's
test functions share their numbering); one quad presents the displayed
buffer, 480x272 filtered to 400x240. `sdmc:/psp2i/software` switches back
to the software rasterizer.

What the PICA200 / citro3d / Azahar demanded (each found in Azahar, in the
order it bit; all written up in c3d_ge.c's comments):

- **Render targets only in VRAM** (6 MB): citro3d refuses others, so the
  first build drew nothing. Targets take the narrowest power-of-two width
  for their stride; depth is 16-bit like the PSP's (0.5 MB, shared).
- **PSP2i uses five targets** at the title (the two displayed buffers, the
  scene at 0x0CC000, two 256-wide ones overlapping them); four slots evicted
  one every frame and overlapping targets swapped garbage through VRAM.
  Slots are now limited by VRAM only.
- **No target deletion inside a citro3d frame** (it panics): the frame is
  ended first.
- **The CPU never touches a target's VRAM itself.** Azahar renders on the
  PC's GPU and writes the result into the 3DS's memory only for GX display
  transfers; CPU reads of a target returned what the CPU had written at its
  creation. Read-backs go through a display transfer into a linear staging
  buffer, uploads through a GX texture copy, the depth clear through a GX
  fill.
- **Rows:** the sampler and rendering agree with each other, and both put
  PSP row y at a texture's memory row height-1-y as the CPU writes it.
  Decoded textures stored row 0 first came out upside down, and the text's
  UVs read the empty bottom of the glyph atlas: no text anywhere. All CPU
  writes go through `mrow()`; the display transfer's own row order is
  measured at start-up (`staging_selftest`: reversed in Azahar).
- **Waiting for the GPU before CPU access**, between frames too (present
  submits without waiting).

Tools added on the way (sdmc:/psp2i/ files the host reads at start-up):
`dumpframes` (seconds: every draw of that frame to `dump_<s>.txt`),
`shots` (seconds: the displayed buffer to `shot_<s>.ppm`, via the GPU
read-back), the per-target counters in the `c3d:` stats line every 10 s.

Next in T3: play past the dialog (needs input) and check the title, the
lobby and a field against the PC build; overlapping-target transfers on
the GPU (gl_ge.c's rt_xfer) instead of through VRAM; first numbers on the
console.
