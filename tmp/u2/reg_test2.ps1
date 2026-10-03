$ErrorActionPreference = 'Continue'
$key = 'HKCU:\Software\MrProper\UI'
function NavCurrent { (Get-Item -LiteralPath $key).GetValue('nav.current', '<нет>') }
function RunGate($exe, $shot, $page, $log) {
    New-ItemProperty -Path $key -Name 'nav.current' -Value 'settings' -PropertyType String -Force | Out-Null
    Write-Host ('[t] ' + (Get-Date -Format 'HH:mm:ss.fff') + ' поставлено settings, запуск ' + $exe + ' -Page ' + $page)
    $psi = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', ('"D:\Project\MrProper\tmp\u2\ui.bat" ' + $exe + ' ' + $shot + ' dark ' + $page + ' ' + $log) -PassThru -Wait -RedirectStandardOutput 'D:\Temp\u2tmp\reg_out.txt'
    Write-Host ('[t] ' + (Get-Date -Format 'HH:mm:ss.fff') + ' код ворот ' + $psi.ExitCode + ', nav.current = ' + (NavCurrent))
    Start-Sleep -Seconds 4
    Write-Host ('[t] ' + (Get-Date -Format 'HH:mm:ss.fff') + ' через 4 с nav.current = ' + (NavCurrent))
    Get-Process mrproper -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ('[t] живой процесс: ' + $_.Id) }
}
RunGate 'build\s2\src\ui\Debug\mrproper.exe' 'D:\Temp\u2tmp\pre2.png' 'disks' 'D:\Project\MrProper\tmp\u2\ui_pre2.log'
RunGate 'build\u2\src\ui\Debug\mrproper.exe' 'D:\Temp\u2tmp\post2.png' 'disks' 'D:\Project\MrProper\tmp\u2\ui_post2.log'