# PSP2i New 3DS port: progress

Plan: [3DS_PLAN.md](3DS_PLAN.md). Branch `3ds_port`, worktree
`D:\work\PSP2I_Recomp_3DS`. Skills: `psp2i-3ds-build`, `psp2i-3ds-test`.
The user's console: **New 3DS XL, Luma3DS**.

## Handoff (2026-10-26): the game runs on the console

**State: the citro3d build boots and plays on the user's New 3DS XL** (.cia,
804 MHz + L2). The user's report: ~30 fps in dialogue with 2D portraits,
~6 fps on the field. Log on the field (console): 6-7 frames a second, ms/s
**game 490-550, ge 160-200, wait 150-200, draw 70-85**, 250-340 draws a
frame; `depth+texture` 70 us a draw. Before this, every console run of the
citro3d build froze the whole console (Rosalina too) at the game's first
draw; the software renderer worked. Azahar never showed it.

**The cause: our vertex shader wrote only part of an output register**
(`mul outuv.xy, ...`: o1.zw never written, though the output map ignores
them). The PICA200 locks up on that; Azahar doesn't model it. Fixed in
`port/host/c3d_ge.v.pica`: every output register is written whole, from
temporaries. Found with a GPU self-test (`gpu_selftest` in c3d_ge.c, runs
when `sdmc:/psp2i/diag` exists): draws one at a time, each waited for and
traced to `sdmc:/psp2i/diag.txt` (opened, appended and closed per line, so
it survives a hard freeze), with each step's GPU command list dumped.
Shader variants `port/host/c3d_test*.v.pica` built up to ours a feature at a
time; every variant with a partial output write hung, every one without
passed (2a2 `mul outuv.xy` hung, 2a4 `mul outuv` passed).

Wrong turns, so they aren't repeated: VRAM banks (depth in the same bank as
colour draws fine on the console: the depth-bank copies in `depth_get` are
not needed and cost time), the vertex buffer's physical address, clip-edge
vertex data, the shbin's layout and operand descriptor count. One trap in
the self-test itself: `C3D_BindProgram` ignores the program already bound,
so variants sharing one `shaderProgram_s` all ran the first one.

**Depth-bank copies taken out (2026-10-26, console):** `depth+texture` went
from 70 to 6 us a draw and GPU finishes from 3 a frame to 0; `wait` from
150-200 to 115-155 ms/s. Field still ~6 fps (340-730 draws a frame in that
run): **game code is now 600-690 ms/s**, the bottleneck on the console.

**O3 on by default (2026-10-26, console):** `PSP2I_HOT_LOCALS` (hot
functions keep the MIPS registers in C locals) measured on the field against
the run above, compared at the same draws per frame: game ms per frame
300-349 draws 106 -> 95, 350-399 110 -> 97, 400-449 123 -> 115, 450-499
127 -> 124 (250-299: 76 -> 82; 6-41 samples a bucket, one run each). About
8% less game time; fps 5-7 on the field, so still ~100 ms of game code a
frame. Cost: 232 KiB of code (heap after the code 62132 -> 61900 KiB); peak
heap unchanged (45 MB). PC and Azahar showed no gain; only the console does.

**On-console profiler (2026-10-26, bff2f49):** `-DPSP2I_PROFILE=ON`
(own build dir, `build_prof`) samples the current game function at 1 kHz
from core 2 into `sdmc:/psp2i/profile.txt` (top 40 per 10 s window, with
frames, game ms/s and draws/frame to pick the field windows). The code is
over 32 MiB then, and makerom's code compression corrupts code that big
(dies before `main`; Azahar: `unknown svc` at 0x00100004), so profiling
.cias go uncompressed (`APP_COMPRESS`, psp2i.rsf). Normal builds are ~31.5
MB, close to that edge. First field result: 55.6% in `sceGeListEnQueue`,
7.1% sceSasCore (16% in another run), ~30% game code spread thin (no
function over 1.8%). The `time:` line explained only ~37% of the GE share.

**The profile's attribution, corrected (2026-10-26):** three ways that
run charged time to the wrong place: the caller's code after a firmware call
went to the import (imports have no `PSP_SP_CALL` after them); the import
marker stayed set through `psp_sched_after_hle` (vblank interrupt handlers,
posted calls, the host's `on_vblank`, thread switches); idle had no mark.
Now the runtime marks its own work under `PSPRECOMP_PROF`
(psprecomp/src/hle/prof.h: GE list commands, GE prims, scheduler events,
idle) and the thunks put the caller back. Every build's log also gets a
`gelist:` line (lists = prims + callbacks + commands, ms/s): the non-PRIM
commands were counted in "game" until now.

**The corrected profile (console, field, 350-560 draws):** GE prims 31%,
present 12.4% (it ran at every vblank, ~2 ms each, while the game draws ~6
frames a second), sceSasCore 7-9%, GE list commands 5% (~50 ms/s, the
`gelist:` line agrees), idle 3%, scheduler work and thread switches
negligible, game code ~30% with no function over 1.4%. **Present only new
frames (0b7f64d):** a present when the game flipped, and every 6th vblank
at least (`sdmc:/psp2i/present_every_vblank`: the old way). A/B, same build:
fps 6.09 -> 6.43 / 5.76 -> 5.82 / 4.94 -> 5.09 at 350/400/450 draws, game
ms per frame 107 -> 87 / 112 -> 91 / 123 -> 107. "wait" fell from ~130 to
~5 ms/s but "ge" (prims minus the backend's timed parts) rose ~110 -> ~300:
the GPU wait moved somewhere untimed. The mixer now goes through host
pointers, bit-exact (psprecomp 6f5fd3d, `tests/test_sas.c`).

**The draw path, split (2b6ae54, console, field, 400-499 draws, 5.8
fps):** vertices (the CPU's transform) ~42 ms a frame, the backend
(citro3d draws and waits) ~20, the triangles' assembly (CPU clip, cull,
collect) ~12, setup and state ~1.5, nothing else (the log's `prims:` line).

**The PICA's vertex shader for the common draws (f15f9a6, psprecomp
23f7192):** unskinned, unlit triangles, strips and fans go to
`c3d_draw_raw` with the game's vertex bytes; `c3d_ge_raw.v.pica` applies one
matrix per draw and the PICA clips and culls. Console, field: 350-399 draws
7.8 -> 10.4 fps, 400-449 5.7 -> 8.9, 450-499 5.8 -> 7.0 (33 field seconds);
picture checked on the console. **Hardware lesson:** an attribute buffer per
attribute over the game's bytes (1- and 2-byte offsets, stride larger than
the attribute) drew in Azahar and hung the console's GPU on the first draw;
packed as one buffer of whole 4-byte words with the stride their sum, it
runs. The diag self-test (steps 6a-6g) draws each format through it.
Switches: `sdmc:/psp2i/vs_cpu` (the CPU's transform), `cull_flip`.

**Skinning and lighting on the PICA (28b8ff9, psprecomp 62b66e3):** one
vertex program for every draw (booleans choose the screen path, raw, skin,
light), the GE's lighting as `psp_ge_light_fast` term for term. Console,
field: 350-399 draws 10.4 -> 11.4 fps, 400-449 8.9 -> 11.0, 500-549 5.0 ->
8.0 (60 field seconds, few above 400); the runtime's vertex loop ~95 -> ~2
ms/s; characters checked on the console. Switches: `vs_noskin`,
`vs_nolight`.

**O4b, GE lists on core 2 (branch claude/3ds-ge-core2-188db8, psprecomp
12e82bc, port 97bd2bf; built on f3fb601, without the PICA skinning).**
`sceGeListEnQueue` / `UpdateStallAddr` queue jobs (the list and the stall
address given) that a thread on core 2 runs in order (`psp_ge_async_start`,
ge.c "the worker"); ListSync, DrawSync, DeQueue, the callback setters and
the game's VRAM accesses (the backend's hook, wrapped) wait for it; finish /
signal callbacks are queued and called on the game's thread at its next
preemption point; the present and the reports are posted to it
(`psp_ge_post`); an APT hook drains it before HOME / sleep / exit.
`sdmc:/psp2i/ge_sync` keeps lists in the game's calls. The log's `core2:`
line: the worker's busy ms/s and the game's waits by reason. Console, same
build, same route, 135 s each (A/B by the switch), fps by draws a frame:

| draws/f | in the game's calls | on core 2 |
|---|---|---|
| 350-399 | 11.7 | 14.3 |
| 400-449 | 8.6 | 11.8 |
| 450-549 | 4.7-5.5 | 8.9-9.2 |
| 650-799 | 5.3-5.7 | 8.7-10.5 |
| 1,200-2,450 | 3.0-4.0 | 6.0-9.0 |

The game waits for core 2 once a frame, at its DrawSync (0-300 ms/s);
callbacks 40-60 a second, none dropped; no hang or wrong picture reported
in either run. **Core 2 is now the bottleneck** above ~450 draws: busy
850-1,050 ms/s while the game's thread idles or waits. So the GE's own work
(list commands, the remaining CPU vertex work, citro3d submission) is what
to cut next; the PICA skinning (28b8ff9) lowers exactly that, so merge both
and measure again. Not checked: in Azahar three of the four mode-select
labels drew as dark bars; whether the synchronous build does the same
wasn't compared.

**Merged (652adc0, psprecomp 4d0e936), console, field (117 s), fps by
draws a frame:** 300-349 14.8, 350-399 14.3, 400-449 13.1, 450-499 11.8,
500-549 12.0, 550-599 11.7 (skinning alone: 11.4 / 11.0 / - / 8.0 / 7.5;
core 2 alone: 14.3 / 11.8 / 8.9-9.2 at 450-549). Picture checked by the
user. Core 2 is busy ~460-520 ms/s and the game waits for it ~20-30 ms/s:
**the game's own thread is the limit now** (~55-75 ms a frame).

**The game's thread, profiled on the merged build (console, field, 8
windows at 316-1,446 draws):** game function 08D8B864 10.9%, 08D8BA7C 6.1%,
08D921F8 3.7%, 08D920A0 3.2% (~24% together, all hot-listed), idle 7.9%,
sceGeDrawSync 4.9% (waiting for core 2), the sound mixer 0.9%, the rest
spread thin. All four are the game's own VFPU culling and vertex
transforms: 08D8B864 loads 16-bit positions (lh), vtfm4, compares them with
the view volume (vsge / vslt, vmin / vmax out-codes) and writes indices;
08D8BA7C tests bounding boxes (vscl, vmin / vmax, vsge / vslt); 08D920A0
transforms (vtfm4 x 8, called from 08D92F50, 08D93464, 08D93560,
08D935D4); 08D921F8 is another such test. In the generated C every VFPU
operation reads and writes `psp_cpu.v[]` in memory behind a run-time
prefix check (`psp_vfpu_pfx_any`), and mtv / mfv / vi2f / vconv are calls.

**Faster VFPU code (2026-10-26; builds, PC and Azahar checked, console A/B
pending).** Three changes:

1. *The prefix check, proven away at recompile time* (allegrexrecomp
   `emit.c` `check_pfx_site`): every prefix that isn't part of an inlined
   group must be followed, in straight-line code, by an operation the
   runtime implements with `consume()`. For PSP2i that holds for every
   prefix (one more sits before an undecodable word: data). The guard is now
   `PSP_VFPU_NOPFX` from the generated `gen/recomp_vfpu.h`: `1` when proven
   and built with `-DPSPRECOMP_VFPU_UNGUARDED` (the 3DS's `psp2i_game`),
   else `!psp_vfpu_pfx_any` as before (PC, Android, macOS:
   `PSP2I_VFPU_NOINLINE` still works there).
2. *mtv / mfv (vector registers), vi2f, vconv, vcst and vmmov inline*, the
   runtime's arithmetic exactly; a prefix right before one keeps the
   runtime call. None of their 1,365 calls in the game remain outside dead
   fallbacks.
3. *VFPU registers in C locals in the hot functions* (`reg_locals.py`
   vregs, `PSP2I_HOT_VLOCALS`, on): `psp_cpu.v[N]` becomes `xv_N`, written
   back and reloaded around game / HLE calls and the VFPU runtime calls
   left (dead fallbacks after 1).

Checks: `port/3ds/tools/vfpu_diff.py` pairs each of the 1,968 distinct VFPU
statements of the old and new code, runs the old one forced through the
runtime (prefixes and all) and the new one inline on 1,000 random register
files: 0 mismatches (NaN payloads differ in ~1% of runs of the arithmetic
that was already inline: x64 propagates whichever NaN operand the compiler
put first; the 155 moves and bit operations are compared bit for bit); a
planted bug is caught. psprecomp's 15 tests pass. PC headless town replay
(new code, unguarded): town drawn, no VFPU traps; the replay isn't
frame-deterministic (two runs of the old exe differ as much as old vs new),
so that's a smoke test, not a comparison. Azahar: boots to the lobby
cutscene, drawn right. `.text` 29.48 MB (1+2), 29.57 MB (1-3).
`port/3ds/build.sh` takes `-Dopt=value` (kept in the cache):
`-DPSP2I_HOT_VLOCALS=OFF` rebuilds only the four hot files.

**Next:** the console A/B on the field: `psp2i_t4_vfpu12.cia` (1+2) and
`psp2i_t4_vfpu.cia` (1-3) against the merged build's numbers above, then
a profile. The previous Next: the game code (more hot
functions, O3 on more of them), the sound mixer, the GE commands it still
waits for; then the console baseline route (spawn stand-still 30 s, busy
spots). The user's Azahar save is on the console's SD
(`GameData/ms/PSP/SAVEDATA/NPJH50332`, `NPJH50332SC`); the save the console
made on 2026-10-26 is backed up off the console.

## Handoff (2026-10-25): where to pick up

**State (2026-10-25, end of session):** T0-T3 done in Azahar (the user
played into town, no drawing errors; saves load). **T4 (speed):** town went
from 2 to 5.4 fps in Azahar (target thrashing fixed, overlapping targets on
the GPU, runtime at -O2; section "T4"). The work in progress is the next
step, hot game functions at -O2 with inline memory access: it reached
**6-7 fps in town and then PANICked after about 80 s**. Fixed (below);
**the hot list is on by default** since then.

**The PANIC is found and fixed (2026-10-25, second session), confirmed in
Azahar: the user played the hot build into town (2+ minutes, 78 s of them
at 300+ draws a frame); `user_main` peaked at 4 KB in all 15 reports, no
PANIC.** It was not the hot build: `user_main`'s host
stack overflowed its 4 MB. The game's inflate (`0x08DE0C40`, ADDRESS_MAP.md
"The data decoder") falls from `0x08DE0E10` into a label inside another
body once per decoded byte, and the recompiler emitted that as a dispatch
call, so a loaded file nested one call per byte: on PC in town, 177,000
calls and 16.6 MB, still growing after 150 s (a 64 MB reserve hides it
there). The hot build ran faster, so it loaded the file that crossed 4 MB
sooner; the -Os build would have too. The overflow wrote below the stack
between guard checks, so the guard's message never reached the log.
Found with `port/3ds/tools/gdepth.c` (attach to the PC build: the fiber's
depth each second, and a full stack walk at each new record). Fixed in
allegrexrecomp (psprecomp 8d67e25, `fall_target` / `scc_fall`): such a fall
now stays inside the tail-call cycle trampoline. Checked: PC town replay,
deepest 22 KB in 150 s, town draws right (`town.rec` frame at vblank 3600);
Azahar, the hot build: `user_main` peaks at 4 KB through the title and the
save load (was 1,136 KB there), and in town as above. `.text` 29.68 MB
(+0.39: more cycle wrappers). Town in that run: 6 fps at first, 3-4 later
with `ge` at 600+ ms/s (the GE vertex work, not the game's code), 4.1 on
average. The hot list is now on by default (`port/3ds/CMakeLists.txt`).
**Next:** measure on the console, then the GE vertex work on core 2.

**Was: the hot-function build's PANIC** (kept for its details):

- What it is: `split_gen.py` takes a hot list (`port/3ds/hot_functions.txt`,
  1,076 addresses) and moves those functions to `gen_split/recomp_hot_*.c`.
  CMake builds those at -O2 with `PSPRECOMP_INLINE_MEM` (psprecomp `mem.h`:
  main-RAM loads and stores inline, the rest through `psp_read32` & co.;
  `psp_mem_fast_rd/_wr`, kept by `mem.c`'s `fast_update`). `.text` 29.29 MB
  (+0.9). Inline memory in all the game's code: 52 MB, doesn't fit.
- On by default (`PSP2I_HOT_LIST` = `port/3ds/hot_functions.txt`); configure
  with `-DPSP2I_HOT_LIST=` to build all of the game's code at -Os. It's a
  cache variable: an existing build directory keeps what it was configured
  with, so check `PSP2I_HOT_LIST` in its CMakeCache.txt.
- The crash: the `.cia` in Azahar, town, ~81 s: Azahar's log says
  `Emulated program broke execution! Reason: PANIC` (svcBreak). The last
  `time:` lines before it: 982 and then 2,106 draws a frame with 73 and 19
  read-backs a frame (normal town: 330 draws, 0 read-backs). The game's log
  is saved nowhere in the repo; rerun to reproduce. Not diagnosed. Suspects,
  in order: (1) an inline store that should have gone through the slow path
  (e.g. a write the runtime must see: VRAM is excluded by the range check,
  but check the GE list / `psp_gpu_cpu_sync` assumptions and anything
  hooked by address in `hooks.txt`); (2) -O2 exposing undefined behaviour in
  generated code (strict aliasing: try `-fno-strict-aliasing` on the hot
  files, then -O2 without `PSPRECOMP_INLINE_MEM`, to split the two causes);
  (3) citro3d running out of something at 2,000 draws a frame (its panic
  would also be svcBreak). Get the PC from Azahar's log
  (`%APPDATA%\Azahar\log\azahar_log.txt`, "Exception") and map it with
  `arm-none-eabi-addr2line -f -e port/3ds/build/psp2i.elf`.
- Profiling tools: `port/3ds/tools/gsample.c` (attach to the PC build,
  charge each sample to the innermost game function, memory helpers
  included; build with MSVC + dbghelp.lib winmm.lib) and
  `port/3ds/tools/town.rec` (the user's input from the title into town, for
  `psp2i.exe --replay-input town.rec --headless --seconds 70`; town is
  vblank ~1800-3900). The PC findings: game time is spread over ~1,000
  functions (the biggest 0.7%); the cost is the call per guest load/store,
  which makes the compiler spill and reload the MIPS registers kept in
  `psp_cpu` (on the 3DS "game" was 3.3x the GE time; on PC 1:1).
- After that: measure on the console (the ISO isn't on its SD; ask the
  user), then the GE vertex work on core 2.

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
- **The runtime at -O2 (it was -Os, like the game's code; `PSP2I_RUNTIME_OPT`
  in `port/3ds/CMakeLists.txt`). The user's town runs before and after
  (2026-10-25), the seconds with 330-480 draws a frame:**

  | | frames a second | game ms/frame | ge ms/frame |
  |---|---|---|---|
  | -Os (80 s) | 3.2 (mostly 2-3) | 180 | 96 |
  | -O2 (37 s) | **5.4 (all 5-6)** | 121 | 37 |

  The GE's vertex work is 2.6 times faster, and the game's part too (it
  calls the runtime for the HLE and memory). `.text` grew by 70 KB (28.30 to
  28.37 MB). Next for the game's own code: find its hot functions (a
  profile of the PC build runs the same generated code) and build those at
  -O2; the 200 MB in all has to stay -Os to fit.
- **Why some town views cost 5x the GE time (the user's run, hot build,
  2026-10-26).** The log's `vtx:` line (psprecomp
  `psp_gpu_vkinds_take`: the CPU vertex loop timed per kind of draw) over
  290 s, 184 of them in town. The slow views draw **5x the vertices**, at
  the same cost per vertex; it isn't a bug or a fallback:

  | views | s | fps | game ms/f | ge ms/f | draw ms/f | static unlit v/f | skinned lit v/f |
  |---|---|---|---|---|---|---|---|
  | normal (ge < 50 ms/f) | 50 | 5.3 | 113 | 38 | 18 | 3,000-5,000 | 750-1,500 |
  | slow (ge > 100 ms/f) | 89 | 2.7 | 194 | 130 | 22 | ~19,700 (272 draws) | ~7,240 (77 draws) |

  Cost per vertex (Azahar ticks): **2.9 us** static unlit (`w0 l0`), 6.4
  with two lights and powf (`w0 l2 p`), **8.1** with 4 bone weights and two
  lights (`w4 l2 p`), 4.2 skinned unlit. So two lights with powf add ~3.5
  us a vertex and 4-weight skinning ~1.3. Even the plainest vertex is
  expensive: `read_one` decodes each component through `comp()` and a
  memory lookup, `memset`s, then three matrix products (world, view,
  projection: `to_clip` doesn't combine them), and `to_screen` converts
  the viewport registers per vertex. The game's own code also grows in
  those views (113 -> 194 ms/f: more characters to animate).
  **Options:** a CPU fast path per draw (combined matrix, direct pointers
  to the vertex data, the common formats decoded without `comp()`, a
  cheaper powf), or the vertex work on the PICA200's vertex shader (the
  vertex data copied to linear memory, transform / skinning / lighting in
  the shader), which removes it from the CPU.
- **The CPU vertex fast path (psprecomp `fast_vertices`, 2026-10-26).**
  Transformed draws without morph or texgen: one pointer to the vertices
  and one to the indices, components decoded in place, registers converted
  once per draw, world x view x projection as one matrix for unlit draws;
  on the 3DS a cheaper powf for the lights (`light_pow`, 1.1e-5 worst
  relative error against powf over 2 million values). PC check
  (`PSP2I_VTX_VERIFY=1`, town replay, 33 million vertices): on screen at
  most 0.0037 px from the generic loop; uv and colours identical. The
  user's town runs in Azahar (hot build; seconds by vertices a frame):

  | | town s | GE us/vertex | 18,000+ v/f: fps, game, ge ms/f | 8,000-18,000 v/f: fps, ge ms/f |
  |---|---|---|---|---|
  | before | 184 | 5.23 | 2.8, 202, 128 | 4.3, 62 |
  | fast path | 300 | 2.88 | 3.3, 185, 75 | 4.3, 46 |

  Per vertex: static unlit 2.91 -> 0.94 us, skinned with two lights 8.06
  -> 5.36. Lighting (`psp_ge_light`, ~3 us a vertex for two lights) is now
  most of the GE time; in busy views the **game's own code (185 ms a
  frame) is the biggest part**.
- **O1, prepared lighting (3DS_PLAN.md "T4 speed", 2026-10-26).**
  `psp_ge_light_prepare` once per draw (directional lights' direction and
  half vector, light x material colours, directional ambient folded, zero
  terms dropped), `psp_ge_light_fast` per vertex, in the fast path only.
  Checked: a randomized test (`test_ge_vertex`, 160,000 cases, within
  1e-4); PC town replay, 13.1 million lit vertices with identical colours
  (this game's lights have no ambient or specular colour, so the prepared
  form does the same float operations). The user's town run in Azahar
  (208 s): lit vertices **5.36 -> 3.82 us** (skinned, two lights) and
  **4.25 -> 2.88** (static, two lights); unlit unchanged at 0.95. Its
  busiest seconds drew 35,000 vertices and 609 draws a frame (the last
  run's: 27,500 and 430) at 3.6 fps with the same GE time (77 ms a frame).
- **O2, where a citro3d draw's time goes (2026-10-26).** `c3d_draw_on`
  timed in parts (the `c3d: us per draw` line every 10 s), the user's town
  runs in Azahar: target bookkeeping 4-13 us, **depth + texture lookup
  26-154**, vertex ring 2-11, state setters 2.7, submit (`C3D_DrawArrays`,
  citro3d emitting dirty state) 16, row marking 5-10. So caching state
  saves at most part of ~19 us a draw. The texture part (`c3d: texture
  checks per frame`): 60-137 hashes a frame (~0.8 M texels, 4-6 ms), and
  decoding up to 70 ms a frame in bursts. The decodes (`c3d: decode` lines)
  were nearly all **new** CLUT4 / CLUT8 textures in main RAM, ~150 ms for a
  512x512 CLUT4 (texel by texel through `psp_gpu_decode_texture`, then a
  second pass into tiles); a few evicted to fit the 12 MB budget.
  **Fast paletted decode** (`tex_fill_clut`, psprecomp
  `psp_gpu_texture_info`): one pass through a pointer, palette converted
  once, straight to tile order; the first 20 checked against the old path,
  0 texels different. Decoding 8-70 -> 0-2 ms a frame; steady frame rates
  unchanged (busy views 3.6 fps): decodes come in bursts as scenery comes
  into view, so this shortens those stalls. Left in the draw: hashing
  (4-6 ms a frame) and the submit.
- **O2, the rest: a citro3d state cache and a texture re-check schedule
  (2026-10-26).** `cs_*` in `c3d_ge.c` call a citro3d setter only when the
  value changed (scissor, depth, blend, blend colour, alpha test, texenv,
  bound texture, viewport uniform); `C3D_FrameDrawOn` turns the scissor off
  (its `C3D_SetViewport` writes the scissor mode: disassembly), so a target
  switch forgets it, and the overlap copies and the present forget all of
  it. **Submit 16 -> 10 us a draw, a draw 56 -> 44-45 us** in town.
  Texture bytes in RAM are re-hashed on a schedule (1, 2, 4 ... 16 frames
  while unchanged), every frame for VRAM textures and for 3 s after a
  loading-like file read (`psp_mem_load_writes`: 96 KB or more, or 4 KB or
  more somewhere the last 8 reads didn't touch; the town streams a 270 MB
  file 57-70 KB at a time into one buffer, ~5 reads a second, found with
  `PSPRECOMP_IO_TRACE` on the PC build), the CLUT on every first use in a
  frame; a faster 32-bit hash. In town after a few seconds: ~20 hashes a
  frame with ~140 skipped, hashing 4-6 -> 1-2 ms a frame. The user saw no
  stale or wrong textures (2 runs). Busy views 3.6-3.8 fps across the runs
  (the views differ run to run). O2 in all: ~10 ms a frame of ~290.
- **A GPU command buffer PANIC in busy town scenes (fixed).** 1,550-1,630
  draws a frame filled citro3d's 512 KB command buffer: svcBreak in
  libctru's `GPUCMD_AddInternal` at 105 s. The check before each draw used
  `C3D_GetCmdBufUsage()`, which is the fill at the last submit, and
  citro3d's sync transfers split the buffer mid-frame (its space comes back
  only when a frame begins). `cmd_room()` in `c3d_ge.c` now reads the bytes
  left (`GPUCMD_GetBuffer`) and submits and waits under 16 KB; the buffer
  is 1 MB. The next 300 s of town: no PANIC, 0 waits for room.
- **Azahar's CPU speed is not the console's.** Azahar emulates the ARM11
  with its own cycle count and runs this build at "Speed 90-100%", but how
  that compares with the console's 804 MHz with L2 isn't known. The CPU
  part (game + ge) has to be measured on the console before deciding
  between CPU work (an `-O2` build of the hot code, the GE's vertex work on
  core 2) and the rest.
- Azahar measures correctness, not speed. These numbers say where the
  time goes in Azahar; only the console says how fast it is.
