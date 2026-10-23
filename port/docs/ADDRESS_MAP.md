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
| `0x08D8D4E8`(model) | Per model, just before it is drawn: its matrices (+8 of them, array at +64[part], times the scale at +24) into the bone records of its prebuilt display list (+68[part], 52 bytes: 12 commands + RET); +72 / +76 the same for world and texture matrices. Scheduled through 0x08D8D7B0 | known (disassembly) |
| `0x08D74E54` / `0x08D74E84`(palette, skeleton, nodes, M) | The skinning palette: for each of the skeleton's +40 nodes (144-byte records at +48), `palette[slot] = M x node x inverse bind` (slot = record +4 as a short, -1 = no slot; inverse bind at record +48 unless flag 0x8) | known (disassembly + write watch on a palette entry) |
| `0x08A846DC`(model, &palette, skeleton) | Calls the above with nodes = `*(model+100)`, M = model+32 (model -> eye). **Hooked** (port/vr/vr_body.c, dev `PSP2I_SKEL`): the place to change the local player's pose for IK | known (disassembly + hook) |
| `0x08A850A8`(model) | Evaluates the animation into `*(model+100)` (through `0x08A846B8` -> `0x08D74BD8`), then calls the model's callback `*(model+112)`(nodes) if set (0 for the player) | seen (disassembly) |
| `*(player+0x178)+0x8` | **The local player's animated model** (in all three runs, town, with party members; the address changes per area) | seen (hook, 3 runs) |
| player model `+100` | Its node matrices: 28 x 4x4 (64 bytes, VFPU columns), model space, each bone along its local +x | seen (dumps) |
| skeleton record (144 bytes) | +0 flags, +4 palette slot (short) and parent (short at +6), +8 first child (short), +10 next sibling, +12..+20 rest offset from the parent (floats), +24..+32 rest rotation (16-bit angles), +36..+44 scale, +48 inverse bind 4x4 | seen (dumps; parents agree with the poses) |

The tree (node <- parent, rest length in model units; the head is ~13.3
above the feet, so ~7.8 model units per metre for a 1.7 m character; the
model+32 matrix scales it into the world). Sides from the poses: model +z
is forward, so +x is the character's left; not yet checked in the headset.

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
Open for IK: whether those parts read this model's nodes (then changing
`*(model+100)` moves them too) or have their own (then they need the same
change), and where the weapon hangs.

## Needed for VR (todo)

| What | Phase | How to find it |
|---|---|---|
| The orbit maths (yaw, pitch, distance) in `0x08AFA8E0` / `0x08AFA714` | P3 / P4 | The apply point `0x08AFA598` is found (above); decode its callers, or hook `0x08AFA598` and leave them alone |
| LOD / map-sector culling, if any (the frustum cull is found, above) | P3 | Gaps or popping far to the side with `vr_cull_scale` high |
| Bloom / blur passes (signature to skip or redo per eye) | P2 | Redone per eye without a signature: P2 replays every draw per eye, and a render target sampled as a texture is sampled in the same eye's copy (d3d11.c `d3d_draw_eye`). A signature is still needed to switch bloom off |
| Player skeleton: bone indices for head, neck, spine, hands, feet | P4 / P6 | **Found** ("The player's skeleton" above); left/right still to confirm in the headset |
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
