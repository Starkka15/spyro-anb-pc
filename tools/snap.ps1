# Screenshot of the running game window: tools\snap.ps1 <name>
param([string]$Name = 'snap')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public struct Rect { public int L, T, R, B; }
public static class Win {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
$p = Get-Process spyro_anb_recomp -ErrorAction Stop
[void][Win]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 150
$r = New-Object Rect
[void][Win]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$b = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
$b.Save("H:\spyro-xbox\logs\$Name.png")
"saved $Name.png"
