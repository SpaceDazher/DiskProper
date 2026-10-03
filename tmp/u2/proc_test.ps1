$ErrorActionPreference = 'Continue'
$exe = 'D:\Project\MrProper\build\u2\src\ui\Debug\mrproper.exe'
$p = Start-Process -FilePath $exe -PassThru
Write-Host ('[p] запущен pid ' + $p.Id)
for ($i = 1; $i -le 6; $i++) {
    Start-Sleep -Seconds 1
    $ids = (Get-Process mrproper -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ','
    Write-Host ('[p] t=' + $i + ' с HasExited(запущенный)=' + $p.HasExited + ' все mrproper: [' + $ids + ']')
}
$all = Get-CimInstance Win32_Process -Filter "Name='mrproper.exe'"
foreach ($proc in $all) {
    Write-Host ('[p] процесс ' + $proc.ProcessId + ' создан ' + $proc.CreationDate + ' cmd=' + $proc.CommandLine)
}
foreach ($proc in $all) { Stop-Process -Id $proc.ProcessId -Force }
Start-Sleep -Seconds 2
Write-Host ('[p] после kill остались: ' + ((Get-Process mrproper -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ','))