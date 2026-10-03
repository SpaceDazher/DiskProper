foreach ($d in @('C:', 'D:')) {
    try {
        $di = New-Object IO.DriveInfo($d)
        Write-Host ("{0} ready={1} format={2} label='{3}' total={4} free={5} avail={6}" -f `
            $d, $di.IsReady, $di.DriveFormat, $di.VolumeLabel, [Math]::Round($di.TotalSize / 1GB, 1),
            [Math]::Round($di.TotalFreeSpace / 1GB, 3), [Math]::Round($di.AvailableFreeSpace / 1GB, 3))
    } catch { Write-Host ("$d : " + $_.Exception.Message) }
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Voi {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetVolumeInformationW(string root, System.Text.StringBuilder name, uint nameSize,
        out uint serial, out uint maxComp, out uint flags, System.Text.StringBuilder fs, uint fsSize);
}
'@
foreach ($d in @('C:\', 'D:\')) {
    $name = New-Object Text.StringBuilder 256
    $fs = New-Object Text.StringBuilder 256
    [uint32]$serial = 0; [uint32]$maxComp = 0; [uint32]$flags = 0
    $ok = [Voi]::GetVolumeInformationW($d, $name, 256, [ref]$serial, [ref]$maxComp, [ref]$flags, $fs, 256)
    Write-Host ("{0} ok={1} fs='{2}' serial=0x{3:X8} maxComp={4} flags=0x{5:X} label='{6}'" -f `
        $d, $ok, $fs.ToString(), $serial, $maxComp, $flags, $name.ToString())
}