# U2 probe: почему allocatedBytes < logicalBytes у vfs_edge_tests.cpp:541.
#
# Проба повторяет ровно то, что делает тест: создаёт файл на длинном пути
# (длиннее MAX_PATH), пишет в него 5000 байт, закрывает и немедленно спрашивает
# GetCompressedFileSizeW (это и есть allocatedBytes в vfs_size::measurePath) и
# GetFileSizeEx (logicalBytes). Разница, если она есть, — гонка метаданных NTFS,
# а не логика приложения.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tmp\u2_probe_alloc.ps1 -Runs 200
param(
    [int]$Runs = 200,
    [int]$Payload = 5000,
    [switch]$Flush,
    [switch]$Handle,
    [switch]$Quiet,
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Alloc {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CreateDirectoryW(string name, IntPtr sa);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint done, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool FlushFileBuffers(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetCompressedFileSizeW(string name, out uint high);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetFileSizeEx(string name, out long size);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetFileInformationByHandleEx(IntPtr h, int cls, out FILE_STANDARD_INFO info, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeleteFileW(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetDiskFreeSpaceW(string root, out uint spc, out uint bps, out uint fc, out uint tc);
    [StructLayout(LayoutKind.Sequential)]
    public struct FILE_STANDARD_INFO {
        public long AllocationSize;
        public long EndOfFile;
        public uint NumberOfLinks;
        [MarshalAs(UnmanagedType.U1)] public bool DeletePending;
        [MarshalAs(UnmanagedType.U1)] public bool Directory;
    }
    public const int FileStandardInfo = 1;
}
'@

$base = Join-Path $env:TEMP ('u2-alloc-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
[void][Alloc]::CreateDirectoryW($base, [IntPtr]::Zero)
$segment = 'глубокий-уровень-очень-длинного-имени'
$deep = $base
while ($deep.Length -lt 300) { $deep = Join-Path $deep $segment }
$ext = '\\?\' + (Join-Path $deep 'u2.dat')
$cur = '\\?\' + $deep
while ($cur.Length -gt ('\\?\' + $base).Length) {
    $up = Split-Path -Parent $cur
    if (-not [Alloc]::CreateDirectoryW($up, [IntPtr]::Zero)) { break }
    $cur = $up
}

$size = [int]$Payload
$data = New-Object byte[] $size
for ($i = 0; $i -lt $size; $i++) { $data[$i] = [byte](65 + ($i % 26)) }

[uint32]$spc = 0; [uint32]$bps = 0; [uint32]$fc = 0; [uint32]$tc = 0
$cluster = 0
if ([Alloc]::GetDiskFreeSpaceW($ext, [ref]$spc, [ref]$bps, [ref]$fc, [ref]$tc)) {
    $cluster = [uint64]$spc * [uint64]$bps
}
Write-Host ("[u2] путь {0} симв., кластер {1} байт, Flush={2}, Handle={3}" -f $ext.Length, $cluster, [bool]$Flush, [bool]$Handle)

$bad = 0
$badValues = New-Object System.Collections.ArrayList
for ($run = 1; $run -le $Runs; $run++) {
    $h = [Alloc]::CreateFileW($ext, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
    if ($h -eq [IntPtr]::Zero) {
        Write-Host ("[u2] CreateFileW: win32={0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        exit 3
    }
    $done = 0
    [void][Alloc]::WriteFile($h, $data, [uint32]$size, [ref]$done, [IntPtr]::Zero)
    if ($Flush) { [void][Alloc]::FlushFileBuffers($h) }
    [void][Alloc]::CloseHandle($h)

    $high = [uint32]0
    $low = [Alloc]::GetCompressedFileSizeW($ext, [ref]$high)
    $alloc = ([uint64]$high -shl 32) -bor [uint64]$low
    $logical = [int64]0
    [void][Alloc]::GetFileSizeEx($ext, [ref]$logical)
    $allocHandle = [int64]0
    if ($Handle) {
        $rh = [Alloc]::CreateFileW($ext, [uint32]2147483648, [uint32]1, [IntPtr]::Zero, 3, 0x80, [IntPtr]::Zero)
        $info = New-Object Alloc+FILE_STANDARD_INFO
        if ([Alloc]::GetFileInformationByHandleEx($rh, [Alloc]::FileStandardInfo, [ref]$info,
                [Runtime.InteropServices.Marshal]::SizeOf([type][Alloc+FILE_STANDARD_INFO]))) {
            $allocHandle = $info.AllocationSize
        }
        [void][Alloc]::CloseHandle($rh)
    }
    if ($alloc -lt [uint64]$logical) {
        $bad++
        if ($badValues.Count -lt 15) { [void]$badValues.Add(("проход {0}: alloc={1} logical={2}" -f $run, $alloc, $logical)) }
    }
    if (-not $Quiet -or $alloc -lt [uint64]$logical) {
        Write-Host ("[u2] проход {0}: alloc={1} logical={2}{3}" -f $run, $alloc, $logical,
            $(if ($Handle) { " handleAlloc=$allocHandle" } else { '' }))
    }
    [void][Alloc]::DeleteFileW($ext)
}
Write-Host ("[u2] ИТОГО: прогонов {0}, alloc < logical {1} ({2} %)" -f $Runs, $bad, [Math]::Round(100.0 * $bad / $Runs, 1))
foreach ($line in $badValues) { Write-Host ("[u2]   " + $line) }