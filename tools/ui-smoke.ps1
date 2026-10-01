# MrProper: дымовой прогон окна с проверкой «окно не пустое».
#
# Зачем: окно компилируется, линкуется и запускается, но оболочка могла не
# подключить ни рельс навигации, ни экраны — и всё это выглядит как «приложение
# работает», пока человек не откроет его. Проверка ловит именно это: берёт
# содержимое окна через PrintWindow и считает пиксели, отличные от фона.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-smoke.ps1 -Exe <путь> [-Shot <png>]
#
# Коды возврата: 0 — окно нарисовало содержимое; 4 — окно пустое (только фон);
# 5 — окно не создано; 6 — процесс упал.
param(
    [string]$Exe = 'build\main\src\ui\Release\mrproper.exe',
    [string]$Shot = 'D:\Temp\mrproper-window.png',
    [int]$MinInkPixels = 2000
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { Write-Host "[ui] нет бинарника: $Exe"; exit 6 }

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class MrWin {
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -AssemblyName System.Drawing

$proc = Start-Process $Exe -PassThru
Start-Sleep -Seconds 7
if ($proc.HasExited) { Write-Host "[ui] процесс упал, код $($proc.ExitCode)"; exit 6 }

$hwnd = [IntPtr]::Zero
$cb = [MrWin+EnumProc] {
    param($h, $l)
    $q = 0
    [void][MrWin]::GetWindowThreadProcessId($h, [ref]$q)
    if ($q -eq $proc.Id) {
        $t = New-Object Text.StringBuilder 512
        [void][MrWin]::GetWindowTextW($h, $t, 512)
        $r = New-Object MrWin+RECT
        [void][MrWin]::GetWindowRect($h, [ref]$r)
        if ($script:hwnd -eq [IntPtr]::Zero -and [MrWin]::IsWindowVisible($h) -and
            ($r.R - $r.L) -gt 200 -and ($r.B - $r.T) -gt 200 -and $t.Length -gt 0) {
            $script:hwnd = $h
            Write-Host ("[ui] окно '{0}' {1}x{2}" -f $t.ToString(), ($r.R - $r.L), ($r.B - $r.T))
        }
    }
    return $true
}
[void][MrWin]::EnumWindows($cb, [IntPtr]::Zero)

if ($script:hwnd -eq [IntPtr]::Zero) {
    [void]$proc.CloseMainWindow(); Start-Sleep -Seconds 2
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Write-Host '[ui] окно не найдено'
    exit 5
}

$rect = New-Object MrWin+RECT
[void][MrWin]::GetWindowRect($script:hwnd, [ref]$rect)
$w = $rect.R - $rect.L
$h = $rect.B - $rect.T
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [MrWin]::PrintWindow($script:hwnd, $dc, 2)
$g.ReleaseHdc($dc)
$g.Dispose()
$bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)

# Считаем «чернила»: пиксели, отличные от самого частого цвета (фона).
$ink = 0
$data = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $w, $h)),
                      [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                      [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$stride = $data.Stride
$bytes = New-Object byte[] ($stride * $h)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$bmp.UnlockBits($data)
$bmp.Dispose()

# Клиентская область в координатах снимка окна: рамку и заголовок рисует DWM,
# и без отсечения пустое окно проходит проверку (самый частый цвет тогда — белый
# заголовка плюс тёмная рамка).
$origin = New-Object MrWin+POINT
$origin.X = 0; $origin.Y = 0
[void][MrWin]::ClientToScreen($script:hwnd, [ref]$origin)
$cx = $origin.X - $rect.L
$cy = $origin.Y - $rect.T
$cw = $w - 2 * [Math]::Max(0, $cx)
$ch = $h - 2 * [Math]::Max(0, $cy)
if ($cw -le 0 -or $ch -le 0) { $cx = 0; $cy = 0; $cw = $w; $ch = $h }
Write-Host "[ui] клиентская область ${cw}x${ch} (окно ${w}x${h})"

$counts = @{}
for ($y = $cy; $y -lt ($cy + $ch); $y += 2) {
    $row = $y * $stride
    for ($x = $cx; $x -lt ($cx + $cw); $x += 2) {
        $i = $row + $x * 4
        $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
        if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
    }
}
$background = ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
foreach ($kv in $counts.GetEnumerator()) {
    if ($kv.Name -ne $background) { $ink += $kv.Value }
}

Write-Host ("[ui] printwindow=$ok размер=${w}x${h} фон=$background чернила=$ink (порог $MinInkPixels)")

[void]$proc.CloseMainWindow(); Start-Sleep -Seconds 3
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; Write-Host '[ui] закрыт принудительно' }
else { Write-Host "[ui] закрылся сам, код $($proc.ExitCode)" }

if ($ink -lt $MinInkPixels) {
    Write-Host "[ui] ПРОВАЛ: окно показывает только фон — интерфейс не нарисован. Снимок: $Shot"
    exit 4
}
Write-Host "[ui] окно нарисовало содержимое. Снимок: $Shot"
exit 0