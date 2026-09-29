# MrProper H3: build a reparse-loop sandbox in %TEMP%. Read-only w.r.t. the real disk.
# ASCII only, no parentheses in echo inside if(...).
$ErrorActionPreference = 'Continue'
$root = Join-Path $env:TEMP 'mrproper-h3'
if (Test-Path -LiteralPath $root) { cmd.exe /c rmdir "$root" > $null 2>&1 }
New-Item -ItemType Directory -Path $root -Force | Out-Null

$bytes = New-Object byte[] 4096
(New-Object System.Random 7).NextBytes($bytes)

# 1. self-loop: junction at root pointing at root
cmd.exe /c mklink /J "$root\loop" "$root" | Out-Null

# 2. deep chain, every level has a junction to its own parent -> 30 nested loops
$cur = $root
for ($i = 0; $i -lt 30; $i++) {
    $next = Join-Path $cur ('lvl{0:d2}' -f $i)
    New-Item -ItemType Directory -Path $next -Force | Out-Null
    cmd.exe /c mklink /J (Join-Path $next 'up') $cur | Out-Null
    cmd.exe /c mklink /J (Join-Path $next 'root') $root | Out-Null
    $f = Join-Path $next 'f.bin'
    if (-not (Test-Path -LiteralPath $f)) { [IO.File]::WriteAllBytes($f, $bytes) }
    $cur = $next
}

# 3. wide fan: 200 sibling dirs, each with a junction back to the root
for ($i = 0; $i -lt 200; $i++) {
    $d = Join-Path $root ('wide{0:d3}' -f $i)
    New-Item -ItemType Directory -Path $d -Force | Out-Null
    cmd.exe /c mklink /J (Join-Path $d 'back') $root | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $d 'w.bin'), $bytes)
}

# 4. unicode + odd names next to a loop
$odd = Join-Path $root 'unicode'
New-Item -ItemType Directory -Path $odd -Force | Out-Null
$nameRu = -join @(0x043A, 0x0438, 0x0440, 0x0438, 0x043B, 0x0438, 0x0446, 0x0430 | ForEach-Object { [char]$_ })
$nameZh = -join @(0x4E2D, 0x6587, 0x76EE, 0x5F55 | ForEach-Object { [char]$_ })
$nameEmoji = -join @(0x1F5D1, 0xFE0F | ForEach-Object { [char]$_ })
[IO.File]::WriteAllBytes((Join-Path $odd ($nameRu + '.bin')), $bytes)
[IO.File]::WriteAllBytes((Join-Path $odd ($nameZh + '.bin')), $bytes)
[IO.File]::WriteAllBytes((Join-Path $odd ($nameEmoji + '.bin')), $bytes)
[IO.File]::WriteAllBytes((Join-Path $odd 'sp ace$trail .bin'), $bytes)
cmd.exe /c mklink /J (Join-Path $odd 'loop') $root | Out-Null

# 5. long names (segment 200 chars) plus a loop at the end
$long = $root
$seg = 'L' * 200
for ($i = 0; $i -lt 4; $i++) {
    $long = Join-Path $long $seg
    New-Item -ItemType Directory -Path $long -Force | Out-Null
}
cmd.exe /c mklink /J (Join-Path $long 'loop') $root | Out-Null
[IO.File]::WriteAllBytes((Join-Path $long 'deep.bin'), $bytes)

# 6. file that is a symlink to itself
cmd.exe /c mklink (Join-Path $root 'selflink.bin') (Join-Path $root 'a.bin') | Out-Null

$junctions = (cmd.exe /c "dir /a /s /b `"$root`"" | Where-Object { $_ -match 'JUNCTION' }).Count
$dirs = (Get-ChildItem -LiteralPath $root -Directory -Recurse -Force -ErrorAction SilentlyContinue | Measure-Object).Count
$files = (Get-ChildItem -LiteralPath $root -File -Recurse -Force -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Host ("ROOT " + $root)
Write-Host ("DIRS " + $dirs)
Write-Host ("FILES " + $files)
Write-Host ("OK")
