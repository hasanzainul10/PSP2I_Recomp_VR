# Dev tool (3DS renderer testing): restart Azahar with port/3ds/build/psp2i.3dsx, wait,
# screenshot its window into port/3ds/build/shots/<Shot>.png, print the log tail.
#   powershell -File port/3ds/tools/azrun.ps1 -Wait 20 -Shot title [-NoRestart]
param([int]$Wait = 40, [string]$Shot = "shot", [switch]$NoRestart)
# Restart Azahar with the 3DS build, wait, screenshot its window, print the log.
$ErrorActionPreference = "Continue"
$az = 'D:\Tools\azahar\azahar-windows-msvc-2126.2\azahar.exe'
$game = 'D:\work\PSP2I_Recomp_3DS\port\3ds\build\psp2i.3dsx'
$dir = Join-Path $PSScriptRoot "..buildshots"; New-Item -ItemType Directory -Force $dir | Out-Null
if (-not $NoRestart) {
    Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 1
    Start-Process $az -ArgumentList "`"$game`"" | Out-Null
}
Start-Sleep $Wait
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class AzW { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
public struct RECT { public int L, T, R, B; } }
'@
$p = Get-Process azahar -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p) {
    $h = $p.MainWindowHandle
    if ([AzW]::IsIconic($h)) { [AzW]::ShowWindow($h, 4) | Out-Null; Start-Sleep -Milliseconds 700 }   # restore without activating
    # a known place and size on the main screen, on top (no focus needed)
    [AzW]::SetWindowPos($h, [IntPtr](-1), 40, 40, 1021, 727, 0x10) | Out-Null    # HWND_TOPMOST, NOACTIVATE
    Start-Sleep -Milliseconds 600
    $r = New-Object AzW+RECT
    [AzW]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -gt 0 -and $hh -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        [AzW]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x13) | Out-Null    # HWND_NOTOPMOST
        $bmp.Save("$dir\$Shot.png")
        "screenshot: $dir\$Shot.png (${w}x${hh}) title: $($p.MainWindowTitle)"
    }
} else { "azahar is not running" }
$log = "$env:APPDATA\Azahar\sdmc\psp2i\psp2i_log.txt"
Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch '^stack:|AllocPartition|GetBlockHead|sceUtilityLoadModule' } | Select-Object -Last 14
$a = "$env:APPDATA\Azahar\log\azahar_log.txt"
Get-Content $a -ErrorAction SilentlyContinue | Select-String "Exception Type|PANIC" | Select-Object -First 2 | ForEach-Object { "azahar: " + $_.Line }
