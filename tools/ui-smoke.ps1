# MrProper: дымовой прогон окна с проверкой «окно не пустое».
#
# Зачем: окно компилируется, линкуется и запускается, но оболочка могла не
# подключить ни рельс навигации, ни экраны — и всё это выглядит как «приложение
# работает», пока человек не откроет его. Проверка ловит именно это: берёт
# содержимое окна через PrintWindow и считает пиксели, отличные от фона.
# Рамку и заголовок в счёт не берём — их рисует DWM, и пустое окно с заголовком
# проходит наивную проверку «процесс жив» именно на них.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-smoke.ps1 -Exe <путь> [-Shot <png>]
#   ... -Width 1280 -Height 720                    окно 1280x720 логических пикселей
#   ... -Width 1280 -Height 720 -DpiPercent 150    то же при 150 % (окно 1920x1080)
#   ... -ThemeMode light -ExpectBackground light   палитра пришла из темы, а не из константы
#
# -Width/-Height заданы в логических пикселях (DIP, 96 DPI = 1.0), а не в
# физических: при -DpiPercent 150 окно 1280x720 DIP становится окном 1920x1080
# физических пикселей — ровно тот случай, который в задании назван «1280x720 в
# логических пикселях при 150 %». Оба ключа задаются вместе: задать один
# значит спросить про раскладку, которой нет.
#
# -DpiPercent не меняет масштаб экрана: это требует прав администратора и выхода
# из сеанса, а в CI такой возможности нет вовсе. Вместо этого скрипт шлёт
# окну WM_DPICHANGED_AFTERPARENT (0x02E3) — то самое сообщение, которым
# Windows в режиме per-monitor v2 сообщает смену DPI дочерним окнам, и которое
# оболочка проводит через тот же handleDpiChanged, что и WM_DPICHANGED.
# Почему не 0x02E0: отправленный из другого процесса WM_DPICHANGED до окна не
# доходит вообще (проверено: WM_GETMINMAXINFO до и после отправки отдаёт одни и
# те же 900x600), а 0x02E3 доходит и переводит приложение на dpi 144
# (900x600 -> 1350x900). Это эмуляция смены монитора, а не самого монитора:
# нетронутными остаются неклиентская рамка (её рисует система в своём DPI) и
# реальный per-monitor контекст — их проверяет только сеанс с масштабом 150 %.
#
# Числа, а не слова: после отправки скрипт спрашивает у окна WM_GETMINMAXINFO и
# требует, чтобы минимум окна вырос ровно в effDpi/96 раз. Молчаливый «масштаб
# не применился» дал бы зелёные ворота на непроверенной раскладке.
#
# -ThemeMode пишет СВОЙ ключ приложения HKCU\Software\MrProper\UI\ThemeMode
# («light»/«dark»/«auto») и возвращает прежнее значение после прогона. Системная
# тема (Personalize\AppsUseLightTheme) не трогается: это настройка машины, а не
# приложения, и «переключить у себя тему Windows» — не то же самое, что проверить
# тёмную палитру продукта.
#
# Чернила считаются в ДВУХ областях, и это не перестраховка, а смысл проверки.
# Область «клиент» — вся клиентская область окна. Область «содержимое» — только
# окно содержимого (дочернее окно справа от рельса навигации). Рельс рисует
# оболочка сама и нарисует его всегда, поэтому одного клиента достаточно, чтобы
# пропустить ровно тот отказ, который ворота и ловят: окно 1136x795, рельс из
# пяти пунктов, а справа — заливка цветом windowBackground, то есть интерфейса
# нет. Проверено числами: при пустом хосте содержимого в нём 0 чернил, а при
# нарисованном экране — от 16530 (экран очистки, самый пустой из пяти) до
# 103745 (экран настроек, светлая панель). Порог по умолчанию 5000 — втрое
# ниже самого пустого нарисованного экрана и в 5000 раз выше пустого хоста;
# замер сделан тем же алгоритмом (шаг 2, самый частый цвет — фон), что и здесь.
# -MinContentInkPixels 0 отключает проверку содержимого — так меряют только
# оболочку, когда экран наполняет ещё другой слой.
#
# Область содержимого берётся у самого окна: скрипт перечисляет дочерние окна
# и берёт самое крупное видимое (сейчас это ровно одно — хост содержимого,
# 896x760 при окне 1136x795, то есть рельс 240 пикселей). Ширина рельса в коде
# не зашита: как только её станет больше 240, зашитая константа тихо съела бы
# часть содержимого и проверка стала бы зелёной на пустом экране.
#
# Второй критерий на ту же область — число разных цветов. Он ловит то, чего не
# ловит счёт пикселей: окно содержимого на 19 пикселей выше клиентской области
# оставляет свою неприпаркованную полосу видимой снизу и справа, это 13152
# «чернильных» пикселя при 2 цветах — и версия «работает и пуста» прошла бы
# счёт. Нарисованный экран даёт 7..9 цветов даже без ClearType.
#
# Третий критерий — чернила в центральных 70 % окна содержимого. Он ловит
# ложнозелёный случай, когда окно содержимого съехало и накрыло рельс: при
# 150 % DPI оно вылезает на 31 пиксель влево, и 7590 «чернил» с 7 цветами из
# подписей рельса дали код 0 при пустом экране. В центре 70 % у того же окна
# 0 чернил и 1 цвет, у пяти нарисованных экранов — от 409 (очистка) до 29264.
# Порог 250 — в 1,6 раза ниже самой пустой нарисованной страницы.
#
# Коды возврата:
#   0 — окно нарисовало содержимое;
#   2 — неверные аргументы;
#   3 — окно не приняло запрошенный размер или DPI;
#   4 — окно пустое: нет ни клиента, ни содержимого (только фон);
#   5 — окно не создано;
#   6 — процесса нет или он упал;
#   7 — среда не подготовлена (реестр недоступен, нужен -ThemeMode);
#   8 — фон не соответствует ожидаемой теме;
#   9 — окно содержимого не найдено, в нём только фон, в нём меньше
#       -MinContentColors разных цветов или в его центральных 70 % меньше
#       -MinCenterInkPixels чернил.
#
# Прав администратора не требует и не должен: приложение только рисует окно, а
# состояние готовится в HKCU собственного ключа. Порог чернил подобран по
# худшему состоянию — пустому скану («ничего не найдено», а не пустота); см.
# CONTRIBUTING.md, раздел «Ворота интерфейса».
param(
    [string]$Exe = 'build\main\src\ui\Debug\mrproper.exe',
    [string]$Shot = 'D:\Temp\mrproper-window.png',
    [int]$MinInkPixels = 2000,
    [int]$MinContentInkPixels = 5000,
    [int]$MinContentColors = 4,
    [int]$MinCenterInkPixels = 250,
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$DpiPercent = 0,
    [string]$ThemeMode = '',
    [string]$ExpectBackground = '',
    [int]$SettleSeconds = 7,
    [int]$RepaintSeconds = 2
)

$ErrorActionPreference = 'Stop'

# --- проверка аргументов: код 2, а не «сработало на значениях по умолчанию» ---
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "[ui] нет бинарника: $Exe"; exit 6 }
if ($MinInkPixels -lt 0) { Write-Host "[ui] MinInkPixels отрицателен: $MinInkPixels"; exit 2 }
if ($MinContentInkPixels -lt 0) { Write-Host "[ui] MinContentInkPixels отрицателен: $MinContentInkPixels"; exit 2 }
if ($MinContentColors -lt 1) { Write-Host "[ui] MinContentColors меньше 1: $MinContentColors"; exit 2 }
if ($MinCenterInkPixels -lt 0) { Write-Host "[ui] MinCenterInkPixels отрицателен: $MinCenterInkPixels"; exit 2 }
if (($Width -gt 0) -xor ($Height -gt 0)) {
    Write-Host '[ui] -Width и -Height задаются вместе (задано только одно)'
    exit 2
}
if (($Width -le 0) -and ($Height -le 0) -and ($DpiPercent -gt 0)) {
    Write-Host '[ui] -DpiPercent без -Width/-Height: масштабировать нечего'
    exit 2
}
if ($DpiPercent -ne 0 -and ($DpiPercent -lt 50 -or $DpiPercent -gt 500)) {
    Write-Host "[ui] -DpiPercent вне 50..500: $DpiPercent"
    exit 2
}
if ($ThemeMode -ne '' -and @('auto', 'light', 'dark') -notcontains $ThemeMode) {
    Write-Host "[ui] -ThemeMode ожидает auto|light|dark, получено '$ThemeMode'"
    exit 2
}
if ($ExpectBackground -ne '' -and @('dark', 'light') -notcontains $ExpectBackground) {
    Write-Host "[ui] -ExpectBackground ожидает dark|light, получено '$ExpectBackground'"
    exit 2
}

Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public class MrWin {
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 // CharSet.Unicode обязателен: без него среда объявляет вызов как ANSI,
 // маршализер отдаёт буфер как байтовый, а GetWindowTextW пишет туда
 // UTF-16 — заголовок «MrProper» читался как «M» (обрезание по первому
 // нулю), и строка ворота печатала не то название окна.
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
 [DllImport("user32.dll", SetLastError=true)] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
 [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr rect, IntPtr rgn, uint flags);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint timeout, out IntPtr result);
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
 [StructLayout(LayoutKind.Sequential)] public struct MINMAXINFO {
   public POINT ptReserved; public POINT ptMaxSize; public POINT ptMaxPosition;
   public POINT ptMinTrackSize; public POINT ptMaxTrackSize;
 }
}
'@
Add-Type -AssemblyName System.Drawing

# Константы Win32, которые иначе пришлось бы считать в голову:
#   WM_DPICHANGED_AFTERPARENT = 0x02E3 (winuser.h), WM_GETMINMAXINFO = 0x0024,
#   SMTO_ABORTIFHUNG = 0x0002, RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN = 0x0181.
$script:wmDpiAfterParent = 0x02E3
$script:wmGetMinMaxInfo = 0x0024
$script:smtoAbortIfHung = 0x0002
$script:redrawAll = 0x0181
$script:baseDpi = 96

# --- состояние темы в реестре: запомнить, поставить, вернуть как было ---------
$script:themePath = 'HKCU:\Software\MrProper\UI'
$script:themeName = 'ThemeMode'
$script:themeSavedExisted = $false
$script:themeSavedValue = ''
$script:themeKeyExisted = $false

function Save-ThemeState {
    $script:themeKeyExisted = Test-Path -LiteralPath $script:themePath
    try {
        $key = Get-Item -LiteralPath $script:themePath -ErrorAction Stop
        $value = $key.GetValue($script:themeName, $null)
        if ($null -ne $value) {
            $script:themeSavedExisted = $true
            $script:themeSavedValue = [string]$value
        }
    } catch {
        # Ключа нет — это не ошибка: состояние «тема не задана» тоже состояние.
    }
}

function Set-AppThemeMode([string]$mode) {
    if (-not (Test-Path -LiteralPath $script:themePath)) {
        New-Item -Path $script:themePath -Force | Out-Null
    }
    New-ItemProperty -Path $script:themePath -Name $script:themeName -Value $mode `
        -PropertyType String -Force | Out-Null
}

function Restore-AppThemeMode {
    if ($ThemeMode -eq '') { return 0 }
    try {
        if ($script:themeSavedExisted) {
            New-ItemProperty -Path $script:themePath -Name $script:themeName -Value $script:themeSavedValue `
                -PropertyType String -Force | Out-Null
        } elseif (Test-Path -LiteralPath $script:themePath) {
            Remove-ItemProperty -LiteralPath $script:themePath -Name $script:themeName `
                -ErrorAction SilentlyContinue
        }
        # Ключа не было — не оставляем после себя пустой ветки.
        if (-not $script:themeKeyExisted -and (Test-Path -LiteralPath $script:themePath)) {
            $key = Get-Item -LiteralPath $script:themePath
            if ($key.GetValueNames().Length -eq 0 -and $key.GetSubKeyNames().Length -eq 0) {
                Remove-Item -LiteralPath $script:themePath -Force
            }
        }
    } catch {
        Write-Host "[ui] ВНИМАНИЕ: прежний ThemeMode не восстановлен ($($_.Exception.Message))"
        return 7
    }
    return 0
}

# Относительная яркость по WCAG: решением «тёмный фон или светлый» занимается
# не цветовой канал, а формула, иначе тёмно-синий прошёл бы за серый.
function Get-RelativeLuminance([int]$r, [int]$g, [int]$b) {
    $channel = {
        param($value)
        $c = $value / 255.0
        if ($c -le 0.03928) { return $c / 12.92 }
        return [Math]::Pow(($c + 0.055) / 1.055, 2.4)
    }
    return 0.2126 * (& $channel $r) + 0.7152 * (& $channel $g) + 0.0722 * (& $channel $b)
}

function Find-AppWindow([int]$processId) {
    $script:hwnd = [IntPtr]::Zero
    $script:hwndClass = ''
    $cb = [MrWin+EnumProc] {
        param($h, $l)
        $q = 0
        [void][MrWin]::GetWindowThreadProcessId($h, [ref]$q)
        if ($q -eq $processId) {
            $t = New-Object Text.StringBuilder 512
            [void][MrWin]::GetWindowTextW($h, $t, 512)
            $r = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($h, [ref]$r)
            if ($script:hwnd -eq [IntPtr]::Zero -and [MrWin]::IsWindowVisible($h) -and
                ($r.R - $r.L) -gt 200 -and ($r.B - $r.T) -gt 200 -and $t.Length -gt 0) {
                $script:hwnd = $h
                $c = New-Object Text.StringBuilder 512
                [void][MrWin]::GetClassNameW($h, $c, 512)
                $script:hwndClass = $c.ToString()
                Write-Host ("[ui] окно '{0}' класс '{1}' {2}x{3}" -f $t.ToString(), $script:hwndClass, ($r.R - $r.L), ($r.B - $r.T))
            }
        }
        return $true
    }
    [void][MrWin]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:hwnd
}

# Окно содержимого — самое крупное видимое дочернее окно главного. Рельс
# навигации рисует оболочка на самом окне, дочерних окон у него одно: хост
# содержимого, в который views_* и рисуют. Возвращает @{ Found; X; Y; W; H } в
# координатах СНИМКА окна (то есть уже с вычетом неклиентской рамки).
function Find-ContentHost([IntPtr]$hwnd, $windowRect) {
    $script:bestArea = 0
    $script:best = $null
    $cb = [MrWin+EnumProc] {
        param($h, $l)
        if ([MrWin]::IsWindowVisible($h)) {
            $r = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($h, [ref]$r)
            $area = ($r.R - $r.L) * ($r.B - $r.T)
            if ($area -gt $script:bestArea) {
                $script:bestArea = $area
                $script:best = New-Object MrWin+RECT
                $script:best.L = $r.L
                $script:best.T = $r.T
                $script:best.R = $r.R
                $script:best.B = $r.B
            }
        }
        return $true
    }
    [void][MrWin]::EnumChildWindows($hwnd, $cb, [IntPtr]::Zero)
    if ($null -eq $script:best) {
        return @{ Found = $false; X = 0; Y = 0; W = 0; H = 0 }
    }
    $x = $script:best.L - $windowRect.L
    $y = $script:best.T - $windowRect.T
    return @{ Found = $true; X = $x; Y = $y; W = $script:best.R - $script:best.L; H = $script:best.B - $script:best.T }
}

# Считает чернила в прямоугольнике: пиксели, отличные от самого частого цвета
# внутри области. Шаг 2 по обеим осям — четверть выборки (на 1920x1080 это
# 494 тысячи точек, полный обход в PowerShell 5.1 занял бы минуты, а решение
# «нарисован интерфейс или нет» от числа точек не зависит).
function Measure-Ink($bytes, $stride, $x0, $y0, $x1, $y1) {
    $counts = @{}
    $samples = 0
    for ($y = $y0; $y -lt $y1; $y += 2) {
        $row = $y * $stride
        for ($x = $x0; $x -lt $x1; $x += 2) {
            $i = $row + $x * 4
            $key = "$($bytes[$i]),$($bytes[$i + 1]),$($bytes[$i + 2])"
            if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
            $samples++
        }
    }
    if ($samples -eq 0) { return @{ Background = ''; Ink = 0; Samples = 0; Ratio = 0.0; Colors = 0 } }
    $background = ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Name
    $ink = 0
    foreach ($kv in $counts.GetEnumerator()) {
        if ($kv.Name -ne $background) { $ink += $kv.Value }
    }
    return @{ Background = $background; Ink = $ink; Samples = $samples;
              Ratio = [Math]::Round(100.0 * $ink / $samples, 2); Colors = $counts.Count }
}

# Минимум окна, объявленный самим приложением (AppShell::handleGetMinMaxInfo
# отдаёт ptMinTrackSize = логический минимум, умноженный на dpi_/96). Число
# служит измерителем внутреннего DPI: при 96 dpi это 900x600, при 144 — 1350x900.
function Get-AppMinTrack([IntPtr]$hwnd) {
    $info = New-Object MrWin+MINMAXINFO
    $size = [Runtime.InteropServices.Marshal]::SizeOf($info)
    $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($size)
    try {
        $info.ptMaxSize.X = -1; $info.ptMaxSize.Y = -1
        $info.ptMaxPosition.X = -1; $info.ptMaxPosition.Y = -1
        $info.ptMinTrackSize.X = -1; $info.ptMinTrackSize.Y = -1
        $info.ptMaxTrackSize.X = -1; $info.ptMaxTrackSize.Y = -1
        [Runtime.InteropServices.Marshal]::StructureToPtr($info, $ptr, $false)
        $result = [IntPtr]::Zero
        $sent = [MrWin]::SendMessageTimeout($hwnd, $script:wmGetMinMaxInfo, [IntPtr]::Zero, $ptr,
                                            $script:smtoAbortIfHung, 5000, [ref]$result)
        if ($sent -eq [IntPtr]::Zero) { return $null }
        $back = [Runtime.InteropServices.Marshal]::PtrToStructure($ptr, [type][MrWin+MINMAXINFO])
        if ($back.ptMinTrackSize.X -le 0 -or $back.ptMinTrackSize.Y -le 0) { return $null }
        return @([int]$back.ptMinTrackSize.X, [int]$back.ptMinTrackSize.Y)
    } finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
    }
}

function Stop-AppProcess($proc) {
    if ($null -eq $proc) { return }
    if ($proc.HasExited) { return }
    [void]$proc.CloseMainWindow()
    Start-Sleep -Seconds 3
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        Write-Host '[ui] закрыт принудительно'
    } else {
        Write-Host "[ui] закрылся сам, код $($proc.ExitCode)"
    }
}

function Invoke-Smoke {
    $proc = Start-Process $Exe -PassThru
    try {
        Start-Sleep -Seconds $SettleSeconds
        if ($proc.HasExited) { Write-Host "[ui] процесс упал, код $($proc.ExitCode)"; return 6 }

        $hwnd = Find-AppWindow $proc.Id
        if ($hwnd -eq [IntPtr]::Zero) { Write-Host '[ui] окно не найдено'; return 5 }

        $realDpi = [MrWin]::GetDpiForWindow($hwnd)
        if ($realDpi -eq 0) { $realDpi = $script:baseDpi }
        $effDpi = $realDpi
        $minTrackBefore = Get-AppMinTrack $hwnd
        if ($null -eq $minTrackBefore) {
            Write-Host '[ui] ПРОВАЛ: окно не ответило на WM_GETMINMAXINFO — нечем подтвердить масштаб'
            return 3
        }
        Write-Host ("[ui] минимум окна до эмуляции: {0}x{1} (системный dpi окна: {2})" -f `
            $minTrackBefore[0], $minTrackBefore[1], $realDpi)

        if ($DpiPercent -gt 0) {
            $effDpi = [int][Math]::Round($DpiPercent * $script:baseDpi / 100.0)
            $wp = [IntPtr](($effDpi -band 0xFFFF) -bor ($effDpi -shl 16))
            $result = [IntPtr]::Zero
            $sent = [MrWin]::SendMessageTimeout($hwnd, $script:wmDpiAfterParent, $wp, [IntPtr]::Zero,
                                                 $script:smtoAbortIfHung, 5000, [ref]$result)
            Start-Sleep -Milliseconds 300
            $minTrackAfter = Get-AppMinTrack $hwnd
            if ($null -eq $minTrackAfter) {
                Write-Host '[ui] ПРОВАЛ: после WM_DPICHANGED_AFTERPARENT окно перестало отвечать'
                return 3
            }
            $wantX = [int][Math]::Round($minTrackBefore[0] * $effDpi / [double]$realDpi)
            $wantY = [int][Math]::Round($minTrackBefore[1] * $effDpi / [double]$realDpi)
            Write-Host ("[ui] DPI {0} (запрошено {1}%), минимум окна {2}x{3}, ждали {4}x{5}" -f `
                $effDpi, $DpiPercent, $minTrackAfter[0], $minTrackAfter[1], $wantX, $wantY)
            if ($sent -eq [IntPtr]::Zero -and ([Math]::Abs($minTrackAfter[0] - $wantX) -gt 4 -or
                                               [Math]::Abs($minTrackAfter[1] - $wantY) -gt 4)) {
                Write-Host '[ui] ПРОВАЛ: приложение не приняло эмуляцию DPI — раскладка не проверена'
                return 3
            }
            if ([Math]::Abs($minTrackAfter[0] - $wantX) -gt 4 -or
                [Math]::Abs($minTrackAfter[1] - $wantY) -gt 4) {
                Write-Host '[ui] ПРОВАЛ: минимум окна не вырос в effDpi/96 раз'
                return 3
            }
        }

        if ($Width -gt 0 -and $Height -gt 0) {
            $targetW = [int][Math]::Round($Width * $effDpi / [double]$script:baseDpi)
            $targetH = [int][Math]::Round($Height * $effDpi / [double]$script:baseDpi)
            [void][MrWin]::MoveWindow($hwnd, 40, 40, $targetW, $targetH, $true)
            Start-Sleep -Milliseconds 400
            $after = New-Object MrWin+RECT
            [void][MrWin]::GetWindowRect($hwnd, [ref]$after)
            $gotW = $after.R - $after.L
            $gotH = $after.B - $after.T
            Write-Host ("[ui] запросили окно {0}x{1} ({2}x{3} DIP при dpi {4}), получили {5}x{6}" -f `
                $targetW, $targetH, $Width, $Height, $effDpi, $gotW, $gotH)
            if ([Math]::Abs($gotW - $targetW) -gt 2 -or [Math]::Abs($gotH - $targetH) -gt 2) {
                Write-Host '[ui] ПРОВАЛ: окно не приняло запрошенный размер'
                return 3
            }
            if ($proc.HasExited) { Write-Host '[ui] процесс упал после смены размера'; return 6 }
        }

        # Снимок берём после принудительной перерисовки всей иерархии: иначе
        # PrintWindow успевает до WM_PAINT и ловит предыдущий кадр.
        #
        # Окно к этому моменту может оказаться свёрнутым: на машине, где
        # параллельно работают другие сессии, фокус уходит, и приложение (или
        # система) сворачивает окно. GetWindowRect у свёрнутого окна отдаёт
        # иконку 160x24 в точке (-32000,-32000) — снимок такого окна дал бы
        # «11% чернил в окне 160x24» и код 8 вместо честного отказа. Поэтому
        # перед съёмкой окно разворачивается, а размер проверяется повторно.
        if ([MrWin]::IsIconic($hwnd)) {
            Write-Host '[ui] окно оказалось свёрнутым — разворачиваем (SW_RESTORE)'
            [void][MrWin]::ShowWindow($hwnd, 9)
            Start-Sleep -Milliseconds 400
        }
        Start-Sleep -Seconds $RepaintSeconds
        [void][MrWin]::RedrawWindow($hwnd, [IntPtr]::Zero, [IntPtr]::Zero, $script:redrawAll)
        [void][MrWin]::UpdateWindow($hwnd)
        Start-Sleep -Milliseconds 300

        $rect = New-Object MrWin+RECT
        [void][MrWin]::GetWindowRect($hwnd, [ref]$rect)
        $w = $rect.R - $rect.L
        $h = $rect.B - $rect.T
        if ($w -lt 200 -or $h -lt 200) {
            Write-Host ("[ui] ПРОВАЛ: к моменту съёмки окно имеет размер {0}x{1} — это не окно приложения" -f $w, $h)
            return 3
        }
        $shotDir = Split-Path -Parent $Shot
        if ($shotDir -ne '' -and -not (Test-Path -LiteralPath $shotDir)) {
            New-Item -ItemType Directory -Path $shotDir -Force | Out-Null
        }
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc()
        $ok = [MrWin]::PrintWindow($hwnd, $dc, 2)
        $g.ReleaseHdc($dc)
        $g.Dispose()
        $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)

        # Клиентская область в координатах снимка окна: рамку и заголовок рисует DWM,
        # и без отсечения пустое окно проходит проверку (самый частый цвет тогда —
        # белый заголовок плюс тёмная рамка).
        $origin = New-Object MrWin+POINT
        $origin.X = 0; $origin.Y = 0
        [void][MrWin]::ClientToScreen($hwnd, [ref]$origin)
        $cx = $origin.X - $rect.L
        $cy = $origin.Y - $rect.T
        $cw = $w - 2 * [Math]::Max(0, $cx)
        $ch = $h - 2 * [Math]::Max(0, $cy)
        if ($cw -le 0 -or $ch -le 0) { $cx = 0; $cy = 0; $cw = $w; $ch = $h }

        $data = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $w, $h)),
                              [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                              [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $stride = $data.Stride
        $bytes = New-Object byte[] ($stride * $h)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $bmp.UnlockBits($data)
        $bmp.Dispose()
        Write-Host "[ui] клиентская область ${cw}x${ch} (окно ${w}x${h})"

        # Считаем «чернила»: пиксели, отличные от самого частого цвета (фона).
        $client = Measure-Ink $bytes $stride $cx $cy ($cx + $cw) ($cy + $ch)
        $background = $client.Background
        $ink = $client.Ink
        $samples = $client.Samples
        $ratio = $client.Ratio
        Write-Host ("[ui] printwindow=$ok размер=${w}x${h} фон=$background чернила=$ink из $samples точек (${ratio}%, порог $MinInkPixels)")

        if ($ExpectBackground -ne '') {
            $parts = $background.Split(',')
            $lum = Get-RelativeLuminance ([int]$parts[0]) ([int]$parts[1]) ([int]$parts[2])
            Write-Host ("[ui] яркость фона {0} (ожидалась тема {1})" -f [Math]::Round($lum, 4), $ExpectBackground)
            # Границы 0.03 и 0.50 разведены в 16 раз, а крайние палитры дают
            # 0.013 (тёмная 0x20) и 0.886 (светлая 0xF3).
            $bad = if ($ExpectBackground -eq 'dark') { $lum -ge 0.03 } else { $lum -lt 0.50 }
            if ($bad) {
                Write-Host "[ui] ПРОВАЛ: фон не соответствует теме $ExpectBackground — палитра пришла не из темы"
                return 8
            }
        }

        if ($ink -lt $MinInkPixels) {
            Write-Host "[ui] ПРОВАЛ: окно показывает только фон — интерфейс не нарисован. Снимок: $Shot"
            return 4
        }

        # --- содержимое: окно содержимого справа от рельса ----------------------
        # Клиентских чернил мало, чтобы отличить «интерфейс есть» от «нарисован
        # только рельс»: рельс — это всегда ~17% клиента при 1280x720. Содержимое
        # проверяется отдельно, иначе версия «работает и пуста» снова пройдёт.
        if ($MinContentInkPixels -gt 0) {
            $contentWindow = Find-ContentHost $hwnd $rect
            if (-not $contentWindow.Found) {
                Write-Host ("[ui] ПРОВАЛ: у окна нет дочернего окна содержимого — нечего проверять. Снимок: {0}" -f $Shot)
                return 9
            }
            # Область содержимого обрезается по клиенту: дочернее окно не может
            # вылезать за него, а лишние пиксели рамки в подсчёте только шумят.
            $hx = [Math]::Max($cx, $contentWindow.X)
            $hy = [Math]::Max($cy, $contentWindow.Y)
            $hx2 = [Math]::Min($cx + $cw, $contentWindow.X + $contentWindow.W)
            $hy2 = [Math]::Min($cy + $ch, $contentWindow.Y + $contentWindow.H)
            Write-Host ("[ui] окно содержимого {0}x{1} в ({2},{3}), в снимке {4}x{5}" -f `
                $contentWindow.W, $contentWindow.H, $contentWindow.X, $contentWindow.Y, ($hx2 - $hx), ($hy2 - $hy))
            if (($hx2 - $hx) -le 0 -or ($hy2 - $hy) -le 0) {
                Write-Host '[ui] ПРОВАЛ: окно содержимого не попало в клиентскую область'
                return 9
            }
            $content = Measure-Ink $bytes $stride $hx $hy $hx2 $hy2
            Write-Host ("[ui] содержимое: фон={0} чернила={1} из {2} точек ({3}%, порог {4}) цветов={5}" -f `
                $content.Background, $content.Ink, $content.Samples, $content.Ratio, $MinContentInkPixels, $content.Colors)
            if ($content.Ink -lt $MinContentInkPixels) {
                Write-Host ("[ui] ПРОВАЛ: рельс нарисован ({0} чернил), но окно содержимого пустое — экран не отрисован. Снимок: {1}" -f $ink, $Shot)
                return 9
            }
            if ($content.Colors -lt $MinContentColors) {
                # Плоская заливка плюс рамка — это не экран. Проверено числами: у
                # пустого хоста 1 цвет, у артефакта «дочернее окно выше клиентской
                # на 19 пикселей, его неприпаркованный фон виден белой полосой
                # снизу и справа» — 2 цвета, у пяти нарисованных экранов — 7..9
                # даже без ClearType (с ним больше). Порог 4 отделён от обоих.
                Write-Host ("[ui] ПРОВАЛ: в окне содержимого {0} цветов (порог {1}) — это плоская заливка, а не экран. Снимок: {2}" -f `
                    $content.Colors, $MinContentColors, $Shot)
                return 9
            }

            # Центральные 70 % окна содержимого. Ловят класс «счёт прошёл, а
            # экрана нет»: при 150 % DPI окно содержимого съезжает влево на
            # 31 пиксель и накрывает правый край рельса — 7590 «чернил» и 7
            # цветов из подписей рельса давали зелёный код при пустом экране.
            # В центре 70 % у того же окна 0 чернил и 1 цвет, а у пяти
            # нарисованных экранов — от 409 (очистка, самая пустая) до 29264.
            # Порог 250 — в 1,6 раза ниже самой пустой нарисованной страницы.
            $centerShare = 0.70
            $mx0 = $hx + [int](($hx2 - $hx) * (1.0 - $centerShare) / 2.0)
            $mx1 = $hx2 - [int](($hx2 - $hx) * (1.0 - $centerShare) / 2.0)
            $my0 = $hy + [int](($hy2 - $hy) * (1.0 - $centerShare) / 2.0)
            $my1 = $hy2 - [int](($hy2 - $hy) * (1.0 - $centerShare) / 2.0)
            $center = Measure-Ink $bytes $stride $mx0 $my0 $mx1 $my1
            Write-Host ("[ui] центр 70%: {0}x{1} чернила={2} из {3} точек ({4}%, порог {5})" -f `
                ($mx1 - $mx0), ($my1 - $my0), $center.Ink, $center.Samples, $center.Ratio, $MinCenterInkPixels)
            if ($center.Ink -lt $MinCenterInkPixels) {
                Write-Host ("[ui] ПРОВАЛ: в центре окна содержимого {0} чернил (порог {1}) — экран не отрисован, края закрашены. Снимок: {2}" -f `
                    $center.Ink, $MinCenterInkPixels, $Shot)
                return 9
            }
        }

        Write-Host "[ui] окно нарисовало содержимое. Снимок: $Shot"
        return 0
    } finally {
        Stop-AppProcess $proc
    }
}

$themeReady = 0
if ($ThemeMode -ne '') {
    Save-ThemeState
    try {
        Set-AppThemeMode $ThemeMode
        Write-Host "[ui] режим темы приложения: $ThemeMode (системную тему не трогаем)"
    } catch {
        Write-Host "[ui] ПРОВАЛ: не удалось подготовить режим темы: $($_.Exception.Message)"
        $themeReady = 7
    }
}

$code = if ($themeReady -eq 0) { Invoke-Smoke } else { $themeReady }
$restored = Restore-AppThemeMode
if ($code -eq 0 -and $restored -ne 0) { $code = $restored }
exit $code
