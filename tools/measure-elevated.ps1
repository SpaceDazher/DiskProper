# MrProper: замеры, которым нужны права администратора.
#
# Зачем: §12.1 (эталонные VM) и §12.2 (тайминги скана C:, карта разделов) нельзя
# закрыть без повышения: \\.\PhysicalDriveN не открывается на GENERIC_READ обычным
# запуском, и приложение честно помечает устройство недоступным вместо того, чтобы
# врать. Этот скрипт сам повышает права (один запрос UAC) и снимает то, что иначе
# остаётся непроверенным.
#
# Кроме карты разделов снимает код отказа КАЖДОГО вызова слоя IOCTL: без него
# в отчёте видно только «0 разделов», а не «какой IOCTL и с каким кодом отказал».
# Числа сверены с Get-Disk / Get-Partition — эталон Windows, а не мнение.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-elevated.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-elevated.ps1 -SkipScan
#
# Пишет D:\Temp\measure-elevated.json и печатает сводку. Прав администратора
# скрипт не меняет: он только читает.
param(
    # Полный скан по набору правил занимает сотни секунд (SPEC §12.2) и к карте
    # разделов отношения не имеет. -SkipScan пропускает именно его.
    [switch]$SkipScan,
    # Явный путь к CLI; по умолчанию перебираются слоты сборки.
    [string]$CliPath = ''
)
$ErrorActionPreference = 'Continue'
# Вывод приложения — UTF-8, а PowerShell 5.1 по умолчанию читает_native_-вывод в
# OEM-кодировке: русский текст в логе становится мусором. Кодировку задаём явно.
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
$repo = 'D:\Project\MrProper'
$out = 'D:\Temp\measure-elevated.json'
$logFile = 'D:\Temp\measure-elevated.log'

# Повышенный процесс запускается в отдельном окне, поэтому его stdout никто не
# видит: без файла журнала любой сбой выглядит как «скрипт ничего не сделал».
# Say пишет и в консоль, и в журнал — причина отказа всегда остаётся на диске.
function Say {
    param([string]$Message)
    Write-Host $Message
    try {
        $dir = Split-Path $logFile
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
        Add-Content -Path $logFile -Value $Message -Encoding utf8
    } catch { }
}
try { Set-Content -Path $logFile -Value ("=== запуск " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " elevated=" + (Test-Admin)) -Encoding utf8 } catch { }

# Вывод CLI берём РАЗДЕЛЬНО: stdout — это JSON, stderr — человеческий текст о ходе
# работы. Смешивать их через 2>&1 нельзя: строки прогресса попадают в середину
# тела JSON, и ConvertFrom-Json падает на позиции 6889 (проверено).
function Invoke-Cli {
    param([string[]]$Arguments)
    $errFile = [IO.Path]::GetTempFileName()
    try {
        $stdout = & $cli @Arguments 2> $errFile
        $code = $LASTEXITCODE
        $stderr = ''
        if (Test-Path $errFile) { $stderr = [string](Get-Content $errFile -Raw -ErrorAction SilentlyContinue) }
        return [ordered]@{ exit = $code; out = ($stdout -join "`n"); err = $stderr }
    } finally {
        Remove-Item $errFile -Force -ErrorAction SilentlyContinue
    }
}

function Read-AppJson {
    param([string]$Text)
    $start = $Text.IndexOf('{')
    $end = $Text.LastIndexOf('}')
    if ($start -lt 0 -or $end -le $start) { return $null }
    try { return ($Text.Substring($start, $end - $start + 1) | ConvertFrom-Json) }
    catch {
        Say "[measure] JSON приложения не разобран: $($_.Exception.Message)"
        return $null
    }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Самоповышение: если прав нет, перезапускаем с RunAs и выходим.
if (-not (Test-Admin)) {
    Say '[measure] требуется повышение: подтвердите UAC'
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\measure-elevated.ps1",
        $(if ($SkipScan) { '-SkipScan' } else { '' }))
    exit 5
}

if (-not $CliPath) {
    foreach ($slot in @('a1', 'a4', 'main')) {
        $candidate = Join-Path $repo "build\$slot\Debug\mrproper_cli.exe"
        if (Test-Path $candidate) { $CliPath = $candidate; break }
    }
}
$cli = $CliPath
if (-not $cli -or -not (Test-Path $cli)) {
    Say "[measure] нет бинарника CLI: $cli — сначала tools\build.bat Debug"
    exit 2
}
Say "[measure] CLI: $cli"

$result = [ordered]@{
    elevated  = $true
    machine   = $env:COMPUTERNAME
    os        = (Get-CimInstance Win32_OperatingSystem).Caption
    osBuild   = [string](Get-CimInstance Win32_OperatingSystem).BuildNumber
    cli       = $cli
    disks     = $null
    diskExit  = $null
    totals    = $null
    diskStderr = ''
    scanMs    = $null
    scanKb    = $null
    scanExit  = $null
    ioctl     = @()
    reference = $null
    match     = $null
    bitlocker = @()
    protected = @()
}

# --- 1) Коды отказа каждого вызова слоя IOCTL --------------------------------
# Константы из winioctl.h (SDK 10.0.19041). Раньше в журнал попадало только
# «0 разделов»; теперь видно, какой именно вызов отказал и с каким кодом.
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class MrpIoctl {
  [StructLayout(LayoutKind.Sequential)] struct OV { public IntPtr hEvent; public IntPtr a; public IntPtr b; public int off; public int offHi; }
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateFileW(string n,uint a,uint s,IntPtr z,uint d,uint f,IntPtr t);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DeviceIoControl(IntPtr h,uint c,IntPtr i,uint isz,byte[] o,uint osz,out uint r,IntPtr ov);
  [DllImport("kernel32.dll",EntryPoint="DeviceIoControl",SetLastError=true)] static extern bool DeviceIoControlOv(IntPtr h,uint c,IntPtr i,uint isz,byte[] o,uint osz,out uint r,ref OV ov);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool GetDiskFreeSpaceExW(string r,out ulong a,out ulong t,out ulong f);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr FindFirstVolumeW(StringBuilder b,uint n);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool FindNextVolumeW(IntPtr h,StringBuilder b,uint n);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool FindVolumeClose(IntPtr h);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateEventW(IntPtr a,bool m,bool i,string n);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr FindFirstVolumeMountPointW(string root,StringBuilder b,uint n);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool FindNextVolumeMountPointW(IntPtr h,StringBuilder b,uint n);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool GetVolumePathNamesForVolumeNameW(string vol,IntPtr buf,uint n,out uint need);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern uint QueryDosDeviceW(string dev,StringBuilder target,uint n);
  const uint DEVNUM = 0x0041080;   // IOCTL_STORAGE_GET_DEVICE_NUMBER
  const uint LAYOUT = 0x00070050;  // IOCTL_DISK_GET_DRIVE_LAYOUT_EX
  const uint LENGTH = 0x0007405C;  // IOCTL_DISK_GET_LENGTH_INFO
  const uint EXTENT = 0x00040000;  // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
  static IntPtr INV = new IntPtr(-1);
  static string E(){ return "win32=" + Marshal.GetLastWin32Error(); }
  static string Sync(IntPtr h,uint code,string nm){
    byte[] b=new byte[65536]; uint r;
    if(!DeviceIoControl(h,code,IntPtr.Zero,0,b,65536,out r,IntPtr.Zero)) return nm+" FAIL "+E();
    if(code==LAYOUT) return nm+" OK PartitionStyle="+BitConverter.ToUInt32(b,0)+" PartitionCount="+BitConverter.ToUInt32(b,4);
    if(code==LENGTH) return nm+" OK Length="+BitConverter.ToInt64(b,0);
    if(code==DEVNUM) return nm+" OK DeviceType="+BitConverter.ToUInt32(b,0)+" Number="+BitConverter.ToUInt32(b,4);
    if(code==EXTENT) return nm+" OK NumberOfDiskExtents="+BitConverter.ToUInt32(b,0)+" DiskNumber="+BitConverter.ToInt32(b,8);
    return nm+" OK bytes="+r;
  }
  static string Ov(IntPtr h,uint code,string nm){
    IntPtr ev=CreateEventW(IntPtr.Zero,true,false,null);
    OV o; o.hEvent=ev; o.a=IntPtr.Zero; o.b=IntPtr.Zero; o.off=0; o.offHi=0;
    byte[] b=new byte[65536]; uint r;
    if(!DeviceIoControlOv(h,code,IntPtr.Zero,0,b,65536,out r,ref o)){ string e=E(); CloseHandle(ev); return nm+" FAIL "+e; }
    string s;
    if(code==DEVNUM) s=nm+" OK DeviceType="+BitConverter.ToUInt32(b,0)+" Number="+BitConverter.ToUInt32(b,4);
    else if(code==EXTENT) s=nm+" OK NumberOfDiskExtents="+BitConverter.ToUInt32(b,0)+" DiskNumber="+BitConverter.ToInt32(b,8);
    else s=nm+" OK bytes="+r;
    CloseHandle(ev); return s;
  }
  static string TryOpen(string path,uint access,uint flags,bool ov,uint code,string nm){
    IntPtr h=CreateFileW(path,access,3,IntPtr.Zero,3,flags,IntPtr.Zero);
    if(h==INV) return nm+" | open FAIL "+E();
    string s = ov ? Ov(h,code,nm) : Sync(h,code,nm);
    CloseHandle(h); return s;
  }
  // Разделитель в конце пути — предмет проверки, а не украшение: под «\\?\» он не
  // нормализуется. Оба варианта печатаются рядом, чтобы отказ был виден с кодом.
  static string MountPoints(string root){
    StringBuilder sb=new StringBuilder(1024);
    IntPtr h=FindFirstVolumeMountPointW(root,sb,1024);
    if(h==INV) return "FAIL "+E();
    string first=sb.ToString(); int n=1;
    while(FindNextVolumeMountPointW(h,sb,1024)) n++;
    FindVolumeClose(h);
    return "OK first='"+first+"' count="+n;
  }
  static string VolumePathNames(string vol){
    uint need=0;
    IntPtr buf=Marshal.AllocHGlobal(65536);
    try {
      if(!GetVolumePathNamesForVolumeNameW(vol,buf,65536,out need)) return "FAIL "+E();
      return "OK '"+Marshal.PtrToStringUni(buf)+"'";
    } finally { Marshal.FreeHGlobal(buf); }
  }
  static string DosDevice(string dev){
    StringBuilder sb=new StringBuilder(1024);
    uint n=QueryDosDeviceW(dev,sb,1024);
    if(n==0) return "FAIL "+E();
    return "OK '"+sb.ToString().Replace("\0"," | ")+"'";
  }
  public static string[] Probe(string ifacePath,string diskPath,string volumePath){
    var rows=new System.Collections.Generic.List<string>();
    rows.Add(TryOpen(ifacePath,0,0x02000000u,true,DEVNUM,"iface as-is        GET_DEVICE_NUMBER"));
    rows.Add(TryOpen(ifacePath+"\\",0,0x02000000u,true,DEVNUM,"iface +backslash   GET_DEVICE_NUMBER"));
    rows.Add(TryOpen(diskPath,0xC0000000u,0,false,LAYOUT,"disk RW            GET_DRIVE_LAYOUT_EX"));
    rows.Add(TryOpen(diskPath,0x80000000u,0,false,LAYOUT,"disk RD            GET_DRIVE_LAYOUT_EX"));
    rows.Add(TryOpen(diskPath,0x80u,0,false,LAYOUT,"disk ATTR          GET_DRIVE_LAYOUT_EX"));
    rows.Add(TryOpen(diskPath,0xC0000000u,0,false,LENGTH,"disk RW            GET_LENGTH_INFO"));
    rows.Add(TryOpen(diskPath,0x80000000u,0,false,LENGTH,"disk RD            GET_LENGTH_INFO"));
    rows.Add(TryOpen(diskPath,0x80u,0,false,LENGTH,"disk ATTR          GET_LENGTH_INFO"));
    string trimmed = volumePath.EndsWith("\\") ? volumePath.Substring(0, volumePath.Length-1) : volumePath;
    rows.Add(TryOpen(trimmed,0,0x02000000u,true,EXTENT,"volume no-separator GET_VOLUME_DISK_EXTENTS"));
    rows.Add(TryOpen(trimmed+"\\",0,0x02000000u,true,EXTENT,"volume +separator  GET_VOLUME_DISK_EXTENTS"));
    ulong a,t,f;
    rows.Add(GetDiskFreeSpaceExW(trimmed,out a,out t,out f) ? "GetDiskFreeSpaceExW no-separator OK total="+t : "GetDiskFreeSpaceExW no-separator FAIL "+E());
    rows.Add(GetDiskFreeSpaceExW(trimmed+"\\",out a,out t,out f) ? "GetDiskFreeSpaceExW +separator  OK total="+t : "GetDiskFreeSpaceExW +separator  FAIL "+E());
    // Точки монтирования: какой из способов вообще работает на этой машине.
    string guidBare = trimmed.Substring(4, trimmed.Length-4);   // Volume{GUID}
    rows.Add("mount FindFirstVolumeMountPointW 'volumeGuid+'  -> " + MountPoints(volumePath));
    rows.Add("mount FindFirstVolumeMountPointW 'volumeGuid'   -> " + MountPoints(trimmed));
    rows.Add("mount FindFirstVolumeMountPointW 'C:\\'          -> " + MountPoints("C:\\"));
    rows.Add("mount FindFirstVolumeMountPointW '\\\\?\\C:\\'     -> " + MountPoints("\\\\?\\C:\\"));
    rows.Add("mount FindFirstVolumeMountPointW '\\\\.\\C:'      -> " + MountPoints("\\\\.\\C:"));
    rows.Add("mount GetVolumePathNamesForVolumeNameW volumeGuid -> " + VolumePathNames(volumePath));
    rows.Add("mount GetVolumePathNamesForVolumeNameW no-sep     -> " + VolumePathNames(trimmed));
    rows.Add("mount QueryDosDeviceW Volume{GUID}               -> " + DosDevice(guidBare));
    rows.Add("mount QueryDosDeviceW volumeGuid                 -> " + DosDevice(trimmed));
    return rows.ToArray();
  }
  public static string[] EnumerateVolumeGuids(){
    var rows=new System.Collections.Generic.List<string>();
    StringBuilder sb=new StringBuilder(1024);
    IntPtr h=FindFirstVolumeW(sb,1024);
    if(h==INV){ rows.Add("FindFirstVolumeW FAIL "+E()); return rows.ToArray(); }
    do { rows.Add(sb.ToString()); } while(FindNextVolumeW(h,sb,1024));
    FindVolumeClose(h);
    return rows.ToArray();
  }
}
'@
if (-not ('MrpIoctl' -as [type])) {
    Say '[measure] Add-Type не собрал MrpIoctl — коды вызовов IOCTL не сняты'
    exit 4
}

# --- 2) Карта разделов приложения ------------------------------------------------
$sw = [Diagnostics.Stopwatch]::StartNew()
$diskRun = Invoke-Cli @('disks', '--json', '--strict')
$sw.Stop()
$result.diskExit = $diskRun.exit
$text = [string]$diskRun.out
$result.disks = $text
$result.diskStderr = [string]$diskRun.err
Say ("[measure] disks: код {0}, {1} мс, {2} байт JSON" -f $diskRun.exit, $sw.ElapsedMilliseconds, $text.Length)

# Итоги приложения достаём из его же JSON, а не считаем глазами по тексту.
$cliTotals = $null
$doc = Read-AppJson $text
if ($null -ne $doc) {
    $cliTotals = [ordered]@{
        diskCount       = $doc.totals.diskCount
        partitionCount  = $doc.totals.partitionCount
        volumeCount     = $doc.totals.volumeCount
        diskBytes       = $doc.totals.diskBytes
        volumeBytes     = $doc.totals.volumeBytes
        degraded        = $doc.inventory.degraded
        degradedReasons = @($doc.inventory.degradedReasons)
        issues          = @($doc.inventory.issues)
        consistency     = @($doc.consistency)
    }
}
$result.totals = $cliTotals

# Путь интерфейса берём из отчёта приложения: PNPDeviceID из WMI («SCSI\DISK&…»)
# — это идентификатор экземпляра, а открывать нужно «\\?\X#&…». Путь в JSON
# приложения уже проверен разделом devices и не маскируется (маскируются только
# серийники и GUID томов), поэтому он — честный источник.
$ifacePath = ''
if ($null -ne $doc -and $doc.disks.Count -gt 0) { $ifacePath = [string]$doc.disks[0].devicePath }
$probeDisk = '\\.\PhysicalDrive0'
$probeVolume = $null
$volumes = [MrpIoctl]::EnumerateVolumeGuids()
if ($volumes -and $volumes.Count -gt 0 -and -not $volumes[0].StartsWith('FindFirst')) {
    $probeVolume = $volumes[0]
}
if ($ifacePath -and $probeVolume) {
    $result.ioctl = [MrpIoctl]::Probe($ifacePath, $probeDisk, $probeVolume)
} else {
    $result.ioctl = @("не сняты коды: ifacePath='$ifacePath' probeVolume='$probeVolume'")
}
foreach ($row in $result.ioctl) { Say "[ioctl] $row" }


# --- 3) Эталон Windows: Get-Disk / Get-Partition ------------------------------
$ref = [ordered]@{ diskCount = 0; diskBytes = 0; partitionCount = 0; partitionBytes = 0; perDisk = @() }
try {
    $gd = @(Get-Disk -ErrorAction Stop)
    $ref.diskCount = $gd.Count
    $ref.diskBytes = [int64](($gd | Measure-Object -Property Size -Sum).Sum)
    $gp = @(Get-Partition -ErrorAction Stop)
    $ref.partitionCount = $gp.Count
    $ref.partitionBytes = [int64](($gp | Measure-Object -Property Size -Sum).Sum)
    $ref.perDisk = @($gd | ForEach-Object {
        $n = $_.Number
        $parts = @(Get-Partition -DiskNumber $n -ErrorAction SilentlyContinue)
        [ordered]@{
            number         = $n
            sizeBytes      = [int64]$_.Size
            partitionStyle = [string]$_.PartitionStyle
            partitionCount = $parts.Count
            partitionBytes = [int64](($parts | Measure-Object -Property Size -Sum).Sum)
        }
    })
} catch {
    Say "[measure] Get-Disk/Get-Partition недоступны: $_"
}
$result.reference = $ref
Say ("[measure] эталон: дисков {0}, разделов {1}, размер диска {2} байт" -f `
    $ref.diskCount, $ref.partitionCount, $ref.diskBytes)

# --- 4) Сверка: это и есть критерий приёмки -----------------------------------
$match = [ordered]@{
    partitionCountEqual = $false
    diskCountEqual      = $false
    diskBytesEqual      = $false
    partitionBytesEqual = $false
    notes               = @()
}
if ($cliTotals -and $ref.partitionCount -gt 0) {
    $match.partitionCountEqual = ([int]$cliTotals.partitionCount -eq [int]$ref.partitionCount)
    $match.diskCountEqual      = ([int]$cliTotals.diskCount -eq [int]$ref.diskCount)
    $match.diskBytesEqual      = ([int64]$cliTotals.diskBytes -eq [int64]$ref.diskBytes)
    # Сумма размеров разделов меньше диска ровно на служебные области GPT,
    # поэтому сравниваем их друг с другом по разнице, а не по равенству.
    $match.partitionBytesEqual = ($null -ne $cliTotals.diskBytes -and [int64]$cliTotals.diskBytes -ge [int64]$ref.partitionBytes)
    if (-not $match.partitionCountEqual) {
        $match.notes += "разделов: приложение $($cliTotals.partitionCount), Get-Partition $($ref.partitionCount)"
    }
    if ([int64]$cliTotals.diskBytes -ne [int64]$ref.diskBytes) {
        $match.notes += "размер диска: приложение $($cliTotals.diskBytes), Get-Disk $($ref.diskBytes)"
    }
    if ([int64]$cliTotals.diskBytes -lt [int64]$ref.partitionBytes) {
        $match.notes += "сумма разделов $($ref.partitionBytes) больше размера диска $($cliTotals.diskBytes) — не сходится"
    }
}
$result.match = $match
Say ("[measure] СВЕРКА: разделы {0} (прил {1} / Get-Partition {2}); диски {3} (прил {4} / Get-Disk {5}); размер {6} (прил {7} / Get-Disk {8})" -f `
    $(if ($match.partitionCountEqual) { 'СОВПАДАЮТ' } else { 'РАСХОДЯТСЯ' }), $cliTotals.partitionCount, $ref.partitionCount, `
    $(if ($match.diskCountEqual) { 'СОВПАДАЮТ' } else { 'РАСХОДЯТСЯ' }), $cliTotals.diskCount, $ref.diskCount, `
    $(if ($match.diskBytesEqual) { 'СОВПАДАЮТ' } else { 'РАСХОДЯТСЯ' }), $cliTotals.diskBytes, $ref.diskBytes)
foreach ($n in $match.notes) { Say "[measure] расхождение: $n" }

# --- 5) Тайминг полного скана набором правил репозитория (SPEC §12.2) ---------
if ($SkipScan) {
    Say '[measure] скан пропущен (-SkipScan)'
    $result.scanExit = 'skipped'
} else {
    $rules = Join-Path $repo 'rules'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $scanRun = Invoke-Cli @('scan', '--json', '--rules', $rules, '--quiet')
    $sw.Stop()
    $result.scanExit = $scanRun.exit
    $result.scanMs = $sw.ElapsedMilliseconds
    $result.scanKb = [int]([string]$scanRun.out).Length / 1024
    Say ("[measure] scan по набору правил: код {0}, {1} мс, {2} КБ" -f $result.scanExit, $result.scanMs, $result.scanKb)
}

# --- 6) Состояние BitLocker: приложение читает его, но здесь нужно как эталон.
try {
    $bde = & manage-bde -status 2>&1
    $result.bitlocker = @($bde | Where-Object { $_ -match 'Conversion|Protection|Защита|Преобразование|Percentage' } | ForEach-Object { $_.Trim() })
} catch { $result.bitlocker = @("manage-bde недоступен: $_") }

# --- 7) Что приложение считает защищённым: полезно знать, что не является мусором.
foreach ($p in @("$env:SystemRoot", "$env:SystemRoot\System32", 'C:\Program Files', "$env:ProgramData")) {
    if (Test-Path $p) { $result.protected += $p }
}

New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
$result | ConvertTo-Json -Depth 8 | Out-File $out -Encoding utf8
Say "[measure] записано: $out"
Say ("[measure] ИТОГ: разделов {0} (эталон {1}); размер диска {2} байт (эталон {3}); томов {4}" -f `
    $cliTotals.partitionCount, $ref.partitionCount, $cliTotals.diskBytes, $ref.diskBytes, $cliTotals.volumeCount)
if ($match.partitionCountEqual -and $match.diskBytesEqual -and $match.diskCountEqual) {
    Say '[measure] КРИТЕРИЙ ПРИЁМКИ ВЫПОЛНЕН: числа сошлись с Get-Disk/Get-Partition'
    exit 0
}
Say '[measure] КРИТЕРИЙ ПРИЁМКИ НЕ ВЫПОЛНЕН — смотрите расхождения выше и ioctl-коды'
exit 3