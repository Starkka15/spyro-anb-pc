# Starts the game for live driving: tools\live.ps1 <name>. Press with: Add-Content logs\pad.txt "a:200"
param([string]$Name = 'live')
$logs = 'H:\spyro-xbox\logs'
Set-Content "$logs\pad.txt" ''
$env:RECOMP_PAD_LIVE = "$logs\pad.txt"
$p = Start-Process -FilePath 'H:\spyro-xbox\build\spyro_anb_recomp.exe' -WorkingDirectory 'H:\spyro-xbox\game_files' `
    -RedirectStandardOutput "$logs\$Name.out.log" -RedirectStandardError "$logs\$Name.err.log" -PassThru
"started $($p.Id)"
