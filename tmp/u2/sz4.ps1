$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Sz4 {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint done, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool FlushFileBuffers(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetCompressedFileSizeW(string name, out uint high);
}
'@
$dir = $args[0]
$runs = [int]$args[1]
$flush = $args[2] -eq 'flush'
[void][IO.Directory]::CreateDirectory($dir)
$n = 5000
$data = New-Object byte[] ([int]$n)
for ($i = 0; $i -lt $n; $i++) { $data[$i] = [byte](65 + ($i % 26)) }
Write-Host ("[sz4] dir=" + $dir + " runs=" + $runs + " flush=" + $flush + " payload=" + $n)
$less = 0
$hist = @{}
for ($run = 1; $run -le $runs; $run++) {
    $path = Join-Path $dir ("n$run.bin")
    $h = [Sz4]::CreateFileW($path, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
    if ($h -eq [IntPtr]::Zero) { Write-Host ("[sz4] CreateFileW win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error()); exit 3 }
    $done = 0
    [void][Sz4]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
    if ($flush) { [void][Sz4]::FlushFileBuffers($h) }
    [void][Sz4]::CloseHandle($h)
    $high = [uint32]0
    $low = [Sz4]::GetCompressedFileSizeW($path, [ref]$high)
    $alloc = ([uint64]$high -shl 32) -bor [uint64]$low
    $key = [string]$alloc
    if ($hist.ContainsKey($key)) { $hist[$key]++ } else { $hist[$key] = 1 }
    if ($alloc -lt [uint64]$n) { $less++ }
    [void][IO.File]::Delete($path)
}
$parts = @()
foreach ($k in ($hist.Keys | Sort-Object { [int]$_ })) { $parts += ($k + " x" + $hist[$k]) }
Write-Host ("[sz4] alloc < logical: " + $less + " из " + $runs + " (" + [Math]::Round(100.0 * $less / $runs, 2) + " %)")
Write-Host ("[sz4] распределение alloc: " + ($parts -join ', '))