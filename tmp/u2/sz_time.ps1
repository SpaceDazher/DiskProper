$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Sz2 {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint done, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool FlushFileBuffers(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetCompressedFileSizeW(string name, out uint high);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetDiskFreeSpaceW(string root, out uint spc, out uint bps, out uint fc, out uint tc);
}
'@
$dir = $args[0]
[void][IO.Directory]::CreateDirectory($dir)
[uint32]$spc = 0; [uint32]$bps = 0; [uint32]$fc = 0; [uint32]$tc = 0
[void][Sz2]::GetDiskFreeSpaceW($dir.Substring(0, 3), [ref]$spc, [ref]$bps, [ref]$fc, [ref]$tc)
$cluster = [uint64]$spc * [uint64]$bps
Write-Host ("[sz2] том {0}, кластер {1} байт" -f $dir.Substring(0, 3), $cluster)
$n = 5000
$data = New-Object byte[] ([int]$n)
for ($i = 0; $i -lt $n; $i++) { $data[$i] = [byte](65 + ($i % 26)) }
for ($run = 1; $run -le 3; $run++) {
    $path = Join-Path $dir ("t$run.bin")
    $h = [Sz2]::CreateFileW($path, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
    $done = 0
    [void][Sz2]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
    [void][Sz2]::FlushFileBuffers($h)
    [void][Sz2]::CloseHandle($h)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $last = -1
    $line = ''
    while ($sw.ElapsedMilliseconds -lt 6000) {
        $high = [uint32]0
        $low = [Sz2]::GetCompressedFileSizeW($path, [ref]$high)
        $alloc = ([uint64]$high -shl 32) -bor [uint64]$low
        if ($alloc -ne $last) {
            $line += ("{0}мс={1} " -f $sw.ElapsedMilliseconds, $alloc)
            $last = [int64]$alloc
        }
        Start-Sleep -Milliseconds 10
    }
    $rounded = [uint64]([Math]::Ceiling($n / $cluster) * $cluster)
    Write-Host ("[sz2] progov " + $run + " logical=" + $n + " okruglenie=" + $rounded + ": " + $line)
    [void][IO.File]::Delete($path)
}