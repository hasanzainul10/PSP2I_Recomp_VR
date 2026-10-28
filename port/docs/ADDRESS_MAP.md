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
| player +0x70..+0xAF | **The player's facing**: a 4x4 matrix, rows x (+0x70), y (+0x80, always 0,1,0), z = forward (+0x90), then a position (+0xA0). Facing = `atan2(+0x90, +0x98)` (the same atan2(x, z) as a move); a copy of the rotation at +0xB0..+0xDF. Found in a per-frame player trace (`PSP2I_PL_TRACE`, 2026-10-23, `pltrace1.bin`): within 1 degree of the walk direction in 90% of steady walking frames. The stick turns it 20 degrees a game frame, starting 1-3 frames after the push; letting the stick go mid-turn snaps it to the stick's heading a frame later; Square / Triangle with no stick don't turn it, and a stick pushed during an attack's start doesn't either. Read by the attack nudge (host/main.c `fp_nudge`) | known (trace + headset) |
| player +0x214, +0x7E0, +0x7E4 | 2 while R is held (lock-on) and ~8 frames after it is let go, else 0 (the trace, two R holds). During it Square combos turned the facing a few degrees (towards the target); without it never. `fp_nudge` skips while it is set | seen (trace) |
| player +0x87C | Counts 0..3 while L is held, 0xFA otherwise (the trace); not used | seen |
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
| `0x08FA9818` | Near/far (and viewport) values the tests use: `0x08FA9818` far, `0x08FA981C` near (0x08D7CAEC: outside if the box is entirely nearer than near, which includes behind the camera, or beyond far) | decoded |
| `0x08D7C934`, `0x08D7CAEC`, `0x08D7CD88`, `0x08D7CE6C`, `0x08D7CF90` | The five frustum tests (one caller each, through `0x08D7CD48` / `0x08D7D1A4` / `0x08D7D1E4` / `0x08D7D150`); all return 16 = outside, 2 = inside, else crossing flags. **Hooked** (host/main.c `hook_cull_test`): with VR head tracking and `vr_cull_all` they answer 2 without running, so every loaded object is drawn | decoded, hooked; not yet seen in the headset |
| `0x08FA9830` | 1 = the current projection is orthographic | decoded |
| `0x08D7CAEC` | Box (centre, axes, half sizes) against the planes: 16 = outside, 2 = inside, else crossing flags. Its neighbours `0x08D7C934`, `0x08D7CD88`, `0x08D7CE6C`, `0x08D7CF90` use the same planes (other shapes, not decoded) | decoded |
| `0x08D7DCE4` | From an integer angle: the floats at camera +0x118 / +0x11C (likely sin / cos of the FOV angle), called by the projection build `0x08AFCF3C` | seen |

### Drawing less: draw distance and sprites (3DS quality options, 2026-10-27)

Found for the 3DS port's quality options (3DS_PROGRESS.md, handoff
2026-10-28); hooked in `port/host/3ds_hooks.c` behind switch files.

| Address | What | Status |
|---|---|---|
| `0x08D7D8E0` | **SetNearFar(near $f12, far $f13)**: the frustum tests' near / far (`0x08FA981C` / `0x08FA9818`, copies at `0x08FA9784` / `0x08FA9780`). Called by ApplyCamera `0x08AFD474` (cam+244 near, cam+248 far), `0x08AFD500` and `0x089EFE5C`. Far: 30,000 for the field camera, 10,000 for the title / menu one (Azahar log) | disassembled, hooked (3DS); scaling far changed nothing in field rooms, even at 2% |
| `0x08D7A4CC` | **Draw a sprite array(prim, vertices, count)**: the vertex format by the mode at `0x08EE229C` (jump table `0x08E8614C`); the `0x19F` path copies 24-byte vertices into the list (`0x08D7A6D0` / `0x08D7A6F0`) and draws with vtype 415 (`0x08D7A56C`) | disassembled |
| `0x08D7A74C`, `0x08D7A80C` | Its two wrappers, called from all over the game (the library's draw calls) | hooked (3DS, `fx_trace`: callers counted) |
| `0x08A07C30` | A draw method (in a table at `0x08E3182C`), prim at obj+47: **the minimap's player and enemy markers** (skipped: they vanished, the user saw); ~200 sprite draws a second in the field | hooked, skip seen by the user |
| `0x08967FB8` | An object's render: needs `*(0x08ED2BAC)` and obj+72 != -1, builds an 18x18 grid of quads into `*(0x08EBA45C)`, draws obj+68 of them through `0x089682E4` (sprite mode 2, one quad a call, from obj+64); returns 1 at its end, 0 when there is nothing to draw (returning 0 from a skip stalled the game). Called by `0x08967B50`. Its sprite calls rise in fights, but only 6-14 a second (4 vertices each) | hooked; **not the effects** (Azahar, 2026-10-28: skipped on / off every 6 s in a field fight, draws a frame didn't follow the phases and the user saw nothing missing) |
| `0x08D94330` | **sceGuDrawArray(prim $a0, vtype $a1, count $a2, indices $a3, vertices $t0)**, 9 direct callers (`0x08D7A56C` among them) | hooked (3DS, `fx_trace`: draws per caller and vtype). Field fight (Azahar, 2026-10-28), per 10 s: `0x08D79E00` (in `0x08D79CE4`) 4,000-7,500 x `0x19F` (4 vertices) + ~1,200 x `0x19C`; `0x08D7A574` (sprite arrays) ~2,500 x `0x19C`; `0x08AD3D68` / `0x08AD3FE0` 50-230 x `0x19F`, 30-100 vertices each (strips). Under a fifth of the field's ~500 draws a frame: the rest (scenery, skinned) doesn't come through here |
| `0x08D79CE4` | **Draw a quad array(prim $s2, vertices $s0, count $s1)**, the twin of `0x08D7A4CC`: format by the mode at `0x08EE2284` (2: 20-byte vertices copied to 24 with the colour at `0x08EE2298`, vtype 415 = `0x19F`). Where the effect quads come from | disassembled |
| `0x08D79F8C`, `0x08D7A080` | Its two wrappers (its only callers). Field fight, per 10 s: `0x08D3ED58` (in `0x08D3ED40`) 2,000-11,000 mode 2, also at the title and loading; `0x089BBE24` (in `0x089BBC30`) 1,800-4,400 mode 2, field only; `0x08D3EDB4` 500-1,500 mode 1; the rest under 700 each | hooked (3DS, `fx_trace`: callers counted) |
| `0x08D3ED40` | A wrapper: `$f12` from `0x08EBA454`, then `0x08D79F8C` with a0 = 1. Only called through pointers; nearly all from `0x08D4A66C` (unlooked, in the 2D / UI library range) | hooked (3DS, `fx_trace`) |
| `0x089BBC30` | **The HUD's party panels' frames** (each member's name / HP / PP box): builds a quad array and draws it through `0x08D79F8C` as its last act. Only called through pointers, from `0x089BB584` .. `0x089BC82C` (the panel drawing). Skipped: the frames blinked, the bars and the name text stayed (the user saw, Azahar 2026-10-28) | hooked, skip seen; HUD, so not a quality option |
| | **Note**: a hook address must be a function entry as the recompiler found it (`psp_func_<addr>` in gen); any other address is ignored without a word. `0x089BBC30` / `0x08D3ED40` start two instructions before their stack adjust | |
| `0x08AD9E68` | A third sprite-array caller, occasional (mode 4) | seen in the count only |

### The HUD pass (GE dump, town, 2026-10-22)

After the 3D scene (into `0x0CC000`) and the orthographic bloom passes, the
game clears `0x0CC000` again (depth only, a 2-vertex through-mode clear) and
draws the HUD there: 176 draws, all flat perspective sheets (every vertex at
clip z −1, w a fixed 64-86). The labels (names, the reticle, location names)
are drawn the same way, already projected for the game camera on the CPU.
Last, one through-mode draw copies `0x0CC000` to the display buffer
`0x000000`.

- **The minimap's window** (GE dumps and VR captures, 2026-10-23): the
  minimap's 3D map is drawn in the HUD pass clipped to scissor
  `403,6-473,76`, the only sub-screen scissor on `0x0CC000` in 58 dumps
  (town, field, Ruins). Its clip w is not one layer: it follows the area's
  map (above 2300 in town; −1035 to 883 in the Ruins), so psprecomp's
  `stereo_classify` sends any HUD-pass draw clipped to a window to the HUD
  panel, whatever its w. Status: fix built, waiting for the headset.
- **Effect sprites carry their target's depth** (GE dump `draws_011461`,
  2026-10-23): an enemy's glow is small orthographic sprites (w 1, vt
  `0x00019F`, tex `0x087B68C0`) in the scene pass whose depth value is the
  enemy's (screen z 41223 against the body's 41264 at clip w 106). Read
  through the latest scene draw's depth row (that projection's z row is
  about −0.001, −14 for models, not the first scene draw's −0.0067, −201),
  it gives the body's distance; psprecomp `fx_vertex` places effects there
  instead of at a fixed `fx_dist`. Status: fix built, waiting for the
  headset.
- **The minimap's camera** (dev apply log, `f1_nudge1.log`, 2026-10-23):
  a field frame applies (OrbitCamera_Apply's hook point `FN_APPLY_CAMERA`)
  `[cam 09557DA0]`, the scene walk, `[cam 094AFD40]` three times, then
  `[cam 09557DA0]` again. Both have projection x 1.368 (+0x90). The second
  is the minimap's (with it moved to the head in first person, the map
  turned into a flat eye-level view). Pointers are per session; told apart
  by order: the frame's first field camera is the field camera. Status:
  inferred from the log and the map's change; fix built, not yet confirmed.

### The frame's clears (psprecomp reports, 2026-10-24)

The 3D target `0x0CC000` is cleared at the frame's start, then (in the field)
by a **depth-only** clear (bits 4 of GE `CLEAR`: 1 colour, 2 alpha /
stencil, 4 depth) that starts the HUD pass, 150 of 150 frames in a 5 s
window. Menus and loading screens clear it fully (7). An attack effect
clears colour and alpha (3) mid-scene, after ~40 scene draws. The glow
pass draws its 3D parts into `0x154000` after the scene, then shrinks and
blurs through `0x000000` / `0x022000` and adds the result over the scene
from `0x04000000` (256 x 128). Big attack flashes are orthographic sprites
of up to ~300 x 270 starting mid-screen; bloom / copy passes start at 0,0.

### The data decoder (gdepth on the PC build, town replay, 2026-10-25)

`0x08DE0C40` is the inflate routine (allegrexrecomp's name for it; in the
walk it was entered at `0x08DE0C68` from `0x08D5DE08` <- `0x08D5DE64` <-
`0x08D57620`, a loader, on `user_main`; the bit decoder it
calls with `bltzal` exits through its epilogue, see allegrexrecomp's
`emit_return_check`). Its literal loop is split across bodies by discovery:
`0x08DE0E10` (decode a symbol, `jal 0x08DE0FAC`), falling into
`0x08DE0E0C` (owned by `0x08DE0EAC`: store the byte), back to `0x08DE0E10`;
copies run through `0x08DE0E50` / `0x08DE0E88` / `0x08DE0EAC`. Before the
recompiler fix (psprecomp `fall_target`), the fall into `0x08DE0E0C` was a
C call per byte: 177,000 nested calls (16 MB of host stack) on PC in town,
and the 3DS's 4 MB `user_main` stack overflowed. Status: seen (stack walk).

### Menus and tasks (menu finder, write watch, task log, 2026-10-23)

For the left stick as the D-pad in menus. The game's own "a menu is open"
is not found yet; what is known:

| Address | What | Status |
|---|---|---|
| `0x08EE2F70` | The menus' cursor pulse counter: 0..0x3F one step per game frame, wrapping, pausing at 0 up to ~22 vblanks; runs in the title menu, start menu, shops, salon, palette; 0 in the field. Finishes its cycle 1-2 s after a menu closes; one menu left it at 0 for 20 s while open. Likely +0x510 of a structure at `0x08EE2A60` (`0x088ADC34` / `0x088ADDC4` build that base). Used for `vr_menu_dpad` | seen (menu finder + change log) |
| `0x08DA0DC4` < `0x08DA1264` < `0x08D55690` < the main update `0x08B821B8` | Writes the counter each frame (UI animation tick) | seen (write watch) |
| `0x08DA0FB8` < `0x08DA1194` < `0x08D5F900` < `0x08D5EE58` < `0x089D172C` < ... < `0x089D06E8` | Starts the counter (writes 1) for the title / continue menu | seen (write watch) |
| `0x089F7A64` / `0x089F7A48` | The scene dispatcher: walks a tree of tasks (children +20, next +12, vtable +0, update = vtable +8, flags +4: update skipped while flags & 0x1B) | seen (decoded) |
| task updates `0x089D06E8`, `0x089F0BBC`, `0x089DB0AC` | The title / continue menu's tasks: they exist only while that screen is up | seen (task log) |
| field menus (start menu, palette) | Add no tasks: a state inside always-running tasks, not mapped | open |

### The player's skeleton (disassembly, write watch, PSP2I_SKEL dumps, 2026-10-23)

For VR arms (M2) and the body (P6). Found from the GE side down: the bone
commands, who patches them, who writes the matrices they come from. Three
windowed runs in town with party members (`port/testwork/skel/skel1..3`),
analysed with `.claude/skills/psp2i-static-re/scripts/skeleton.py`.

| Address | What | Status |
|---|---|---|
| `0x08D8D320` / `0x08D8D3B8` / `0x08D8D450` | Copy one 4x4 matrix (64 bytes) into 12 GE bone / world / texture-matrix commands (`0x2B` / `0x3B` / `0x41`) | known (disassembly) |
| `0x08D8D4E8`(model) | Per model, just before it is drawn: its matrices (+8 of them, array at +64[part], times the scale at +24) into the bone records of its prebuilt display list (+68[part], 52 bytes: 12 commands + RET); +72 / +76 the same for world and texture matrices. Scheduled through 0x08D8D7B0. The objects live in main RAM below `0x08800000` (e.g. `0x085F1B70`). **Hooked** (vr_body.c `hook_bone_patch`): the weapon's matrices are moved with the IK'd hand there | known (disassembly + hook, ~39 calls a frame) |
| `0x08D74E54` / `0x08D74E84`(palette, skeleton, nodes, M) | The skinning palette: for each of the skeleton's +40 nodes (144-byte records at +48), `palette[slot] = M x node x inverse bind` (slot = record +4 as a short, -1 = no slot; inverse bind at record +48 unless flag 0x8) | known (disassembly + write watch on a palette entry) |
| `0x08A846DC`(model, &palette, skeleton) | Calls the above with nodes = `*(model+100)`, M = model+32 (model -> eye). **Hooked** (port/vr/vr_body.c, dev `PSP2I_SKEL`): the place to change the local player's pose for IK | known (disassembly + hook) |
| `0x08A8658C`(model, skeleton) | One animation step: samples the motion (animation player at model+152, `0x08A7E270`) into a local pose buffer `*(0x08ED8A58)` (48 bytes a node), runs the model's optional `*(model+108)` callback on it, then `0x08A850A8` | seen (disassembly, 2026-10-24) |
| `0x08A850A8`(model, skeleton, local pose) | Evaluates the animation into `*(model+100)` (through `0x08A846B8` -> `0x08D74BD8`), then calls the model's callback `*(model+112)`(nodes) if set (0 for the player) | seen (disassembly) |
| `0x08D74BD8`(nodes, skeleton, local pose, a3) | Composes each node from the local pose: +0 translation, +16 rotation (`0x08D7AF4C`), +32 scale (`0x08D7B74C`), on the parent's node (record +6); record flags pick special cases (bits 13-16, 0x1000, 0x4/0x8/0x10 << 16) | seen (disassembly, 2026-10-24) |
| bind pose | `inverse(record +48)` per node: model space, unscaled, an upright **A-pose** (arms ~45 degrees down). The animated nodes' column lengths equal the records' +36 scales (0.80-0.94, not inherited); offsets scale with the parent (the hips 9.78 in the bind, 8.96 animated). Used by vr_body.c's avatar as the rest pose | seen (skel4 dump, 2026-10-24) |
| `*(player+0x178)+0x8` | **The local player's animated model** (in all three runs, town, with party members; the address changes per area) | seen (hook, 3 runs) |
| player model `+100` | Its node matrices: 28 x 4x4 (64 bytes, VFPU columns), model space, each bone along its local +x | seen (dumps) |
| skeleton record (144 bytes) | +0 flags, +4 palette slot (short) and parent (short at +6), +8 first child (short), +10 next sibling, +12..+20 rest offset from the parent (floats), +24..+32 rest rotation (16-bit angles), +36..+44 scale, +48 inverse bind 4x4 | seen (dumps; parents agree with the poses) |

**The weapon** (the arms' hook, 2026-10-24): not part of the body's model
and not the container's +0x10 model (that rides on the head, node 13).
Its matrices reach `0x08D8D4E8` like every model's; while R is held
(player +0x214 != 0) they are rigid to hand node 18 (left) or 23 (right),
0.0-0.1 units from it. Without R the game puts it elsewhere (a gun lowered
or holstered). Found by rigidity per node, as vr_body.c does at run time.

**The avatar** (vr_body.c, 2026-10-24, headset-checked): the upper body
(9-26) is drawn in the bind pose (rotation against the parent's from the
bind, the spine's against the root, the animated offsets and scales), the
whole body turned about the root to the gaze; the legs stay animated, or a
learned R-strafe clip (nodes 1-8 in the root's frame, a gait cycle found by
self-similarity of the ankles). In third person, what the game hangs on the
animated head (the rigid head mesh at the GE, `psp_gpu_set_rigid_move`;
bone matrices within 3 units of node 13 in `0x08D8D4E8`) is moved by the
head node's change. A held weapon is drawn on the posed hand node x its
grip, fixed the first time it sits still there.

**The head, for first person** (VR captures and GE dumps with each draw's
bones, 2026-10-24): hair and accessories are body meshes (texture
`0x0935A020`) on nodes 12 / 13 only; the face is body meshes (texture
`0x09353F20`, 4 weights) blended between 12 / 13 and the chest (10) and a
collarbone (14 / 19); the player's container (+0x10) holds an 8-node
accessory model ~2.5 units from node 13; the head itself is a rigid mesh
(texture `0x09358BE0`, vertex type `0x121`, no weights, 141 + 80 + 80 + 36
vertices) whose world matrix none of `0x08D74E54`'s callers builds (its
source not found; dropped at the GE by position instead).

| Address | What | Status |
|---|---|---|
| `0x08D74E54` callers | `0x08A846DC` (M = model+32), `0x08A86AF4` (M in `$s3`), `0x08ADE47C` (M built on the stack), `0x08AA0FD8` / `0x08AA1150` (no M: model space). **Hooked** at `0x08D74E54` (vr_body.c `hook_palette_any`) | known (disassembly + hook) |

The tree (node <- parent, rest length in model units; the head is ~13.3
above the feet, so ~7.8 model units per metre for a 1.7 m character; the
model+32 matrix scales it into the world). Sides from the poses: model +z
is forward, so +x is the character's left: **confirmed in the headset** (node 15
turned the left arm).

| Nodes | What |
|---|---|
| 0, 27 | root (feet), a second root child (locator) |
| 1, 2 | hips (9.78 up), pelvis |
| 3 <- 4 <- 5 / 6 <- 7 <- 8 | left / right leg: hip (0.91 to the side), knee (thigh 3.91), ankle (shin 4.32) |
| 9, 10, 11, 26 | spine (1.39), chest (1.51), upper chest (2.32); 26 a waist piece on 9 |
| 12, 13 | neck (at 11), head (1.07) |
| 14 <- 15 <- 16 <- 17 <- 18 | **left arm:** clavicle (0.32 from 11), shoulder (1.36), elbow (upper arm 2.56), wrist (forearm 2.56), hand end (0.65) |
| 19 <- 20 <- 21 <- 22 <- 23 | **right arm:** the same, mirrored |
| 24, 25 | chest pieces on 10 (both sides) |

Palette slots: only 15 of the 28 nodes skin this model (1, 3, 6, 9, 10,
12-16, 19-21, 24-26). The knees, ankles, wrists and hands (4, 5, 7, 8, 17,
18, 22, 23) have none: the lower legs and the hands are other models (the
character's parts: the first run's 14- and 16-bone models near the player).
**The parts share the nodes** (parts log and the shoulder test in the
headset, 2026-10-24): the player's model goes through `0x08A846DC` three
times a frame, with three 28-node skeletons (`0x09B30D80` the body,
`0x09B3B7C0`, `0x09B37450`: the other meshes, each with its own palette
slots), all reading the same node matrices. Turning node 15 in each call
(put back after it) moved the **left** arm, with the hand and the weapon
(user). So one pose change, applied in each of the model's calls, moves
the whole character. The container `*(player+0x178)` also holds, at +0x10,
an 8-node model with its own nodes (skeleton `0x09B3F330`), ~14.5 units
from the body: not attached to the arm, not identified.

## Needed for VR (todo)

| What | Phase | How to find it |
|---|---|---|
| The orbit maths (yaw, pitch, distance) in `0x08AFA8E0` / `0x08AFA714` | P3 / P4 | The apply point `0x08AFA598` is found (above); decode its callers, or hook `0x08AFA598` and leave them alone |
| LOD / map-sector culling, if any (the frustum cull is found, above) | P3 | Gaps or popping far to the side with `vr_cull_scale` high |
| Bloom / blur passes (signature to skip or redo per eye) | P2 | Redone per eye without a signature: P2 replays every draw per eye, and a render target sampled as a texture is sampled in the same eye's copy (d3d11.c `d3d_draw_eye`). A signature is still needed to switch bloom off |
| Player skeleton: bone indices for head, neck, spine, hands, feet | P4 / P6 | **Found** ("The player's skeleton" above), left / right confirmed in the headset |
| Weapon attach (hand) bones and weapon model draws | P5 | Draws that follow the hand bone; the item / equipment code |
| Lock-on target | P4 | Whether lock-on is on is found (player +0x214, above, set by R); the target object is not |
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
