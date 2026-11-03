# Unattended trip into the level, one surface dump, then stop: tools\tolevel.ps1 <name>
# One-shot presses only. Inherits the environment, so experiment switches can be set first.
param([string]$Name = 'lvl')
$logs = 'H:\spyro-xbox\logs'
$pad = "$logs\pad.txt"
& 'H:\spyro-xbox\tools\live.ps1' $Name
Start-Sleep 38
foreach ($i in 1..3) { 'start:200' | Add-Content $pad; Start-Sleep 4 }
Start-Sleep 2
'a:200' | Add-Content $pad
Start-Sleep 40
New-Item -ItemType File "$logs\dump.now" -Force | Out-Null
Start-Sleep 3
& 'H:\spyro-xbox\tools\snap.ps1' $Name
Stop-Process -Name spyro_anb_recomp -Force -Confirm:$false
