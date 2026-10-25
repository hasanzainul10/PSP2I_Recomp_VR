# PSP2i New 3DS port: progress

Plan: [3DS_PLAN.md](3DS_PLAN.md). Branch `3ds_port`, worktree
`D:\work\PSP2I_Recomp_3DS`. Skills: `psp2i-3ds-build`, `psp2i-3ds-test`.
The user's console: **New 3DS XL, Luma3DS**.

## Decisions

- **2026-10-25 · The game ships as a `.cia`.** A `.3dsx` gets the
  APPLICATION region of a normal title (96 MB, measured in Azahar). The game
  needs ~100-135 MB. The `.cia`'s exheader (`port/3ds/psp2i.rsf`) asks for the
  New 3DS 124 MB mode, core 2, 804 MHz and L2. The `.3dsx` stays for small
  apps pushed with 3dslink. 178 MB was left out: it takes memory from the
  HOME menu.
- **2026-10-25 · Renderer: citro3d on PICA200**, through `psp_gpu_backend`.
  Unai (the PS1 software GPU from PCSX4ALL) was evaluated and rejected: no
  depth buffer, affine texturing, PS1 command set (3DS_PLAN.md section 3).
- **2026-10-25 · `3ds_port` was fast-forwarded to `vr-theater`** (a1a3787), so
  it has the HUD pass, `draw_hud`, `mono_is_left` and the stereo work.

## T0: setup (in progress)

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

Not checked yet, **on the console**:

- [ ] the `.3dsx` through 3dslink and the `.cia` through FBI both boot;
- [ ] `app_mem` 124 MB for the `.cia` on real hardware;
- [ ] `cpu ~804 MHz` (does `osSetSpeedupEnable` work from our title);
- [ ] `core2=2` for the `.cia`, `-1` for the `.3dsx`;
- [ ] the HOME menu icon and banner look right.

## Next: T1 (compile and fit)

- `split_gen.py` into small units for GCC; the runtime (`psprecomp/src`) and
  the generated code built for ARM11 with `-Os`, no LTO.
- `.text` size against the budget (3DS_PLAN.md section 2). With 93.8 MB of
  heap after the code is loaded, the code size decides whether the 32 MB
  guest RAM + caches fit.
