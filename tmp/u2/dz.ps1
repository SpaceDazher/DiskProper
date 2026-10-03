$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Dz {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint done, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool FlushFileBuffers(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetFileAttributesW(string name, uint attrs);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetFileAttributesW(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetCompressedFileSizeW(string name, out uint high);
}
'@
$dir = $args[0]
[void][IO.Directory]::CreateDirectory($dir)
$n = 5000
$data = New-Object byte[] ([int]$n)
for ($i = 0; $i -lt $n; $i++) { $data[$i] = [byte](65 + ($i % 26)) }
function Show([string]$tag, [string]$path) {
    $a = [Dz]::GetFileAttributesW($path)
    $high = [uint32]0
    $low = [Dz]::GetCompressedFileSizeW($path, [ref]$high)
    $alloc = ([uint64]$high -shl 32) -bor [uint64]$low
    Write-Host ("[dz] " + $tag + ": attrs=0x" + $a.ToString('X8') + " compressed=" + (($a -band 0x0800) -ne 0) +
        " GetCompressedFileSize=" + $alloc)
    [void][IO.File]::Delete($path)
}
# d) снять сжатие ПОСЛЕ записи и закрытия
$pd = Join-Path $dir 'd.bin'
$h = [Dz]::CreateFileW($pd, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
$done = 0
[void][Dz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
[void][Dz]::FlushFileBuffers($h)
[void][Dz]::CloseHandle($h)
$ok = [Dz]::SetFileAttributesW($pd, [uint32]128)
Write-Host ("[dz] d: SetFileAttributesW after close ok=" + $ok + " win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
Show 'd: uncompressed after write' $pd

# e) снять сжатие, открыв файл на запись
$pe = Join-Path $dir 'e.bin'
$h = [Dz]::CreateFileW($pe, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
$done = 0
[void][Dz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
[void][Dz]::FlushFileBuffers($h)
$ok = [Dz]::SetFileAttributesW($pe, [uint32]128)
Write-Host ("[dz] e: SetFileAttributesW while open ok=" + $ok + " win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
[void][Dz]::CloseHandle($h)
Show 'e: uncompressed while open' $pe

# f) подкаталог без сжатия внутри сжатого
$sub = Join-Path $dir 'nosub'
[void][IO.Directory]::CreateDirectory($sub)
$okSub = [Dz]::SetFileAttributesW($sub, [uint32]16)
Write-Host ("[dz] f: SetFileAttributesW(dir, DIRECTORY) ok=" + $okSub + " win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
Write-Host ("[dz] f: dir attrs=0x" + ([Dz]::GetFileAttributesW($sub)).ToString('X8'))
$pf = Join-Path $sub 'f.bin'
$h = [Dz]::CreateFileW($pf, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
$done = 0
[void][Dz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
[void][Dz]::FlushFileBuffers($h)
[void][Dz]::CloseHandle($h)
Show 'f: file in uncompressed subdir' $pf
[void][IO.Directory]::Delete($sub, $true)