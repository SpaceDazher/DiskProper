$ErrorActionPreference = 'Continue'
$key = 'HKCU:\Software\MrProper\UI'
function NavCurrent { (Get-Item -LiteralPath $key).GetValue('nav.current', '<нет>') }
Write-Host ('[t] старт nav.current = ' + (NavCurrent) + ' @ ' + (Get-Date -Format 'HH:mm:ss.fff'))
New-ItemProperty -Path $key -Name 'nav.current' -Value 'overview' -PropertyType String -Force | Out-Null
Write-Host ('[t] принудительно поставлено overview @ ' + (Get-Date -Format 'HH:mm:ss.fff'))
$psi = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', '"D:\Project\MrProper\tmp\u2\ui.bat" build\u2\src\ui\Debug\mrproper.exe D:\Temp\u2tmp\reg.png dark settings D:\Project\MrProper\tmp\u2\ui_reg.log' -PassThru -Wait -RedirectStandardOutput 'D:\Temp\u2tmp\reg_out.txt'
Write-Host ('[t] ворота закончились, код ' + $psi.ExitCode + ' @ ' + (Get-Date -Format 'HH:mm:ss.fff'))
Write-Host ('[t] nav.current сразу после ворот = ' + (NavCurrent))
Start-Sleep -Seconds 5
Write-Host ('[t] nav.current через 5 с = ' + (NavCurrent))
Get-Content -LiteralPath 'D:\Temp\u2tmp\reg_out.txt' -Tail 2
Get-Process mrproper -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ('[t] живой процесс: ' + $_.Id) }