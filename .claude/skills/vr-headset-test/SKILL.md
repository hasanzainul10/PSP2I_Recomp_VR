---
name: vr-headset-test
description: Check the OpenXR / VR side of the PSP2i port (vr_test, the game with --vr, the VR status, controller mapping) on the user's PS VR2 through SteamVR or without a headset. Use when testing or debugging VR sessions, swapchains, bindings, or the theater screen.
---

# Testing VR

The user's setup: a PS VR2 through SteamVR (`ActiveRuntime` = SteamVR,
steamxr_win64.json). The Meta Link runtime (OVRServer) is installed but not
active. The PS VR2 Sense controllers also show up as SDL joysticks.

## Without the game

```bash
cd port/build/Release && timeout 90 ./vr_test.exe 40
```

It prints the status changes (`OFF` → `NO HEADSET` / `WAITING` / `RUNNING`) and
the controllers as PSP buttons (`pad: buttons XXXX left x,y right x,y host H`).

- **The first run can sit at OFF** while Steam / SteamVR starts (`xrCreateInstance`
  blocks). Run it again.
- **NO HEADSET:** the headset is off or not connected. The code retries every 3 s.
- **NO OPENXR RUNTIME:** there's no active runtime, or the runtime has no D3D11 support.

## In the game

Run with `--vr` (skill `psp2i-build-run`) and grep the log for `^vr:`. A healthy
start shows:
- `vr: runtime SteamVR/OpenXR ...`
- `vr: headset SteamVR/OpenXR : playstation_vr2`
- `vr: screen swapchain 960x544, 3 images`
- `vr: WAITING`, then `vr: RUNNING`

At exit you should see `vr: OFF`.

**What only the user can check, in the headset:**
- screen size and distance, sharpness, colours (sRGB);
- recenter (left stick click);
- the button mapping (see `port/docs/VR.md`).

Ask them to check, and report what they say. Don't claim it works from the logs alone.

## Headset-free

SteamVR's null driver gives a fake headset. Switching it on edits the user's
SteamVR settings, so ask first. Controller input can't be scripted that way.
Use `--press` / `--stick` for game input instead.

## Where the code is

- **Session, swapchain, bindings:** `port/vr/vr_xr_d3d11.c`.
  - Bindings are in `actions_create`.
  - The pad is built in `input_update`.
- **The game side:** `port/host/main.c`, functions `vr_apply`, `poll_controllers`,
  `update_camera_stick`, and the `vr_submit_frame` call in `on_vblank`.
- **Settings:** the engine menu's VR page (`port/host/menu.c`).
