---
name: psp2i-3ds-test
description: Run and check PSP2i 3DS builds - in the Azahar emulator (launch, install a .cia, read the log, read the app's report from the emulated SD, screenshot) and on the user's New 3DS XL with Luma3DS (3dslink, ftpd, FBI, Rosalina GDB). Use when testing a 3DS build, reading what it reported, measuring memory or speed, or when the user reports what they saw on the console.
---

# Test a 3DS build

Build first with `psp2i-3ds-build`. Then say **where** each result comes
from: "Azahar" and "console" are different claims.

**Test in Azahar by default** (the user's call, 2026-10-25): use the
console only when something can't be answered without it (real speed,
memory limits, cores, stereo on the real screen), and ask first. The game
files are on Azahar's emulated SD (`%APPDATA%\Azahar\sdmc\psp2i\`:
`game.iso`, `GameData/flash`, `GameData/ms` with the DLC). The console's SD
has the fonts, DLC and a game build, but **not the ISO**.

| Question | Azahar | Console |
|---|---|---|
| Does it boot, draw, read files? | yes | yes |
| Memory mode / heap size from the exheader | yes (it applies the mode) | yes, the truth |
| Memory limit enforced? | **no** (P3P3DS saw it ignore the limit) | yes |
| CPU speed, fps | **no** (`ConfigureNew3DSCPU` is a stub, the clock reads ~268 MHz) | yes, the only source |
| Which core a thread ran on | **no** (`GetCurrentProcessorNumber` is a stub; the work ratio is emulated) | yes |
| Stereo depth on the real screen | side-by-side / anaglyph only | the user's eyes |

## Azahar

`D:\Tools\azahar\azahar-windows-msvc-2126.2\azahar.exe` (2126.2, New 3DS
mode by default). Data: `%APPDATA%\Azahar\` (`log\azahar_log.txt`,
`sdmc\` = the emulated SD card).

- **The user plays along in Azahar.** Check `Get-Process azahar` first;
  if it's open, ask the user to load the new build (File > Load File).
  Start it without waiting (`Start-Process` and return), read the log in a
  later step (a background `until grep ...` loop), and **never stop
  Azahar from a script**, even one you started: the user may be playing.
- Game log while it runs: `%APPDATA%\Azahar\sdmc\psp2i\psp2i_log.txt`
  (flushed once a second). Azahar's own log (crash register dumps,
  "Exception Type"): `%APPDATA%\Azahar\logzahar_log.txt`; map a PC or LR
  with `arm-none-eabi-addr2line -f -e port/3ds/build/psp2i.elf <addr>`.
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
- **ftpd** (the user's console: `192.168.1.102:5000`, ask if it changed).
  The fastest loop, used for T0: the user runs ftpd, we upload, the user
  quits ftpd and runs the app, then runs ftpd again and we read the report.

  ```bash
  curl -sS -T port/3ds/build/psp2i_hello.3dsx ftp://<IP>:5000/3ds/    # HBL lists it
  curl -sS -T port/3ds/build/psp2i_hello.cia  ftp://<IP>:5000/cias/   # then FBI installs it
  curl -s ftp://<IP>:5000/psp2i/report_3dsx.txt                      # or report_cia.txt
  curl -s --list-only ftp://<IP>:5000/<dir>/                          # look before writing
  ```

  The SD card holds the user's own files: only add or replace files we put
  there, list a folder before writing to it, and never delete anything.
- **Rosalina** (L + Down + Select): process list, debugger (GDB stub on
  the console's IP, ports from 4000), screenshots to `sdmc:/luma/screenshots`.
  Connect with `/d/msys64/opt/devkitpro/devkitARM/bin/arm-none-eabi-gdb`
  and `target extended-remote <IP>:<port>`.

## The T0 check app (`port/3ds/hello`)

Prints and writes to `sdmc:/psp2i/report_3dsx.txt` (started by the Homebrew
Launcher) or `report_cia.txt` (anything else, including Azahar starting a
`.3dsx` directly):

- `new3ds=` and `envhb=` (1 under the Homebrew Launcher);
- `app_mem=` the APPLICATION region. Console (Luma3DS): 124 MB for both the
  `.3dsx` from HBL and the `.cia`. Azahar: 96 MB for a `.3dsx`, 124 MB for
  the `.cia`;
- `heap=` / `linear=`: what libctru took, the most the game can allocate;
- `clock:` the same loop timed with the speedup off and on; ratio ~3 =
  804 MHz works (console: 3.01; Azahar: 1.00, not modelled);
- `core0 alone` then `coreN: on cpu X, worker/alone R, main/alone M`: a
  worker on core N counts work while core 0 does the same. R ~1 = a parallel
  core of its own, ~0.3 = core 1's time limit, ~0 = no core. M ~1 = core 0
  wasn't slowed. On the console both core 1 and core 2 requests ran on cpu 2
  at 0.9-1.0;
- the top screen shows the 3D slider in %.

Measured so far: `port/docs/3DS_PROGRESS.md`.
