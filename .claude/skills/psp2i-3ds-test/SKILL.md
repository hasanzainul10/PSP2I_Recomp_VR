---
name: psp2i-3ds-test
description: Run and check PSP2i 3DS builds - in the Azahar emulator (launch, install a .cia, read the log, read the app's report from the emulated SD, screenshot) and on the user's New 3DS XL with Luma3DS (3dslink, ftpd, FBI, Rosalina GDB). Use when testing a 3DS build, reading what it reported, measuring memory or speed, or when the user reports what they saw on the console.
---

# Test a 3DS build

Build first with `psp2i-3ds-build`. Then say **where** each result comes
from: "Azahar" and "console" are different claims.

| Question | Azahar | Console |
|---|---|---|
| Does it boot, draw, read files? | yes | yes |
| Memory mode / heap size from the exheader | yes (it applies the mode) | yes, the truth |
| Memory limit enforced? | **no** (P3P3DS saw it ignore the limit) | yes |
| CPU speed, fps | **no** (`ConfigureNew3DSCPU` is a stub, the clock reads ~268 MHz) | yes, the only source |
| Which core a thread ran on | **no** (`GetCurrentProcessorNumber` is a stub) | yes |
| Stereo depth on the real screen | side-by-side / anaglyph only | the user's eyes |

## Azahar

`D:\Tools\azahar\azahar-windows-msvc-2126.2\azahar.exe` (2126.2, New 3DS
mode by default). Data: `%APPDATA%\Azahar\` (`log\azahar_log.txt`,
`sdmc\` = the emulated SD card).

- **The user may be using Azahar too.** Check `Get-Process azahar` before
  starting it; never `Stop-Process` a window you didn't start in this command.
  Ask first when one is already open.
- `.3dsx`: `azahar.exe <path>.3dsx` (it runs it directly, `envhb=0`).
- `.cia`: install, then run the installed `.app`:

  ```powershell
  $az='D:\Tools\azahar\azahar-windows-msvc-2126.2\azahar.exe'
  Start-Process $az -ArgumentList '-i','<path>.cia' -Wait -NoNewWindow
  $app = (Get-ChildItem "$env:APPDATA\Azahar\sdmc\Nintendo 3DS" -Recurse -Filter *.app |
          Where-Object FullName -match '0f522000').FullName     # title ID low word
  $p = Start-Process $az -ArgumentList "`"$app`"" -PassThru
  ```

- Read what the app reported: `%APPDATA%\Azahar\sdmc\psp2i\report.txt`
  (the app writes it as it goes, so it's there while it runs).
- The log is written in full when Azahar exits; while it runs it holds
  stub and unimplemented-function errors (`Service <Error>`, `Kernel.SVC`).
  `svcOutputDebugString` lines appear there too.
- Screenshot: capture the window with System.Drawing (GetWindowRect +
  CopyFromScreen, see this session's history), or ask the user to look.
  Settings dialogs can cover the game.

## The console (New 3DS XL, Luma3DS)

The user does the steps on the console; ask, then read the results.

- **`.3dsx` over WiFi:** the user opens the Homebrew Launcher and presses
  **Y** (it waits for 3dslink). Then:

  ```bash
  /d/msys64/opt/devkitpro/tools/bin/3dslink.exe -a <console IP> port/3ds/build/psp2i_hello.3dsx
  ```

  Without `-a` it looks for the console by broadcast. Add `-s` to get the
  app's stdout back on this PC (with `link3dsStdio()` in the app).
- **`.cia`:** copy it to the SD card (ftpd over WiFi, or the card in a
  reader), then install with **FBI** (SD → the file → Install CIA) and start
  it from the HOME menu. Reinstalling over the same title ID replaces it.
- **ftpd** (on the console, port 5000): fetch `sdmc:/psp2i/report.txt`:

  ```bash
  curl -s ftp://<console IP>:5000/psp2i/report.txt
  ```

- **Rosalina** (L + Down + Select): process list, debugger (GDB stub on
  the console's IP, ports from 4000), screenshots to `sdmc:/luma/screenshots`.
  Connect with `/d/msys64/opt/devkitpro/devkitARM/bin/arm-none-eabi-gdb`
  and `target extended-remote <IP>:<port>`.

## The T0 check app (`port/3ds/hello`)

Prints and writes to `sdmc:/psp2i/report.txt`:

- `new3ds=` and `envhb=` (1 under the Homebrew Launcher);
- `app_mem=` the APPLICATION region: 96 MB for a `.3dsx` / normal title,
  124 MB for our `.cia` on New 3DS;
- `heap=` / `linear=`: what libctru took, the most the game can allocate;
- `cpu ~N MHz`: a timed loop; ~804 means the speedup works (console only);
- `core1=` / `core2=`: the core a test thread ran on, -1 if it couldn't
  start (console only). Core 2 should fail as a `.3dsx` and work as a `.cia`.
- The top screen shows the 3D slider in %.

Measured so far: `port/docs/3DS_PROGRESS.md`.
