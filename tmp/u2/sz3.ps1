$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Sz3 {
    [StructLayout(LayoutKind.Sequential)]
    public struct FILE_STANDARD_INFO {
        public long AllocationSize;
        public long EndOfFile;
        public uint NumberOfLinks;
        [MarshalAs(UnmanagedType.U1)] public bool DeletePending;
        [MarshalAs(UnmanagedType.U1)] public bool Directory;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint done, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool FlushFileBuffers(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetCompressedFileSizeW(string name, out uint high);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetFileInformationByHandleEx(IntPtr h, int cls, out FILE_STANDARD_INFO info, uint size);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetDiskFreeSpaceW(string root, out uint spc, out uint bps, out uint fc, out uint tc);
}
'@
$dir = $args[0]
[void][IO.Directory]::CreateDirectory($dir)
[uint32]$spc = 0; [uint32]$bps = 0; [uint32]$fc = 0; [uint32]$tc = 0
[void][Sz3]::GetDiskFreeSpaceW($dir.Substring(0, 3), [ref]$spc, [ref]$bps, [ref]$fc, [ref]$tc)
$cluster = [uint64]$spc * [uint64]$bps
Write-Host ("[sz3] tom " + $dir.Substring(0, 3) + ", klaster " + $cluster)
foreach ($n in @(5000, 2048, 12288)) {
    $path = Join-Path $dir ("x$n.bin")
    $data = New-Object byte[] ([int]$n)
    for ($i = 0; $i -lt $n; $i++) { $data[$i] = [byte](65 + ($i % 26)) }
    $h = [Sz3]::CreateFileW($path, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
    $done = 0
    [void][Sz3]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
    [void][Sz3]::FlushFileBuffers($h)
    [void][Sz3]::CloseHandle($h)
    $high = [uint32]0
    $low = [Sz3]::GetCompressedFileSizeW($path, [ref]$high)
    $compressed = ([uint64]$high -shl 32) -bor [uint64]$low
    $rh = [Sz3]::CreateFileW($path, [uint32]2147483648, [uint32]1, [IntPtr]::Zero, [uint32]3, [uint32]128, [IntPtr]::Zero)
    $info = New-Object Sz3+FILE_STANDARD_INFO
    $ok = [Sz3]::GetFileInformationByHandleEx($rh, 1, [ref]$info, [Runtime.InteropServices.Marshal]::SizeOf([type][Sz3+FILE_STANDARD_INFO]))
    [void][Sz3]::CloseHandle($rh)
    Write-Host ("[sz3] n=" + $n + " GetCompressedFileSize=" + $compressed + " FileStandardInfo ok=" + $ok +
        " AllocationSize=" + $info.AllocationSize + " EndOfFile=" + $info.EndOfFile +
        " allocated%cluster=" + ($info.AllocationSize % $cluster))
    [void][IO.File]::Delete($path)
}