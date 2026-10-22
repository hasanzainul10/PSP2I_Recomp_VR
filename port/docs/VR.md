# VR

The game in a headset through OpenXR (Windows: Direct3D 11). Phase 1 is the
**theater**: the game's picture on a large screen in front of you, with the
motion controllers working as a PSP pad. Phase 2 makes that screen a stereo
3D window (below). Head tracking, first person and motion combat come in
later phases (see the VR plan).

## Turning it on

- Engine menu (F1, or right stick click on a VR controller) > **VR** > **VR: ON**.
  It takes effect at once and is saved in `psp2i_display.ini` (`vr=on`).
- Or for one run: `psp2i.exe --vr` (`--no-vr` to keep it off).

Needs `openxr_loader.dll` next to the exe (the build copies it) and an OpenXR
runtime with Direct3D 11: SteamVR, Meta Quest Link, Virtual Desktop (VDXR),
Pico Connect, WMR. The status shows on the VR page: NO OPENXR RUNTIME, NO
HEADSET (retried every few seconds; start the headset any time), WAITING,
RUNNING. Without a headset the game keeps running in its window as before.

## Settings (engine menu > VR)

| Item | What |
|---|---|
| VR | On or off, and the status |
| SCREEN DISTANCE | 1.0 to 5.0 m (default 2.5) |
| SCREEN WIDTH | 1.0 to 10.0 m (default 3.0) |
| RECENTER | Put the screen in front of you again |

The screen stays where it was put (world-locked), level and upright. Recenter
whenever you turn your chair.

## Stereo 3D (phase 2)

With the Direct3D 11 renderer the screen is a **stereo window**: the game is
drawn once for each eye, and the screen becomes a window into the game world
(things behind it look far away, close things come out of it). The HUD and
menus sit on the window itself. The window's width follows the game's field of
view at the screen distance (SCREEN WIDTH is then not used); move it closer
for a bigger window.

In `psp2i_display.ini` (no menu items yet):

| Setting | What |
|---|---|
| `vr_stereo=on` | `off` = the flat screen of phase 1 |
| `vr_world_scale=10.0` | Game units per metre. Higher = the world looks smaller and the 3D weaker; lower = bigger and stronger |
| `vr_head=on` | Head tracking (phase 3, below). `off` = the stereo window |
| `vr_level=on` | With head tracking: the horizon stays level with your floor (the game camera's up/down tilt is taken out). `off` = the game's tilt stays |

The IPD comes from the headset. OpenGL keeps the flat screen.

## Head tracking (phase 3)

With `vr_head=on` (the default) you see the game from your own head: the game
camera stays where the game puts it (the right stick still turns it), and your
head turns and moves on top of it, at the world scale. Recenter (left stick
click) makes the direction you face the game camera's again.

For now:

- You see the game's own field of view (about 72 x 45 degrees) in front of
  you. Around it the view is black, and it follows your head.
- Between game frames the headset turns the last picture with your head, so
  head turns stay smooth at 30 fps.
- The game only draws what its camera sees. Things outside its view are
  missing when you look far to the side.
- The HUD is drawn into the picture, so it stays in front of your eyes and
  looks far away.
- The engine menu stays on a screen in front of you.

## Controls

Positional, as on a PlayStation pad. **Circle/Cross** in the CONTROLS page
swaps them for VR controllers too.

| Controller | PSP |
|---|---|
| Left stick | Analog stick |
| Left stick with left grip held | D-pad |
| Right stick | Camera (as the right stick on a pad) |
| A / B (right) | Cross / Circle |
| X / Y (left) | Square / Triangle |
| Left / right trigger | L / R |
| Right grip | Select |
| Left menu button | Start |
| Left stick click | Recenter the screen |
| Right stick click | Engine menu |

Index: left A / B are Square / Triangle; Start is a firm squeeze of the left
trackpad. Vive wands have no face buttons: the trackpads are the sticks, and the
right trackpad click is Cross. Pico 4 uses the Touch layout.

A gamepad (SDL) still works next to the controllers: buttons from both count,
and the VR sticks win unless only the pad's stick is pushed.

## Testing without the game

`vr_test.exe` (built next to `psp2i.exe`) shows a test picture on the VR screen
and prints the controllers as PSP buttons:

```
vr_test.exe 30
```

SteamVR's null headset works for checking the session and frames without
wearing a headset.
