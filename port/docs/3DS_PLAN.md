# PSP2i on New 3DS: plan

Goal: run the recompiled game (NPJH50332) natively on **New 3DS / New 3DS XL /
New 2DS XL**, with stereoscopic 3D on the top screen and the HUD on the
bottom screen. Branch: `3ds_port`.

Reference project: [P3P3DS](https://github.com/endercodezz/P3P3DS) (Persona 3
Portable, the same idea: a PSP static recompilation, citro3d on PICA200). It
is "playable, early": 28-30 fps in the field and 16-25 fps in battle on New 3DS,
with no audio and no stereo yet. Its `docs/3DS_PLATFORM.md` is the best
list of the pitfalls and is summarised under "Lessons from P3P3DS" below.

Old 3DS / 2DS is out of scope. P3P3DS reached the same conclusion: 268 MHz
and 64-96 MB of memory would need a different design, not tuning.

---

## 1. What we already have

| Piece | Where | Use on 3DS |
|---|---|---|
| Hardware GE backend: the CPU does vertex decode, transform, clipping and culling, and hands over **screen-space triangle lists + decoded state** | `psprecomp/include/psprecomp/render.h` (`psp_gpu_backend`), `src/hle/gpu.c` | A citro3d backend is one more `psp_gpu_backend`, like `d3d11.c` / `gl_ge.c`. This is the design P3P3DS uses on PICA200. |
| Stereo: `draw_eye` per eye, `mono_is_left` (two pictures, not three), off-axis convergence, `flat_after_clear` for PSP2i's 3D HUD | `gpu.c` `psp_gpu_set_stereo` (VR P2) | The top screen's left/right eyes. |
| HUD picture: `draw_hud` routes the HUD pass to a separate picture | `gpu.c` ST_PANEL / `hw_draw_hud` (VR HUD panel) | **The bottom screen.** The HUD is already separated from the scene. |
| Software rasterizer as the oracle | `psprecomp/src/render.c` | Check the PICA output against it on PC. |
| Splitting the 200 MB generated C into parts | `port/android/split_gen.py` | GCC (devkitARM) needs it even more than clang. |
| SDL3 host for non-Windows targets | `port/host/sdl_main.c`, Android/macOS ports | The template for a `3ds_main.c` (no SDL; libctru instead). |
| Static and hook RE skills | `.claude/skills/psp2i-static-re`, `psp2i-hook-re` | HUD element identification, finding per-element draws for the bottom-screen layout. |

The branch `3ds_port` is currently `fd149a1` (VR P2), 87 commits behind
`vr-theater`, with no commits of its own. Fast-forwarding it to `vr-theater`
brings the HUD pass, `draw_hud`, `mono_is_left` and the GE fixes in.

## 2. Hardware budget

| | PSP | New 3DS |
|---|---|---|
| CPU | Allegrex MIPS 333 MHz | 4x ARM11 MPCore 804 MHz (2 usable, plus core 2 with an exheader flag), VFPv2, **no NEON**, 2 MB L2 |
| RAM | 32 MB (`PSP_RAM_SIZE`) + 2 MB VRAM | 256 MB FCRAM; the app gets 64-178 MB depending on the memory mode; 6 MB VRAM |
| GPU | GE (fixed function, bones, morph) | PICA200 (vertex shaders, 6-stage TEV combiner, no paletted textures, no logic ops) |
| Screen | 480x272 | Top 400x240 per eye (800x240 in wide 2D mode), bottom 320x240 resistive touch |

Memory estimate (to verify on hardware):

| Item | MB |
|---|---|
| Recompiled code (`-Os`; P3P3DS went from 77 to 44 MB) | 40-60 (the x64 exe is 35 MB) |
| Guest RAM + VRAM mirror | 34 |
| Linear heap (textures, vertex buffers, command lists) | 16-30 |
| Runtime, HLE, audio, file cache | ~10 |
| **Total** | **~100-135** |

This is above the 96 MB of a default `.3dsx`. **Decided: the game is a
`.cia`** with the New 3DS 124 MB memory mode in its exheader, which also
unlocks core 2 (`port/3ds/psp2i.rsf`). In Azahar it gets 124 MB, with 93.8 MB
of heap and 32 MB of linear memory left for the app.

## 3. Renderer: citro3d on PICA200 (and the Unai question)

**Choice: citro3d on PICA200, through the existing `psp_gpu_backend`.**

- The CPU keeps doing what `gpu.c` already does (decode, transform, clip),
  and outputs `(x*w, y*w, z*w, w)`. The PICA vertex shader only maps pixels to
  clip space. P3P3DS uses the same split.
- GE state → TEV: texture function (modulate/decal/blend/replace/add), alpha
  test, depth test/mask, blend equations (approximated where PICA lacks
  them), colour mask, scissor, cull, filtering, clamp/wrap.
- Textures: CLUT4/CLUT8 converted to RGBA at upload (PICA has no palettes),
  Morton 8x8 tiling, rows bottom-up, a cache keyed by `tex_key` + palette
  hash with an LRU budget (P3P3DS uses 6 MB).
- Render targets: a 512x512 VRAM texture per PSP framebuffer address, with a
  shared D24S8 depth buffer. A texture at a target's address binds the
  target (render to texture, used by bloom and the map).
- Not on PICA: logic ops, stencil corner cases, and GPU-to-guest-RAM pixel
  readback. Each needs a per-case decision, found with `psp2i-hook-re`
  (what reads it back, and can that pass be skipped).

**Unai: not an option for this game.** Unai (`gpu_unai`) is the PCSX4ALL
**PlayStation 1** GPU plugin that PCSX ReARMed uses on 3DS because it's faster
than P.E.Op.S. at the cost of accuracy. It's a software rasterizer for the PS1's
command set:

- no depth buffer (the PS1 has none), so it can't draw PSP2i's 3D scenes;
- affine texturing, no perspective correction;
- 15-bit colour, the PS1's 1024x512 VRAM model, the PS1 blend modes only;
- PSP GE features (bones, 32-bit colour, CLUT formats, swizzle, alpha test,
  depth test, blend equations) are missing.

Adapting it would mean writing a new rasterizer. A software path is also the
wrong direction on this hardware: P3P3DS measured its software renderer at
**2 fps vs 45 fps on PICA200** (Azahar, title screen). Stereo would double the
rasterizer cost, while on PICA200 the second eye is nearly free.
What can be borrowed is its *technique* (fixed-point span loops, no per-pixel
divides) if a CPU fallback is ever needed for one effect pass PICA can't do.
The runtime's own software rasterizer already fills the "reference" role.

## 4. Stereoscopic 3D

Two stages:

1. **Correctness first:** reuse `psp_gpu_set_stereo` with `mono_is_left`. The CPU
   transforms each perspective draw per eye (`draw_eye`). `osGet3DSliderState()`
   scales the separation; slider 0 = mono (and switch the top screen to
   800x240 wide mode for sharper 2D).
2. **Speed:** the CPU transform per eye doubles the vertex work, the
   bottleneck on ARM11. Move the per-eye shift into the PICA vertex shader:
   one CPU transform, then for each eye `x_clip += sep * (w - converge)`, set
   as a shader uniform per eye. Through-mode draws (w = 1) and the HUD pass
   get no shift (screen depth). This is the standard 3DS stereo method, and the
   GPU draws twice for almost no CPU cost.

Comfort: convergence at the character (the camera's look-at distance from
the VR P0 camera work, `ADDRESS_MAP.md`), with separation capped well below VR
values. The 3DS screen is small and the eyes are close.

## 5. HUD on the bottom screen

`draw_hud` already gives a separate HUD picture. Steps:

1. **Whole HUD, scaled:** the HUD picture (480x272) drawn to the bottom
   screen (320x240) as one textured quad. The top screen gets the scene only.
   Quick to make, but text shrinks to 0.67x.
2. **Per element:** identify HUD elements (HP/PP bars, minimap, palette,
   chat, lock-on, prompts) by their draw signatures (texture key and
   screen rectangle; the VR `hud_panel` layers already split some). Then place
   each on the bottom screen at a readable scale, and keep a few in the top
   screen at screen depth (lock-on reticle, damage numbers, prompts that are
   tied to the world).
3. **Touch:** map bottom-screen touch to the game's existing inputs only
   (palette slots → the pad buttons that select them, via `psp_ctrl_set`).
   No new game actions.
4. **Menus:** full-screen menus (shop, inventory) stay on the top screen at
   screen depth, mono. The bottom screen shows the map or is blank.

## 6. Tools

| Tool | Purpose | Status |
|---|---|---|
| **devkitPro** (pacman): `3ds-dev` = devkitARM GCC, libctru, citro3d, citro2d, picasso, 3dsxtool, `3ds-cmake` | Build | Not installed |
| **makerom** + **bannertool** | `.cia` with exheader (memory mode, core 2) | Not installed |
| **Azahar** (the Citra successor, New 3DS mode) | Fast bring-up loop, GDB stub, logging, screenshots | Not installed |
| Python (existing) + `split_gen.py` | Split the generated C for GCC | Have |
| `arm-none-eabi-size`, `nm --size-sort` | Code size budget per build | With devkitARM |
| Hardware: New 3DS with Luma3DS, Homebrew Launcher, **3dslink** (WiFi upload of `.3dsx` + stdout), **ftpd** (files, logs), Rosalina's GDB stub and screenshots | The truth for speed and memory | Ask the user |
| PC "3DS layout" mode in the Windows build (`--3ds-layout`: a 400x240 top window + 320x240 bottom window, anaglyph or side-by-side stereo) | Iterate on HUD layout and stereo at PC speed with the existing tools | To build (T2) |

Azahar measures correctness, not speed (P3P3DS found it doesn't even enforce
the memory limit). Speed and memory claims come from hardware only.

## 7. Skills to add (`.claude/skills/`)

- **`psp2i-3ds-build`**: install check for devkitPro; regenerate `port/gen`;
  split it; configure `port/3ds` with `3DS.cmake`; build in the background;
  report `.text` size against the budget; make the `.3dsx` and the `.cia`.
- **`psp2i-3ds-test`**: run in Azahar (headless frames, log, screenshot of
  both screens); push to hardware with 3dslink and read its stdout; fetch
  `sdmc:/psp2i/report.txt` with ftpd; attach GDB (Azahar or Rosalina); how to
  label results: "PC layout mode", "Azahar", "hardware".
- **`psp2i-3ds-perf`**: the on-device profiler. Per-frame time split
  (game code, HLE, GE translate, texture upload, GPU wait, audio) on the
  bottom screen and in `report.txt`; the per-scene table we compare against
  (town, lobby, field, boss).
- Update **`psp2i-static-re`** / **`psp2i-hook-re`** with a "HUD element
  census" recipe (draws in the HUD pass grouped by texture key and rectangle).

## 8. Rules to add to `CLAUDE.md`

- **3DS off means no change:** 3DS code under `#ifdef PSP2I_3DS` (host) and
  `PSPRECOMP_3DS` (runtime). The Windows, Android, macOS and VR builds must stay
  byte-for-byte the same in behaviour.
- **Code size and memory are budgets:** every 3DS build prints `.text` and
  peak heap. A change that grows them by more than 1 MB says why in its
  commit.
- **Report where it ran:** "builds", "PC layout mode", "Azahar" and
  "hardware" are different claims. Frame rates are only quoted from hardware.
- **Online:** the same rule as VR. The bottom screen and touch may only
  change what is drawn locally and what is pressed through `psp_ctrl_set`.
  Online on 3DS (soc:u, WPA2) is a late phase, after offline is stable.
- **No game data in builds:** the `.3dsx` / `.cia` contain generated code from
  the user's EBOOT. Never commit or publish them. The user builds their own,
  like the Android APK.
- **Runtime changes go to `psprecomp`** (submodule, fork remote) first,
  shared with the other ports.

## 9. MCP

**None needed now.** The work is local builds, an emulator and a device on
WiFi, which Bash, the skills and `gh` cover. The MCP registry has no 3DS,
emulator or GDB server. If the hardware loop becomes the slow part, a small
local MCP server wrapping 3dslink + ftpd + Rosalina screenshots (`push`,
`log`, `screenshot`, `report`) can be written later.

## 10. Roadmap

Each phase ends with something checked. Where it was checked is part of the
exit criterion.

| Phase | Work | Exit criterion |
|---|---|---|
| **T0 Setup** | Fast-forward `3ds_port` to `vr-theater`. Install devkitPro, makerom, Azahar. Write the two build/test skills and the CLAUDE.md rules. Decide `.3dsx` vs `.cia`. | `hello.3dsx` from `port/3ds` shows text on both screens in Azahar and on hardware. |
| **T1 Compile & fit** | `port/3ds/CMakeLists.txt` with the 3DS toolchain; `split_gen.py` for GCC; `-Os`, no LTO; runtime builds for ARM11 (endianness is fine, check alignment and `long` size); dispatch tables sized like P3P3DS's direct window. | The whole game links. `.text` and the memory estimate are known. Decision on the memory mode. |
| **T2 Boot, software** | `port/host/3ds_main.c`: libctru init, `osSetSpeedupEnable`, FS for the ISO on SD (`sdmc:/psp2i/`), the pad mapped to `psp_ctrl_set` (circle pad → stick, C-stick → camera), the software rasterizer to the top screen. PC layout mode in parallel. | The game reaches the title screen in Azahar (slow is fine). |
| **T3 citro3d backend** | `port/host/c3d_ge.c` implementing `psp_gpu_backend`: TEV, textures, targets, depth. Checked against the software oracle frame by frame on PC where possible. | Town and a field render correctly in Azahar. First **hardware** fps numbers per scene. |
| **T4 Speed** | Profiler; GE translation + texture decode on a second core; vertex cache; texture cache budget; audio (ATRAC3+ / SAS) on its own thread through ndsp; 30 fps frame pacing; skip or decode movies (the New 3DS has an H.264 decoder service, `mvd:STD`). | Field ≥ 25 fps, town 30 fps on hardware, with sound. |
| **T5 Stereo** | Stage 1 (`draw_eye`, slider), then stage 2 (shader shift). Convergence at the character. | 3D seen on the device by the user, with the cost of stereo measured (target < 10%). |
| **T6 Bottom-screen HUD** | Step 1 (whole HUD scaled), then the per-element layout, touch palette, and menus. | The user plays a mission using the bottom-screen HUD. |
| **T7 Saves, settings, polish** | Saves on SD (compatible with the Windows `GameData/ms` layout), an engine menu page (stereo strength, HUD layout), a home menu banner and icon. | A save made on 3DS loads on PC and the reverse. |
| **T8 Online (optional)** | RPCN / ad hoc server over soc:u, after offline is stable. | A lobby joined with a PC player, nothing changed in the shared state. |

### T4 speed: the plan from 2026-10-26

Where a busy town frame goes in Azahar after the stack fix, the hot build and
the CPU vertex fast path (3DS_PROGRESS.md, T4): **game code 185 ms, GE vertex
work 75, citro3d draws 25, waits and the rest ~15** (3.3 fps); normal views
113 / 38 / 18 (5-6 fps). Two facts set the order: the generated code keeps
the MIPS registers in the global `psp_cpu` (`#define r_v0 psp_cpu.r[2]`), so
the compiler reloads them after every guest store (a byte store may alias
it) and every call; and GE lists run synchronously inside the enqueue
(`ge.c`), so the game waits for all GE work.

| Step | Work | Measure | Guess |
|---|---|---|---|
| **O0** console baseline | The ISO on the console's SD, the current `.cia`; read `time:` / `vtx:`. Azahar doesn't model the CPU speed or cores, so this can reorder the rest. | console | - |
| **O1** lighting | Per draw instead of per vertex: directional lights' directions and half vectors, terms with a zero colour skipped, specular only when it contributes. `PSP2I_VTX_VERIFY` checks it. All platforms. | `vtx:` us/vertex for `l2 p` kinds | busy GE 75 -> ~45 ms |
| **O2** citro3d state | ~55 us a draw: don't send texenv, blend, depth, uniforms that didn't change. | `draw` ms/frame | part of 25 ms |
| **O3** registers in locals | allegrexrecomp: each function copies the registers it uses into locals at entry, writes them back to `psp_cpu` before any call, syscall, dispatch or return and reloads after calls (a call can switch PSP threads or run a hook). Prototype on the hot list; game time on PC (town replay) then 3DS; `.text` watched. All platforms. | `game` ms/frame | if 1.5-2x: busy -60..90 ms |
| **O4** GE off the game's thread | (a) each draw's vertex loop split with core 2 (no change in behaviour), or (b) lists asynchronous like the hardware, waiting at DrawSync / ListSync / callbacks / VRAM reads (hides up to ~100 ms; risk: a game changing data a queued list still reads). Chosen after O0. | console | up to -100 ms |
| **O5** vertex work on the PICA200 | Vertex data copied to linear memory, transform and skinning (then lighting) in the vertex shader; replaces O1 / O4 for the GE time. Only if the GE is still the bottleneck on the console. | console | most of the GE time |

Order: O1, O2 (small, measurable in Azahar), then O3 (the biggest number);
O4 / O5 after O0. If O1-O3 land as guessed: busy views ~150-180 ms a frame
(5-6 fps), normal views 8-10 fps in Azahar; the console decides what that
means.

## 11. Risks

- **CPU speed is the main risk.** PSP2i has more enemies and effects than
  P3P, which already drops to 16-25 fps in battle. Measure on hardware at T3,
  not T5. If battles are too slow: 30 fps cap, LOD and effect hooks (local
  drawing only), bloom off, lower-resolution targets.
- **Memory:** a `.cia` with a large memory mode may be required. It's
  installable on any CFW console, but it's a different install from the
  Homebrew Launcher.
- **Readbacks:** any pass where the game reads GPU pixels back from VRAM
  (bloom, map, screenshots) breaks on PICA. Find each one with a hook census.
- **GCC build time and memory** on 200 MB of C: split finely (P3P3DS uses
  16 KiB units), and build in the background.

## 12. Questions for the user

Answered 2026-10-25:

1. Console: **New 3DS XL with Luma3DS** (has the stereo screen).
2. Tools are installed on **D:** (`D:\msys64`, `D:\Tools`).
3. **`.cia` from the start** for the game; see `3DS_PROGRESS.md`
   "Decisions" (Azahar measured 96 MB for a `.3dsx`, 124 MB for our `.cia`).
