$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Cz {
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
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint returned, IntPtr ov);
}
'@
$dir = $args[0]
[void][IO.Directory]::CreateDirectory($dir)
$dirAttrs = [Cz]::GetFileAttributesW($dir)
Write-Host ("[cz] dir=" + $dir + " attrs=0x" + $dirAttrs.ToString('X8') +
    " compressed=" + (($dirAttrs -band 0x0800) -ne 0))
$n = 5000
$data = New-Object byte[] ([int]$n)
for ($i = 0; $i -lt $n; $i++) { $data[$i] = [byte](65 + ($i % 26)) }

function Show([string]$tag, [string]$path) {
    $a = [Cz]::GetFileAttributesW($path)
    $high = [uint32]0
    $low = [Cz]::GetCompressedFileSizeW($path, [ref]$high)
    $alloc = ([uint64]$high -shl 32) -bor [uint64]$low
    Write-Host ("[cz] " + $tag + ": attrs=0x" + $a.ToString('X8') + " compressed=" + (($a -band 0x0800) -ne 0) +
        " sparse=" + (($a -band 0x0200) -ne 0) + " GetCompressedFileSize=" + $alloc)
    [void][IO.File]::Delete($path)
}

# a) как пишет тест сейчас: CREATE_ALWAYS + FILE_ATTRIBUTE_NORMAL
$p1 = Join-Path $dir 'a.bin'
$h = [Cz]::CreateFileW($p1, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
$done = 0
[void][Cz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
[void][Cz]::FlushFileBuffers($h)
[void][Cz]::CloseHandle($h)
Show 'a: plain' $p1

# b) снять признак сжатия на пустом файле через SetFileAttributesW, потом писать
$p2 = Join-Path $dir 'b.bin'
$h = [Cz]::CreateFileW($p2, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
$okAttr = [Cz]::SetFileAttributesW($p2, [uint32]128)
Write-Host ("[cz] b: SetFileAttributesW(NORMAL) win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error() + " ok=" + $okAttr)
$done = 0
[void][Cz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
[void][Cz]::FlushFileBuffers($h)
[void][Cz]::CloseHandle($h)
Show 'b: SetFileAttributesW then write' $p2

# c) FSCTL_SET_COMPRESSION = 0x00090028, COMPRESSION_FORMAT_NONE = 1, на пустом файле
$p3 = Join-Path $dir 'c.bin'
$h = [Cz]::CreateFileW($p3, [uint32]1073741824, [uint32]1, [IntPtr]::Zero, [uint32]2, [uint32]128, [IntPtr]::Zero)
$buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
[void][Runtime.InteropServices.Marshal]::WriteInt32($buf, 1)
[uint32]$returned = 0
$okIo = [Cz]::DeviceIoControl($h, [uint32]0x00090028, $buf, 4, [IntPtr]::Zero, 0, [ref]$returned, [IntPtr]::Zero)
Write-Host ("[cz] c: FSCTL_SET_COMPRESSION ok=" + $okIo + " win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
[Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
$done = 0
[void][Cz]::WriteFile($h, $data, [uint32]$n, [ref]$done, [IntPtr]::Zero)
[void][Cz]::FlushFileBuffers($h)
[void][Cz]::CloseHandle($h)
Show 'c: FSCTL_SET_COMPRESSION(NONE) then write' $p3