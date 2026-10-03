$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Tz {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint done, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool FlushFileBuffers(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetCompressedFileSizeW(string name, out uint high);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetFileAttributesW(string name);
}
'@
$dir = $args[0]
$reps = [int]$args[1]
[void][IO.Directory]::CreateDirectory($dir)
$n = 5000
$data = New-Object byte[] ([int]$n)
for ($i = 0; $i -lt $n; $i++) { $data[$i] = [byte](65 + ($i % 26)) }
Write-Host ("[tz] dir=" + $dir + " payload=" + $n + " reps=" + $reps)
foreach ($delay in @(0, 5, 20, 50, 100, 200, 500, 1000)) {
    $less = 0
    for ($rep = 0; $rep -lt $reps; $rep++) {
        $path = Join-Path $dir ("t$rep.bin")
        $h = [Tz]::CreateFileW($path, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
        $done = 0
        [void][Tz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
        [void][Tz]::CloseHandle($h)
        if ($delay -gt 0) { Start-Sleep -Milliseconds $delay }
        $high = [uint32]0
        $low = [Tz]::GetCompressedFileSizeW($path, [ref]$high)
        $alloc = ([uint64]$high -shl 32) -bor [uint64]$low
        $attrs = [Tz]::GetFileAttributesW($path)
        if ($alloc -lt [uint64]$n) { $less++ }
        [void][IO.File]::Delete($path)
    }
    Write-Host ("[tz] без FlushFileBuffers, пауза " + $delay + " мс: alloc < logical в " + $less + " из " + $reps)
}