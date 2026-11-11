# Into a level, then a named surface dump every 3 s (e00_, e01_, ...) to catch the brightness change.
param([string]$Name = 'dim', [int]$Count = 16)
$logs = 'H:\spyro-xbox\logs'
$pad = "$logs\pad.txt"
& 'H:\spyro-xbox\tools\live.ps1' $Name
Start-Sleep 38
foreach ($i in 1..3) { 'start:200' | Add-Content $pad; Start-Sleep 4 }
Start-Sleep 2
'a:200' | Add-Content $pad
foreach ($i in 0..($Count - 1)) {
    Start-Sleep 3
    ('e{0:D2}_' -f $i) | Set-Content "$logs\dump.now"
}
Start-Sleep 3
& 'H:\spyro-xbox\tools\snap.ps1' $Name
Stop-Process -Name spyro_anb_recomp -Force -Confirm:$false
