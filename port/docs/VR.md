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
| HUD DISTANCE | With head tracking: the HUD panel's distance, 1.0 to 5.0 m (default 2.5) |
| WORLD SIZE | With head tracking: the world's size as a percentage of life size, 20 to 300%, for the view you're in (1ST / 3RD person; first person starts at 100%, third at 34%) |
| RESOLUTION | The render scale, 1X to 6X (the eye pictures are 480 times it wide): higher is sharper and slower; from the next start (RESTART shows until then) |
| SHARPEN | Sharpening of the headset's pictures, OFF or 10 to 100% (default 50%); at once |
| TEXTURE FILTER | OFF, 2X, 4X, 8X or 16X anisotropic (default 16X); at once, but mipmaps need a start with it on (RESTART shows then) |
| GLOW | the game's glow (bloom) in the headset, ON or OFF (default OFF: with it on, glows can sit off what glows); at once |
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
| `vr_fov_scale=1.6` | With head tracking: the view is that many times the game's across (1.0 to 3.0). Wider = less sharp |
| `vr_fov_scale_y=2.6` | The same up and down (1.0 to 4.0). The picture keeps the PSP's shape, so this makes its pixels taller |
| `vr_cull_scale=8.0` | With head tracking: the game draws objects that much further out to the sides (1 = the game's own culling). Lower it if the frame rate drops |

The IPD comes from the headset. OpenGL keeps the flat screen.

## Head tracking (phase 3)

With `vr_head=on` (the default) you see the game from your own head: the game
camera stays where the game puts it (the right stick still turns it), and your
head turns and moves on top of it, at the world scale. Recenter (left stick
click) makes the direction you face the game camera's again.

For now:

- You see about 99 x 94 degrees (`vr_fov_scale` / `_y`). Past that the view
  is black, and that edge follows your head. Raise `render_scale` if the
  picture looks pixelated (it costs GPU time).
- Between game frames the headset turns the last picture with your head, so
  head turns stay smooth at 30 fps.
- The game's culling is widened (`vr_cull_scale`), so what you look at is
  drawn.
- Names, the lock-on reticle and location labels stay on what they label.
  The HUD (HP, the palette, menus) stays in front of the game camera, about
  7 m away: in front of you until you turn the camera.
- The engine menu stays on a screen in front of you.

## Controls

Two input modes, engine menu > VR > **INPUT MODE**. Face buttons are
positional, as on a PlayStation pad, in both; **Circle/Cross** in the
CONTROLS page swaps them for VR controllers too.

### MOTION (as PSUEx-VR)

Your hands are shown where the controllers are (a ring and a short ray),
and your character's arms reach for them, in first and third person
(`vr_arms=on`, the default; `off` keeps the game's own arm animation). Only
how your character is drawn on your side changes: other players see the
game's animation.

| Control | Left hand | Right hand |
|---|---|---|
| Stick | Move (in manual aim: aim) | Camera / turn |
| Stick click | Manual aim (the game's L + D-pad down) | Start |
| Face buttons | Square / Triangle | Cross / Circle |
| Swing, or pull the trigger | Attack (Square) | Attack (Square) |
| Grip held + swing / trigger | Photon Art (Triangle) | Photon Art (Triangle) |
| Grip alone | R (lock-on) | L |
| Menu button | Start | |

A grip counts as R or L after a moment (0.15 s) held without that hand
attacking; a grip used for a Photon Art sends nothing until you let go.
Swings count above 1.8 m/s (`vr_swing_speed=`).

**Hold both grips** for the VR shortcuts: + Cross recenter, + Circle first /
third person, + Square engine menu, + left stick click switches the left
stick to the D-pad (again to switch back). Left grip + left stick click saves
a VR capture (dev builds only).

### PAD

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
| Left grip + left stick click | VR capture, like F11 (dev builds only) |
| Right stick click | Engine menu |

In first person the left stick walks where your head looks, and the right
stick turns the view; recenter makes the game camera's direction ahead again.

Index: left A / B are Square / Triangle; Start is a firm squeeze of the left
trackpad. Vive wands have no face buttons: the trackpads are the sticks, and the
right trackpad click is Cross. Pico 4 uses the Touch layout.

A gamepad (SDL) still works next to the controllers: buttons from both count,
and the VR sticks win unless only the pad's stick is pushed. With a gamepad
alone in VR: R3 or the Guide / PS button opens the engine menu, L3 tapped
recenters, and L3 held for a second saves a VR capture (dev builds only).

## Testing without the game

`vr_test.exe` (built next to `psp2i.exe`) shows a test picture on the VR screen
and prints the controllers as PSP buttons:

```
vr_test.exe 30
```

SteamVR's null headset works for checking the session and frames without
wearing a headset.
