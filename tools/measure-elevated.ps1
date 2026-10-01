# MrProper: замеры, которым нужны права администратора.
#
# Зачем: §12.1 (эталонные VM) и §12.2 (тайминги скана C:, карта разделов) нельзя
# закрыть без повышения: \\.\PhysicalDriveN не открывается обычным запуском, и
# приложение честно помечает устройство недоступным вместо того, чтобы врать.
# Этот скрипт сам повышает права (один запрос UAC) и снимает то, что иначе
# остаётся непроверенным.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-elevated.ps1
#
# Пишет D:\Temp\measure-elevated.json и печатает сводку. Прав администратора
# скрипт не меняет: он только читает.
$ErrorActionPreference = 'Continue'
$repo = 'D:\Project\MrProper'
$out = 'D:\Temp\measure-elevated.json'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Самоповышение: если прав нет, перезапускаем с RunAs и выходим.
if (-not (Test-Admin)) {
    Write-Host '[measure] требуется повышение: подтвердите UAC'
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\measure-elevated.ps1")
    exit 5
}

$cli = Join-Path $repo 'build\a4\Debug\mrproper_cli.exe'
if (-not (Test-Path $cli)) { $cli = Join-Path $repo 'build\main\Debug\mrproper_cli.exe' }
if (-not (Test-Path $cli)) {
    Write-Host "[measure] нет бинарника CLI: $cli — сначала tools\build.bat Debug"
    exit 2
}

$result = [ordered]@{
    elevated  = $true
    machine   = $env:COMPUTERNAME
    os        = (Get-CimInstance Win32_OperatingSystem).Caption
    osBuild   = [string](Get-CimInstance Win32_OperatingSystem).BuildNumber
    disks     = $null
    diskExit  = $null
    scanMs    = $null
    scanKb    = $null
    scanExit  = $null
    bitlocker = @()
    protected = @()
}

# 1) Карта разделов: главный пункт, ради которого нужны права.
$sw = [Diagnostics.Stopwatch]::StartNew()
$disks = & $cli disks --json --strict 2>&1
$result.diskExit = $LASTEXITCODE
$sw.Stop()
$result.disks = ($disks -join "`n")
Write-Host ("[measure] disks: код {0}, {1} мс, {2} байт вывода" -f $result.diskExit, $sw.ElapsedMilliseconds, $result.disks.Length)

# 2) Тайминг полного скана набором правил репозитория: §12.2 требует число, а не «быстро».
$rules = Join-Path $repo 'rules'
$sw = [Diagnostics.Stopwatch]::StartNew()
$scan = & $cli scan --json --rules $rules --quiet 2>&1
$result.scanExit = $LASTEXITCODE
$sw.Stop()
$result.scanMs = $sw.ElapsedMilliseconds
$result.scanKb = [int](($scan -join "`n").Length / 1024)
Write-Host ("[measure] scan по набору правил: код {0}, {1} мс, {2} КБ" -f $result.scanExit, $result.scanMs, $result.scanKb)

# 3) Состояние BitLocker: приложение читает его, но здесь нужно как эталон.
try {
    $bde = & manage-bde -status 2>&1
    $result.bitlocker = @($bde | Where-Object { $_ -match 'Conversion|Protection|Защита|Преобразование|Percentage' } | ForEach-Object { $_.Trim() })
} catch { $result.bitlocker = @("manage-bde недоступен: $_") }

# 4) Что приложение считает защищённым: полезно знать, что не является мусором.
foreach ($p in @("$env:SystemRoot", "$env:SystemRoot\System32", 'C:\Program Files', "$env:ProgramData")) {
    if (Test-Path $p) { $result.protected += $p }
}

$result | ConvertTo-Json -Depth 5 | Out-File $out -Encoding utf8
Write-Host "[measure] записано: $out"
Write-Host ("[measure] ИТОГ: карта разделов код {0}; скан {1} мс код {2}; томов в отчёте: {3}" -f `
    $result.diskExit, $result.scanMs, $result.scanExit,
    ([regex]::Matches($result.disks, '"mountPoints"').Count))
exit 0