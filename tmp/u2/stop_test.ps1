$ErrorActionPreference = 'Continue'
$exe = 'D:\Project\MrProper\build\u2\src\ui\Debug\mrproper.exe'
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 7
Write-Host ('[s] перед закрытием HasExited=' + $p.HasExited)
[void]$p.CloseMainWindow()
for ($i = 1; $i -le 8; $i++) {
    Start-Sleep -Milliseconds 500
    Write-Host ('[s] +' + ($i * 500) + ' мс HasExited=' + $p.HasExited + ' живы: [' + ((Get-Process mrproper -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',') + ']')
    if ($p.HasExited) { break }
}
if (-not $p.HasExited) {
    Write-Host '[s] принудительное завершение'
    Stop-Process -Id $p.Id -Force
    Start-Sleep -Seconds 1
}
Write-Host ('[s] после: HasExited=' + $p.HasExited + ' живы: [' + ((Get-Process mrproper -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',') + ']')
Write-Host ('[s] nav.current = ' + (Get-Item -LiteralPath 'HKCU:\Software\MrProper\UI').GetValue('nav.current', '<нет>'))