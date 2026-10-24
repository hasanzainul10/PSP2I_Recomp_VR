---
name: psp2i-hook-re
description: Find and change what PSP2i decides at run time, using the recompilation itself - hook any game function (count its calls and results, compare first vs third person or two places, override its answer, run it with altered globals, switch experiments live from the gamepad), then regenerate and test. Use whenever something is missing, wrong or different in VR and the cause is a game decision (what to draw, culling, LOD, cameras, menus, effects), and before trying fixes by guessing. Pairs with psp2i-static-re (reading code and captures) and vr-probe (live memory).
---

# Run-time reverse engineering with hooks (PSP2i)

The game is recompiled C, so every function can be hooked and every
decision it makes can be counted or changed. Don't fix a headset report
by guessing a cause and running again: find the game's decision first.

## The loop

1. **Reproduce and capture.** In the headset: gamepad L3 held 1 s saves a
   VR capture (eyes, HUD picture, a per-draw trace with the frame's draw
   count). Ask the user to capture at the moment, and to switch the thing
   that matters (L3 twice: first / third person) and capture again.
   `python -I .claude/skills/psp2i-static-re/scripts/vrshot.py DIR` renders
   them; read the PNGs. The user's own screenshots don't reach you as
   pictures: rely on captures.
2. **Decide which side drops it.** psprecomp (the GE emulation) or the game:
   - psprecomp draws all it is given: `GE_BJUMP` (bounding-box skip) is never
     taken (`psprecomp/src/hle/ge.c`). It does drop: triangles behind the
     clip plane (`emit_tri`, `g_clip_w`, `near_w`), back faces
     (`culled_area`, `two_sided`), HUD-pass routing (`stereo_classify`).
   - Otherwise the game never submitted it: find the game's decision.
3. **Find candidates statically** (`psp2i-static-re`): `global_refs.py` on
   the globals involved (view `0x08FA98A0`, projection `0x08FA97A0`, cull
   planes `0x08FA9760`, near/far `0x08FA9818`), `callers.py`, `disasm.py`
   (`--all`: big functions are split into interior entries, and the plain
   listing shows only fragments). Note each candidate's arguments and
   return values (`$v0`), and its callers.
4. **Hook and measure.** Add the candidates to `port/hooks.txt`, register
   hooks in `port/host/main.c` (`psp_hook_set`), dev-only code in
   `#ifndef PSP2I_PROD`. Count per reason and log every ~5 s with the state
   that matters (`g_vr_first`, `g_head_on`): compare first vs third person,
   or a place where it's missing vs one where it isn't.
5. **Regenerate and build** (`psp2i-build-run`): `allegrexrecomp emit ...
   --hooks hooks.txt --fixes fixes.txt`, then check
   `grep -c 'psp_hook_find(0xADDR' port/gen/recomp_funcs.c`, then the full
   build in the background (~10 min; the game must be closed: the exe is
   locked). A headless run (`--no-vr --headless --seconds 30`) checks VR off
   is unchanged.
6. **Run, read the log, fix at the decision**, then record the address in
   `port/docs/ADDRESS_MAP.md` (source, status) and the result in
   `VR_PROGRESS.md`.

## Hook patterns (all in host/main.c)

A hook is `static void h(void (*original)(void))`; `psp_cpu.r[4..7]` are
`$a0..$a3`, `r[12]` `$t0`, `r[2]` `$v0` (the result), floats in
`psp_cpu.f[12..15]`. Read memory with `psp_read32` / `psp_read_f32`.

- **Count and classify** (find which rule acts):
  ```c
  static void hook_x(void (*original)(void)) {
      const uint32_t before = psp_read32(psp_cpu.r[4] + psp_cpu.r[6] * 4u);
      original();
      /* classify by before / after / psp_cpu.r[2]; n_reason[k]++;
       * every 300 vblanks: fprintf(stderr, "vr: ... (%s person) ...") and reset */
  }
  ```
  Example: `hook_node_vis` (scene node visibility, `0x08D7D270`).
- **Override the answer** without running it: set `psp_cpu.r[2]` and return.
  Know the convention first: the frustum tests return 16 out, 2 in, else
  crossing (`hook_cull_test`).
- **Run it with an altered global**, restored after: widen the projection
  only while the function runs (`hook_frustum_planes`). Generic: it needs
  no knowledge of the function's results.
- **Replace its input** before it runs: write the object it reads
  (`hook_apply_camera` writes the field camera's view matrix at the head).
- **Live experiments**: switch candidates from the gamepad while the user
  watches, and log the switch (a held button stepping through a list), so
  one run tests many candidates.

## Rules

- **Online:** change only what this game draws or how its camera looks, and
  what it presses. Never game state that other players see.
- **VR off is unchanged:** every override is gated (`g_head_on`,
  `g_vr_first`, a `vr_*` setting) and can be switched off in the ini.
- **Feedback loops:** a value the game reads back next frame (the orbit
  controller saves the camera's eye each frame) can't be overwritten at its
  source. Hook one level down (ApplyCamera's view matrix, not the eye).
- **Measure, don't assume:** if no candidate changes anything, say so and
  move one level up the call chain. Don't present a guess as the cause.

## Pitfalls met

- psprecomp's `on_frame` runs on a copy of `psp_gpu_stereo`; only fields
  copied back after it take effect (`eye_view`, `tag`, `flat_frame`,
  `near_w`, `fx_dist`).
- Capture folders: `--capture-dir` must exist or be makeable (parents are
  made since 2026-10-23).
- Bash heredocs turn `\\n` into real newlines: write edit scripts with the
  Write tool and run them, or use Edit. Many host files mix CRLF and LF.
- A hook that skips the original must leave `$v0` (and any out-argument)
  as the callers expect.
