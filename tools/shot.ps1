# Runs the game, saves a picture of its window at each given second, then stops only that process.
# usage: shot.ps1 [name] [seconds...]   Environment variables are inherited.
param([string]$Name = 'shot', [int[]]$At = @(15, 30))

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public struct Rect { public int L, T, R, B; }
public static class Win {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
}
'@

$logs = 'H:\spyro-xbox\logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$p = Start-Process -FilePath 'H:\spyro-xbox\build\spyro_anb_recomp.exe' -WorkingDirectory 'H:\spyro-xbox\game_files' `
    -RedirectStandardOutput "$logs\$Name.out.log" -RedirectStandardError "$logs\$Name.err.log" -PassThru
$h = $p.Handle
$start = Get-Date
foreach ($s in $At) {
    while (((Get-Date) - $start).TotalSeconds -lt $s) {
        if ($p.HasExited) { 'exited 0x{0:X8} before {1} s' -f $p.ExitCode, $s; return }
        Start-Sleep -Milliseconds 200
    }
    $p.Refresh()
    $r = New-Object Rect
    [void][Win]::GetWindowRect($p.MainWindowHandle, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { "no window at $s s"; continue }
    $b = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
    $b.Save("$logs\$Name-$s.png")
    "saved $Name-$s.png ${w}x$hh"
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false }
