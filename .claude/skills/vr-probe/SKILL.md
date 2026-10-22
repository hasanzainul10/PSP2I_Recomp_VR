---
name: vr-probe
description: Reverse-engineer PSP2i game structures for VR (camera object, player position and yaw, skeleton bones, culling, bloom passes, lock-on, aim) with the F8 VR probe, memory watches and GE dumps, and record results in ADDRESS_MAP.md. Use for VR phase-0 work, or whenever VR code needs a game address that isn't mapped yet.
---

# VR reverse engineering

The tool is `port/vr/vr_probe.c`, built on psprecomp's `psp_gpu_set_draw_observer`.
Results go in `port/docs/ADDRESS_MAP.md`: each entry gives its source and its
status (known / seen / todo).

## Getting reports

1. Build a development build (not `PSP2I_PROD`) and run the game (skill
   `psp2i-build-run`). Reports need a 3D scene: town, a field, a mission.
   Menus and character creation have no scene camera.
2. In the game window, press **F8**. `port/build/Release/logs/vr_probe.txt` gets a
   report. The user can press it while playing; or script it with
   `--press`/`--stick` and a known save.
3. Move or turn a little and press F8 again. Two or three reports are enough.

`PSP2I_VR_PROBE_EVERY=60` prints `vr-probe: flip N eye (...) fwd (...) fovy F`
every 60 frames, which is useful to confirm that the camera follows play.

## Reading a report

**PSP2i's GE view matrix is always the identity**: the camera is folded into
the bone and world matrices on the CPU. So `view i (scene)` shows eye (0,0,0),
and the camera-position and view-matrix RAM hits are noise. Use instead:

- **`camera matrix search`:** rigid 4x4 matrices in RAM that map a float triple
  of the local player object to the player model's eye-space position. The
  view matrix is `0x08FA98A0`, with the player position at player +0x4B0
  (ADDRESS_MAP.md). Keep the addresses that come back in two reports.
- **`unskinned draws` / `world of the largest unskinned draw`:** their WORLD
  is the model-view.

The generic fields (for games that do use the GE view):

- **`view i (scene)`:** the view matrix with the most vertices, taken as the
  scene camera. Others are sky, shadows or the HUD's 3D parts.
- **`camera position at 0x...`:** RAM holding the eye as 3 floats. The camera
  object is around it; dump the area with `--log-floats ADDR COUNT EVERY` to
  find the target, up and FOV next to it.
- **`view matrix (4x3 packed | 4x4 padded) at 0x...`:** the game's own copy of
  the matrix. Its writer is the matrix builder (P2 replaces the view there).
- **`skinned models ... #0`:** usually the player in third person, as the
  model nearest the line of sight. Its `world translation at 0x...` hits are
  candidates for the player position.

## Narrowing down

- **Compare reports:** keep the addresses whose values changed with the move,
  across the reports.
- **Confirm with a write watch:** `--watch ADDR`, or `--watch-from-flip ADDR FLIP`.
  It logs the game stack of every writer, which gives the function to hook
  (`port/hooks.txt` plus `psp_hook_set`; see `host/camera.c` for the pattern).
- **Where matrices come from:** `PSP2I_GE_MATRIX_FLIP=N` logs where frame N's
  matrix uploads come from in the game.
- **Whole-frame dump:** F12 writes every draw with its target, through-mode
  flag, matrices and state. Bloom and blur show up as full-screen through-mode
  draws that sample a render target.
- **Player object:** `hook_is_local` in `host/gamelog.c` already identifies the
  local player object (`0x08A3DE1C`). Offsets from it are often quicker than RAM
  scans.

## Recording

Add each confirmed address to ADDRESS_MAP.md: what it is, its layout, how it
was confirmed. Move it from "todo" to "known" once code relies on it. Update
`port/docs/VR_PROGRESS.md` too.
