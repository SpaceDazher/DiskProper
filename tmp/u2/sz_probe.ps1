$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Sz {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint done, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool FlushFileBuffers(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetCompressedFileSizeW(string name, out uint high);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetFileSizeEx(string name, out long size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetDiskFreeSpaceW(string root, out uint spc, out uint bps, out uint fc, out uint tc);
}
'@
$dir = 'D:\Temp\u2tmp\sz'
[void][IO.Directory]::CreateDirectory($dir)
[uint32]$spc = 0; [uint32]$bps = 0; [uint32]$fc = 0; [uint32]$tc = 0
Write-Host ("[sz] GetDiskFreeSpaceW('D:\') = " + [Sz]::GetDiskFreeSpaceW('D:\', [ref]$spc, [ref]$bps, [ref]$fc, [ref]$tc) +
    " spc=" + $spc + " bps=" + $bps + " cluster=" + ([uint64]$spc * [uint64]$bps))
foreach ($n in @(5000, 2048, 12288, 4097)) {
    $path = Join-Path $dir ("f$n.bin")
    $data = New-Object byte[] ([int]$n)
    $h = [Sz]::CreateFileW($path, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
    Write-Host ("[sz] CreateFileW win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    $done = 0
    [void][Sz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
    [void][Sz]::FlushFileBuffers($h)
    [void][Sz]::CloseHandle($h)
    $high = [uint32]0
    $low = [Sz]::GetCompressedFileSizeW($path, [ref]$high)
    $logical = [int64]0
    [void][Sz]::GetFileSizeEx($path, [ref]$logical)
    $alloc = ([uint64]$high -shl 32) -bor [uint64]$low
    $cluster = [uint64]$spc * [uint64]$bps
    Write-Host ("[sz] logical={0} alloc={1} alloc%cluster={2}" -f $logical, $alloc, ($alloc % $cluster))
    [void][IO.File]::Delete($path)
}