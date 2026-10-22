# PSP2i address map (for the VR work)

Guest addresses in NPJH50332's EBOOT.BIN, as the recompiled code sees them. Each
entry says where it came from and how sure it is. Add to this as the VR probe
(F8, `port/vr/vr_probe.h`) and the memory watches find more.

Status: **known** = used by working code; **seen** = observed, not yet relied
on; **todo** = needed for a VR phase, not found yet.

## Known (used by the port today)

| Address | What | Source |
|---|---|---|
| `0x08A16794` | Camera yaw/pitch input: `(?, float *yaw $a1, float *pitch $a2)` | `host/camera.c` (right-stick camera) |
| `0x08AFA530` | "Camera is being controlled" test; forced true from `$ra 0x08AF8C50` | `host/camera.c` |
| `0x08A1681C` | Camera input curve | `host/camera.c` notes |
| `0x08D64068` / `0x08D64074` | Pad update | `host/camera.c` notes |
| `0x08B86320` | The game's main loop | `host/framerate.c` |
| `0x08B85AB0` | Frame pacing, once per frame | `host/framerate.c` (`hook_frame`) |
| `0x08B81890` | setFrameRate | `host/framerate.c` |
| `0x08B81830` | Timing fill | `host/framerate.c` (`adapt`) |
| `0x08EDA54C` | Timing state: +0x08 fps (float), +0x0C fps (int), +0x10/+0x14 vblanks per frame, +0x18 30/fps, +0x1C dt | `host/framerate.c` |
| `0x08EDA560` | 1/60 s tick counter | `host/framerate.c` (`pace_ticks`) |
| `0x08EDA698` | Frame-end vcount | `host/framerate.c` |
| `0x08EE2138` | fps object pointer | `host/framerate.c` |
| `0x08A3DE1C` | "Is this the local player?" `$a0` = player object | `host/gamelog.c` (`hook_is_local`) |
| player +0x00 | vtable | `host/gamelog.c` |
| player +0x28 | player id (1 the other human; NPCs 2, 3) | `host/gamelog.c` |
| player +0x40 | status link | `host/gamelog.c` |
| player +0x6A8 | HP | `host/gamelog.c` |
| `0x08ED8CEC` | Item owner | `host/gamelog.c` |
| `0x08ED59E8`, `0x08ED5A28` | Session objects | `host/gamelog.c` |

## Seen (F8 probe, 2026-10-22: town and a field, 30 fps, English ISO)

How the game draws 3D, from 4 scene reports a few steps apart:

- **The GE view matrix is always the identity.** So are the world matrices of
  every skinned draw. The camera (and each model's placement) is multiplied
  into the bone matrices on the CPU/VFPU before upload. The vertices reach the
  GE already in eye space. P2 can't replace the camera at the GE's VIEW
  register: it needs the game's own camera matrix, or a hook where it is
  multiplied in.
- **Projection** (GE PROJ): fovy 45.0°, aspect 1.765 (480/272), the same in
  town and field. 4x4: `1.3684 0 0 0 / 0 2.4149 0 0 / 0 0 -0.0067 -1 / 0 0 -201.34 0`.
  So clip w = -z (the view distance) and the horizontal half-FOV tangent is
  1/1.3684 = 0.731. **Used by P2 stereo** (2026-10-22, `psp_gpu_set_stereo` in
  psprecomp gpu.c): a draw counts as perspective when PROJ[11] ≠ 0, and its
  vertices move per eye by their distance (w); the stereo window's width is
  2 · distance · (1/PROJ[0]). Status: known.
- **The player model** is the skinned model nearest the line of sight: about
  55 GE units ahead of the eye and about 1 below it (`(-0.3, -0.9, -54.9)` in
  eye space). Vertex data at `0x09B3F850`, texture `0x0935A020`, 571 vertices
  in 4 draws, 3 weights.

| Address | What | Status |
|---|---|---|
| `0x0955A990`, `0x0955AAD0`, `0x0955AC10` | Player bone matrices (eye space, so camera included): the first bone's origin, in every scene report | seen |
| `0x095970D0`, `0x09597550`, `0x095977D0`, `0x09597810`, `0x09597850` | The same origin again: likely the bone palette as uploaded | seen |
| `0x0904F570`, `0x09054590`, `0x0904F460`, `0x090681D0` | Local player objects (`hook_is_local`); the address changes with the area | seen |
| `ms0:/PICTURE/NPJH50332SS/*.jpg` | Start in the field takes the game's own screenshot (not a port bug) | known |

### Camera matrix and player position (F8 "camera matrix search", second run)

The probe searched RAM for rigid 4x4 matrices that take a float triple of the
local player object to the player model's eye-space position. Two field reports
(vblank 7593 and 8299, a few steps apart with a ~45° camera turn) gave the same
answer at the same addresses:

| Address | What | Status |
|---|---|---|
| `0x08FA98A0` | **The view matrix** (world to eye), 4x4 column-major, VFPU layout (columns of 4 floats, translation in floats 12 to 14, last row 0 0 0 1). It's in the module's BSS (the image ends at `0x08FAA744`), so it's a global, likely the canonical copy | seen (2 reports; writer not yet watched) |
| `0x09049170` | The same matrix (heap) | seen |
| `0x09406020` to `0x09407200` (about 12 slots, 0x40 apart), `0x0942FE70` | Copies of it, probably a per-draw matrix stack. `0x09406980` sometimes holds the previous frame's value | seen |
| player +0x4B0 | **The player's world position** (x, y, z floats): **the feet**. The view matrix maps it to 3.05 units from the first bone (the hips). The GE probe (`psp_gpu_stereo_probe`, 2026-10-22, standing still in town, 6 frames): the model within 1.5 units of the vertical through it reaches **5.77-5.91 units above** it (the head's top), so ~3.4 game units per metre for a ~1.7 m character. The orbit camera sits 48-72 units behind (usually ~56) and aims 3.3-4.4 units above it (the chest). Read every frame by VR first person (host/main.c `head_frame`) | seen (2 reports + probe) |

The camera from that matrix: report 1 eye (-99.96, 17.82, -374.91), forward
(0.985, -0.052, -0.165); report 2 eye (-49.15, 11.86, -439.37), forward (0.801,
0.056, 0.596). +Y is up. The player is about 55 units ahead of the eye.

Unskinned draws aren't drawn with an identity world matrix either: their GE
WORLD is the model-view (view x model, with the model's scale; e.g. a
player-attached model at scale 1.88). So every 3D draw arrives in eye space.

### Who sets the view (write watch, 2026-10-22)

`PSP2I_VR_PROBE_WATCH=0x08FA98D0` (the matrix's translation x), armed by F8 in
a field. Every write comes through the same path, 3 times per frame:

`0x08AFDC60` → `0x08AFD474` → `0x08A88E00` → `0x08D81D84` → `0x08D7AE34` (the copy)

The calls reach `0x08AFDC60` from two places: `0x08AFCE6C` (from `0x089F83B4`,
`0x08A879AC`) and twice from `0x08AFED44` (via `0x08969314` and `0x08969344`).
Two of the 3 writes carry translation x 262.9 (the scene camera); the one via
`0x08969314` carries 6.31 (another camera: maybe the sky or a 3D HUD part, not
checked).

Decoded from the generated code (`port/gen/recomp_funcs.c`):

| Address | What | Status |
|---|---|---|
| `0x08D81D84` | **SetViewMatrix(`$a0` = 4x4)**: copies it to `0x08FA98A0` (`0x08D7AE34`, 4 `lv.q`/`sv.q`), then `0x08D81DAC`. `$a0` = 0 takes another path. **The P2 hook point**: swap in the per-eye view here | seen (decoded, not hooked) |
| `0x08A88E00` | Wrapper: `0x08D81D84($a0)`, then `0x08A88ECC` (marks something dirty: byte +66 = 1 of `0x08A88DE8()`) | seen |
| `0x08AFD474` | **ApplyCamera(`$a0` = camera object)**: SetViewMatrix(cam+0x10), SetProjection(cam+0x90, 0) (`0x08D7DA78`), `0x08D7D8E0`(cam+0xF4, cam+0xF8), then the viewport (`0x08D7D868`) | seen |
| `0x08AFD500` | ApplyCamera's default when there's no camera: view `0x08ED9860`, projection `0x08ED98E0` | seen |
| `0x08AFDC60` | Apply the active camera: index at `0x08ED9920` (-1 = none), slot table at `0x08ED9924` (pointers); slot+4 = the camera object | seen |
| camera +0x10 | View matrix, 4x4 VFPU column-major. The heap copy at `0x09049170` (above) is likely camera +0x10, so camera object `0x09049160` in that run (not confirmed) | seen |
| camera +0x90 | Projection matrix 4x4 (`0x08D7DA78` copies 64 bytes to `0x08FA97A0` and checks whether it's orthographic) | seen |
| camera +0xF4, +0xF8 | Two floats, stored by `0x08D7D8E0` at `0x08FA9784`/`0x08FA9780` and `0x08FA981C`/`0x08FA9818` (likely near/far or depth range) | seen |
| `0x08FA97A0` | Current projection matrix | seen |

### The camera object and its look-at update (write watch, 2026-10-22)

`PSP2I_VR_PROBE_WATCH=camera+0x40` (F8 finds the active camera through the slot
table). In a field the active camera was index 0, object **`0x0942FE60`**
(heap; the address may change by area; the `0x0942FE70` hit in the first
search was its +0x10). All 32 writes to camera +0x40 had one stack:

`0x08AFDC28` → `0x08AFDBD0` → **`0x08AFD3FC`** → **`0x08AFCE90`** → **`0x08D8969C`** → `0x08D89AF0` / `0x08D7AED4`

(from `0x08AFCE6C`, the same frame path as the SetViewMatrix calls.)

| Address / field | What | Status |
|---|---|---|
| `0x08AFD3FC` | **UpdateCamera(cam)**: if cam+0x130 & 1, rebuild the view (`0x08AFCE90`); if & 2, the projection (`0x08AFCF3C`(cam+0x90, cam+0xE8, cam+0x118, +0x11C, +0x120)); then clear +0x130 | seen (decoded) |
| `0x08AFCE90` | Build the view: LookAt(cam+0x10, cam+0xE8) (`0x08D8969C`), inverse into cam+0x50 (`0x08D7AE5C`), then two direction vectors into cam+0xD0 and cam+0xDC | seen (decoded) |
| `0x08D8969C` | **LookAt(out, p)**: direction = p+0x14 − p+0x20 (eye − target), normalised; up from it; roll = −(p+0x2C) about Z (`0x08D7AA98`); result multiplied into `out` | seen (decoded) |
| camera +0x10 | View matrix (world to eye), 4x4 | seen (dump matches) |
| camera +0x50 | Its inverse (the camera's world transform; +0x80 = eye position) | seen (dump: +0x80 = eye) |
| camera +0x90 | Projection 4x4 (fovy 45°, aspect 1.765, the same as the GE PROJ) | seen (dump) |
| camera +0xD0, +0xDC | Direction vectors derived from the view (likely forward and up) | seen |
| camera +0xE8 | Start of the look-at parameter block `p` | seen (decoded) |
| camera +0xF0, +0xF4, +0xF8 | Aspect 1.7647, near 1.0, far 30000 (from the dump; +0xF4/+0xF8 are the pair `0x08D7D8E0` gets) | seen |
| **camera +0xFC** | **Eye position** (x, y, z), world (dump: -58.51, matching +0x80) | seen |
| **camera +0x108** | **Target (look-at point)**, world | seen (decoded; value not dumped yet) |
| camera +0x114 | Roll angle | seen (decoded) |
| camera +0x130 | Dirty flags: 1 = view, 2 = projection | seen (decoded) |

So the game's camera is eye + target + roll. For VR: P2's stereo can offset the
eye per eye after LookAt (or hook SetViewMatrix); P3/P4 head tracking can
either write eye/target and set +0x130 |= 1, or replace the LookAt result in
`0x08AFCE90`.

### The orbit controller (write watch on camera +0xFC, 2026-10-22)

`PSP2I_VR_PROBE_WATCH=camera+0xFC`, F8 in a field, then the right stick. All 32
writes had one stack, and the eye x moved smoothly with the stick (-24.8 →
+18.8):

`0x08AF8250` → `0x08AF962C` → `0x08AF986C` → `0x08AF98C0` → `0x08AF9BCC` → `0x08AFA714` → `0x08AFA8E0` → **`0x08AFA598`** → `0x08AFE0A0` → **`0x08AFD598`**

(from the game update `0x08A87988` → `0x089F8308`. The port's existing camera
hooks `0x08AFA530` and `0x08A16794` are in the same area.)

The F8 dump in that run: camera `0x0942FE60` again, eye +0xFC = (-24.82, 21.94,
-363.26) (= +0x80), target +0x108 = (-5.00, 14.37, -307.13), so the eye is
about 60 units behind and above the target. Roll +0x114 = 0. +0x118 = 0.6388,
+0x11C = 0.7694 (projection inputs, not decoded), +0x128 = +0x12C = 1.0.

| Address | What | Status |
|---|---|---|
| **`0x08AFA598`** | **OrbitCamera_Apply(ctrl `$a0`, eye* `$a1`, target* `$a2`)**: saves the camera's current eye to ctrl+0xB4 and target to ctrl+0xC0 (GetEye `0x08AFDEC8`, GetTarget `0x08AFDF30`), then SetEye (`0x08AFE0A0`) and the target (`0x08AF615C`), then a loop that calls `0x08AF4AC8` (listeners, table at `0x08E40F50`; not decoded). **The P3/P4 hook point**: change eye/target here for head tracking or first person | seen (decoded) |
| `0x08AFA8E0`, `0x08AFA714` | The callers that compute the eye and target (the orbit maths: yaw, pitch, distance around the player); not decoded | seen |
| ctrl +0x30 | The camera slot (slot+4 = camera object) | seen |
| ctrl +0xB4, +0xC0 | Previous eye, previous target | seen |
| `0x08AFE0A0` | SetEye on a slot: converts `$a1` (`0x08D3389C`), then `0x08AFD598`(slot+4) | seen |
| `0x08AFD598` | **Camera_SetEye(cam, vec3*)**: +0xFC..+0x104, +0x130 \|= 1 | seen |
| `0x08AFD5C0` | **Camera_SetTarget(cam, vec3*)**: +0x108..+0x110, +0x130 \|= 1 | seen |
| `0x08AFDEC8`, `0x08AFDF30` | GetEye / GetTarget of a slot (defaults when the slot has no camera; the target default is at `0x08E41020`) | seen |

### The frustum cull (decoded 2026-10-22; hooked for VR)

Found from the generated code: the functions that read the current
projection `0x08FA97A0` (lui/offset scan of `port/gen/recomp_funcs.c`).

| Address | What | Status |
|---|---|---|
| `0x08D7D63C` | **Build the cull planes**: from the projection's x and y scales (`0x08FA97A0` floats 0 and 5) and the viewport/scissor values at `0x08FA97E0` / `0x08FA9818`, writes four normalised view-space side planes (pairs of floats) to `0x08FA9760`..`0x08FA977F`. An orthographic projection (`0x08FA9830` == 1) takes a second path. Called only by SetProjection `0x08D7DA78` and the viewport setter `0x08D7D868`. **Hooked** (host/main.c `hook_frustum_planes`): with VR head tracking it runs with those two scales divided by `vr_cull_scale`, then the projection is put back | decoded, hooked; headset: no gaps noticed at 8x |
| `0x08FA9760` | The four side planes (view space), 8 floats | decoded |
| `0x08FA9818` | Near/far (and viewport) values the tests use | seen |
| `0x08FA9830` | 1 = the current projection is orthographic | decoded |
| `0x08D7CAEC` | Box (centre, axes, half sizes) against the planes: 16 = outside, 2 = inside, else crossing flags. Its neighbours `0x08D7C934`, `0x08D7CD88`, `0x08D7CE6C`, `0x08D7CF90` use the same planes (other shapes, not decoded) | decoded |
| `0x08D7DCE4` | From an integer angle: the floats at camera +0x118 / +0x11C (likely sin / cos of the FOV angle), called by the projection build `0x08AFCF3C` | seen |

### The HUD pass (GE dump, town, 2026-10-22)

After the 3D scene (into `0x0CC000`) and the orthographic bloom passes, the
game clears `0x0CC000` again (depth only, a 2-vertex through-mode clear) and
draws the HUD there: 176 draws, all flat perspective sheets (every vertex at
clip z −1, w a fixed 64-86). The labels (names, the reticle, location names)
are drawn the same way, already projected for the game camera on the CPU.
Last, one through-mode draw copies `0x0CC000` to the display buffer
`0x000000`.

## Needed for VR (todo)

| What | Phase | How to find it |
|---|---|---|
| The orbit maths (yaw, pitch, distance) in `0x08AFA8E0` / `0x08AFA714` | P3 / P4 | The apply point `0x08AFA598` is found (above); decode its callers, or hook `0x08AFA598` and leave them alone |
| LOD / map-sector culling, if any (the frustum cull is found, above) | P3 | Gaps or popping far to the side with `vr_cull_scale` high |
| Bloom / blur passes (signature to skip or redo per eye) | P2 | Redone per eye without a signature: P2 replays every draw per eye, and a render target sampled as a texture is sampled in the same eye's copy (d3d11.c `d3d_draw_eye`). A signature is still needed to switch bloom off |
| Local player yaw | P4 | Position found (player +0x4B0, above). Yaw: an angle or rotation near +0x4B0 that follows turning |
| Player skeleton: bone indices for head, neck, spine, hands, feet | P4 / P6 | Skinned draws of the local player: bone palette per batch; hide one bone's triangles at a time |
| Weapon attach (hand) bones and weapon model draws | P5 | Draws that follow the hand bone; the item / equipment code |
| Lock-on target | P4 | Write watch on what changes when L locks on |
| Aim direction (guns, techniques) | P5 | Free-aim mode: values that follow the right stick while aiming |
| Just Attack window | P5 | The attack-combo state: what the game checks to grant a Just Attack |
| Cutscene / camera-cut state | P2 | A flag or camera mode that changes on cuts and in event scenes |

## Workflow

1. Run a development build (not `PSP2I_PROD`) and go into the field.
2. Press **F8**. `logs/vr_probe.txt` gets the scene camera, the RAM hits for
   its position and view matrix, and the skinned models nearest the line of
   sight with the RAM hits for their positions.
3. Move or turn and press F8 again. Addresses whose values followed are the real
   ones; the others were copies or coincidences.
4. Confirm with `--watch ADDR` (backtraces of writers in the logs) or
   `--watch-from-flip ADDR FLIP`.
5. Record the result here, with its source.

`PSP2I_VR_PROBE_EVERY=N` prints the scene camera every N frames to the console.
