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
| player +0x4B0 | **The player's world position** (x, y, z floats). The view matrix maps it to 3.05 units from the first bone (the hips are likely above the feet) | seen (2 reports) |

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

Next: watch camera +0x10's writer (the camera update: eye, target and up, or
yaw/pitch/distance), which is what VR head tracking and P3's first person
replace.

## Needed for VR (todo)

| What | Phase | How to find it |
|---|---|---|
| Camera object: eye, target, up, FOV | P2 | Layout started (+0x10 view, +0x90 proj, above). The eye/target or orbit parameters: the writer of camera +0x10 (`PSP2I_VR_PROBE_WATCH`) |
| Code that builds camera +0x10 from the orbit camera (to replace the camera, not just the matrix) | P2 / P3 | Write watch on camera +0x10 + 0x30; SetViewMatrix itself is found (`0x08D81D84`) |
| Frustum / LOD culling (objects pop at the headset's wider FOV) | P2 | Objects vanishing at the screen edges; the cull test reads the camera FOV or planes |
| Bloom / blur passes (signature to skip or redo per eye) | P2 | F12 dump: through-mode full-screen draws sampling a render target |
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
