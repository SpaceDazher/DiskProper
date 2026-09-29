# MrProper H3: probe enumerateRoot (src/cli/cmd_apply.cpp) for ACCESS_VIOLATION.
# DRY RUN ONLY: no --execute, nothing is deleted. Root is the %TEMP% sandbox.
$ErrorActionPreference = 'Continue'
$cli = 'D:\Project\MrProper\build\a3\Debug\mrproper_cli.exe'
$root = Join-Path $env:TEMP 'mrproper-h3-enum'
if (Test-Path -LiteralPath $root) { cmd.exe /c rmdir "$root" > $null 2>&1 }

$bytes = New-Object byte[] 4096
(New-Object System.Random 11).NextBytes($bytes)

# candidate dir with files, nested dirs, unicode, a junction loop, a symlink-ish link
New-Item -ItemType Directory -Path $root -Force | Out-Null
[IO.File]::WriteAllBytes((Join-Path $root 'a.bin'), $bytes)
[IO.File]::WriteAllBytes((Join-Path $root 'b.bin'), $bytes)
$nameRu = -join @(0x043A, 0x0438, 0x0440, 0x0438, 0x043B, 0x0438, 0x0446, 0x0430 | ForEach-Object { [char]$_ })
[IO.File]::WriteAllBytes((Join-Path $root ($nameRu + '.bin')), $bytes)
for ($i = 0; $i < 20; $i++) {
    $d = Join-Path $root ('d{0:d2}' -f $i)
    New-Item -ItemType Directory -Path $d -Force | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $d 'x.bin'), $bytes)
    cmd.exe /c mklink /J (Join-Path $d 'loop') $root | Out-Null
}
# a long chain deeper than kMaxEnumerationDepth = 64
$cur = $root
for ($i = 0; $i -lt 80; $i++) {
    $cur = Join-Path $cur ('c{0:d2}' -f $i)
    New-Item -ItemType Directory -Path $cur -Force | Out-Null
    cmd.exe /c mklink /J (Join-Path $cur 'up') (Split-Path $cur -Parent) | Out-Null
}
[IO.File]::WriteAllBytes((Join-Path $cur 'deep.bin'), $bytes)

$doc = [ordered]@{
    schema = 1
    kind = 'scan'
    app = [ordered]@{ version = '0.1.0'; pid = 0; rulesVersion = 'h3.enum' }
    candidates = @([ordered]@{
        ruleId = 'h3.enum'
        category = 'temp.user'
        path = $root
        displayName = 'H3 enumerateRoot probe'
        logicalBytes = 4096
        allocatedBytes = 4096
        fileCount = 3
        oldestWrite = 1600000000
        newestWrite = 1700000000
        lastAccess = 1700000000
        safety = 'safe'
        confidence = 95
        reasons = @('H3 probe')
    })
}
$candPath = Join-Path $env:TEMP 'h3-enum-candidates.json'
$doc | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $candPath -Encoding UTF8

$codes = @{}
for ($i = 1; $i -le 60; $i++) {
    $out = & $cli apply --json --candidates $candPath 2>&1 | Out-Null
    $code = $LASTEXITCODE
    if ($codes.ContainsKey($code)) { $codes[$code]++ } else { $codes[$code] = 1 }
}
Write-Host 'APPLY-CODES'
foreach ($k in ($codes.Keys | Sort-Object)) { Write-Host ('  ' + $k + ' x' + $codes[$k]) }
Write-Host ('TOTAL ' + (($codes.Values | Measure-Object -Sum).Sum))
Write-Host 'OK'
