# PSP2i New 3DS port: progress

Plan: [3DS_PLAN.md](3DS_PLAN.md). Branch `3ds_port`, worktree
`D:\work\PSP2I_Recomp_3DS`. Skills: `psp2i-3ds-build`, `psp2i-3ds-test`.
The user's console: **New 3DS XL, Luma3DS**.

## Handoff (2026-10-25): where to pick up

**State:** T0-T2 done. T3: the user played into the game in Azahar
(2026-10-25) and saw no drawing errors, at **1.8-3 fps**. The PC save
loads (section "Saves"). **T4 (speed) has started**: the host reports where
each second goes (section "T4"), and the per-frame target thrashing in the
field is fixed in code but **not yet checked in a field**.

**Next step:** in Azahar the copies between targets and VRAM are gone
(section "T4") and the game is CPU-bound at 2 fps: the game's code plus the
GE's vertex work. Measure that on the console before choosing CPU work (the
ISO isn't on its SD: ask the user). The log's `time:` lines and the `c3d:`
per-cause counts are in `%APPDATA%\Azahar\sdmc\psp2i\psp2i_log.txt` (on
the console `sdmc:/psp2i/psp2i_log.txt`).

Testing loop:

1. Build: `MSYSTEM=MSYS /d/msys64/usr/bin/bash.exe -l
   /d/work/PSP2I_Recomp_3DS/port/3ds/build.sh Release psp2i_3dsx` (and
   `psp2i_cia`; `port/gen` is generated; only changed files rebuild).
2. The user runs the installed title: install with `azahar.exe -i
   port\3ds\build\psp2i.cia` (title `000400000F522100`; `0F522000` is the
   check app). `port/3ds/tools/azrun.ps1` runs the `.3dsx` (96 MB in
   Azahar) and screenshots it. The user allowed restarting Azahar for this.
3. Scripted input: `sdmc:/psp2i/input`, lines `<second> <BUTTON[+BUTTON]>
   <duration>` or `<second> STICK <x> <y> <duration>` (`script_load`). The
   7-press script on Azahar's SD sometimes gets past the title and
   sometimes not (two runs, 2026-10-25): it's timing, not checked further.

**Open items:** music (no ATRAC3plus decoder: T4), overlapping targets
go through VRAM (gl_ge.c's GPU `rt_xfer` not ported), a save written by the
3DS (not seen yet), nothing measured on
the console since T1 (its SD has the fonts, the DLC and an old build, not
the ISO; the user wants Azahar until the game runs better).

**Don't touch** `D:\work\PSP2I_Recomp_VR`: another session's uncommitted VR
work lives there. Only read it or run its PC build.

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
  so it read as garbage here. Fixed 2026-10-25 (section "Saves" below).
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

## Saves (2026-10-25)

**Cause:** the 3DS host set no savedata crypto provider, and `savecrypt.c`
only had Windows' AES (BCrypt). The PC writes saves encrypted (mode 5, as
a PSP does), so the 3DS passed them to the game still encrypted, and would
have written its own saves plain.

**Fix:** `savecrypt.c` has its own AES-128 where BCrypt isn't there (S-box
built from its definition at first use, the 12 KIRK key slots expanded
once); the IV seed comes from the clock there. `3ds_main.c` sets the
provider as `main.c` does, and the 3DS build links `savecrypt.c`. The
Windows build is unchanged (BCrypt); `SAVECRYPT_OWN_AES` selects our AES on
Windows too, for the test below.

**Checked:**

- On the PC, our AES against BCrypt (a test program built from
  `savecrypt.c` twice): encrypt with one, decrypt with the other, both
  ways, modes 1, 3 and 5, lengths 1 to 301,352 bytes, all equal; the
  PARAM.SFO hashes are byte for byte the same.
- In Azahar: the PC's `SAVEDATA.BIN` loads: `savedata ... decrypted (mode
  5, 301368 -> 301352 bytes)`, no hash mismatch, and the game reaches the
  title screen.
- Not checked: a save written by the 3DS (the title screen didn't take
  the scripted presses), and nothing on the console.

## T4: speed (2026-10-25, started)

### Measuring

Once a second the host writes a `time:` line to the log and lines 2-4 of
the bottom screen: milliseconds per second in each part, each counted once.

| Part | What |
|---|---|
| game | the game's code and the HLE: what's left |
| ge | the runtime's vertex work for the GE (`psp_gpu_prim` minus the backend; psprecomp b0a0383 `psp_gpu_set_clock`) |
| draw | `c3d_ge.c` turning draws into citro3d calls |
| wait | waiting for the GPU or its transfer engine (`C3D_FrameBegin/End`, the sync transfers) |
| readback / upload | converting targets to and from emulated VRAM on the CPU |
| texture | decoding textures |
| present | the top screen's quad |
| idle | no PSP thread runnable (`psp_sched_idle_us`, waiting for the vblank) |

Also per frame: draws, evictions, read-backs, uploads, finishes. Every
10 s, `c3d_ge_report` gives each target's size and its read-backs and
uploads **by cause**: read-backs `evict`, `rows` (it grew), `texture` (a
draw samples VRAM it covers), `overlap` (another target over the same
bytes), `sync` (a GE block transfer or software fallback), `cpu` (the game
read its VRAM); uploads `new`, `rows`, `changed` (VRAM changed under it).

### Found

- **Before (the user's run, old build):** in game, 4 targets of 256x128
  (VRAM+0x000000, 0x022000, 0x044000, 0x066000, format 3: passes drawn
  every frame) were each made and evicted **5,119 times**. Every target was
  512 rows tall (512 KB at 256 wide), VRAM had 137 KB free, and each
  eviction cost a read-back, a GPU wait and an upload: 31,000 read-backs
  and 40,800 uploads in the run.
- **Fix:** a target is as tall as its rows (the next power of two, 64 at
  least): 256x128 now takes 128 KB. A target asked for more rows than its
  texture has is made again from VRAM. **Checked in game (the user's run,
  2026-10-25): 1 eviction in the whole run instead of 5,119 per target.**
  The frame rate didn't change in Azahar (still 2 fps): the time is
  elsewhere (next point).
- **A scene after the title (the `.3dsx`, scripted, with the fix):** 587
  draws per frame at 2 fps, **0 evictions**. In ms per second: ge
  450-500, game 240-330, upload 70-90, readback 50-130, draw 50-60,
  texture 15-130, wait about 10. So the runtime's vertex work is the
  biggest part there, then the game. (This scene's lines came from the
  build without `idle`; the game part may include some idle time.)
- **Loading screen:** 1 fps with 990 of 1000 ms in the game's code.
- **In game, the user's run with the fix (town and field, 350-500 draws
  per frame, 2 fps), ms per second:**

  | game | ge | upload | readback | draw | texture | wait | present | idle |
  |---|---|---|---|---|---|---|---|---|
  | 400-470 | 250-320 | 90-115 | 55-70 | 55-60 | 15 | 13 | 2 | 0-10 |

  So about 750 ms of every second is CPU work in the game's code and the
  runtime's vertex work: about 375 ms a frame, where 30 fps needs 33.
  Target traffic is 4.5 read-backs and 4 uploads a frame, **all of it
  `overlap`**: the 256-wide passes at VRAM+0x000000 / 0x022000 / 0x044000 /
  0x066000 sit inside the two displayed buffers' bytes, so every frame
  they go through emulated VRAM (gl_ge.c's GPU `rt_xfer` would copy them on
  the GPU instead: about 160 ms/s here). The scene (VRAM+0x0CC000) has 564
  `cpu` read-backs in the run: the game reads it now and then.
- **Overlapping targets on the GPU (`rt_take`, `rt_resolve`, `rt_xfer` in
  `c3d_ge.c`), checked by the user in town (2026-10-25: "town looks right,
  no flicker"):** same format and stride: the rows are drawn from one
  target into the other on the GPU; 16-bit over 32-bit (the bloom passes in
  the displayed buffers' memory): not reinterpreted, as in PPSSPP, and each
  target keeps its own pixels. A first version dropped the bloom target's
  rows when the displayed buffer was drawn over them, so the bloom was then
  sampled from stale VRAM: **a flickering yellow glow in town**. Before a
  target is sampled, it now takes the newer same-format rows drawn over it
  (gl_ge.c's `rt_resolve_copy`), and readbacks of overlapping targets run in
  drawing order. `sdmc:/psp2i/xfer_vram` brings back the VRAM path.
- **After, in town (the user's run):** 0 uploads and 0-4 read-backs a frame
  (only the game reading the scene, `cpu`), 1,472 GPU transfers and 5,167
  dropped overlaps in the run. ms per second: game 470-630, ge 300-470,
  draw 45-65, wait 12, readback 0-75, present 3, idle 5. **Still 2 fps in
  Azahar**: the copies are gone, and what's left is CPU work (game + ge,
  about 900 ms of every second).
- **Azahar's CPU speed is not the console's.** Azahar emulates the ARM11
  with its own cycle count and runs this build at "Speed 90-100%", but how
  that compares with the console's 804 MHz with L2 isn't known. The CPU
  part (game + ge) has to be measured on the console before deciding
  between CPU work (an `-O2` build of the hot code, the GE's vertex work on
  core 2) and the rest.
- Azahar measures correctness, not speed. These numbers say where the
  time goes in Azahar; only the console says how fast it is.
