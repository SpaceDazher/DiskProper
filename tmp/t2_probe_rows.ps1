# T2 probe: числа D-75 по факту, на ОДНОЙ странице, без обхода -AllPages.
#
# Зачем проба, если есть tools\ui-smoke.ps1: обход всех страниц переключает их
# клавишами Ctrl+1..5, то есть крадёт фокус, а переднее окно на этой машине
# принадлежит чужому процессу (chrome) — ворота честно дают отказ. Для замера
# одной страницы обход не нужен: страница выбирается через
# HKCU\Software\MrProper\UI\nav.current, и ворота запускаются БЕЗ -AllPages.
#
# Чего эта проба НЕ делает: она не переписывает измерение. Все функции измерения
# (Measure-ListItem, Measure-RowArea, Measure-RowText, Measure-HeaderColumns,
# Measure-DominantColor, Find-ContentHost, Save-WindowShot) ВЫРЕЗАЮТСЯ из
# tools\ui-smoke.ps1 при запуске и выполняются здесь как есть. Поэтому числа
# пробы сопоставимы с числами ворот по построению, а не по моему пересказу, и
# правка tools\ui-smoke.ps1 автоматически меняет и пробу.
#
# Проба берёт на себя ровно то, чего ворота без -AllPages не делают: обход
# потомков хоста содержимого и вызов Measure-ListItem для каждого
# SysListView32/SysTreeView32 — с теми же порогами ($MinListTextPixels,
# $MinListTextSpanPercent, $MinColumnWidthPx, $LayoutTolerance), что и в воротах.
# Передний план не трогается: окно не выводится наверх, ввод не шлётся.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tmp\t2_probe_rows.ps1 ^
#       -Exe build\s2\src\ui\Debug\mrproper.exe -Page disks -ThemeMode dark
#   ... -ThemeMode '' -Page settings   (светлая/системная тема как есть)
#
# Реестр: nav.current и ThemeMode ставятся на время пробы и возвращаются
# как были (включая «ключа не было»). window.placement тоже сохраняется и
# возвращается: приложение пишет его само при закрытии.
param(
    [string]$Exe = 'build\s2\src\ui\Debug\mrproper.exe',
    [string]$Gate = 'tools\ui-smoke.ps1',
    [string]$Page = 'disks',
    [string]$ThemeMode = 'dark',
    [int]$Width = 900,
    [int]$Height = 600,
    [string]$ShotPath = 'D:\Temp\t2-probe-page.png',
    [int]$SettleSeconds = 7,
    [int]$WindowFindTries = 10
)

$ErrorActionPreference = 'Stop'

# --- пороги ворот: те же значения, что в параметрах ui-smoke.ps1 --------------
$MinListTextPixels = 400
$MinListTextSpanPercent = 3.0
$MinColumnWidthPx = 24
$LayoutTolerance = 1
$KeepUiState = $true
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$Gate = (Resolve-Path -LiteralPath $Gate).Path

# --- вырезка функций ворот (read-only: сам tools\ui-smoke.ps1 не меняется) ---
$gateText = Get-Content -LiteralPath $Gate -Raw -Encoding UTF8
$gateLines = $gateText -split "`r?`n"
$start = -1
$stop = -1
for ($i = 0; $i -lt $gateLines.Count; $i++) {
    if ($start -lt 0 -and $gateLines[$i] -match "^Add-Type -TypeDefinition @'") { $start = $i }
    if ($start -ge 0 -and $gateLines[$i] -match "^'@") {
        # после хвостового '@ в воротах идёт Add-Type -AssemblyName
        # System.Drawing: без него New-Object Bitmap не находит тип, а снимок
        # окна без него снять нечем.
        $stop = $i
        if (($i + 1) -lt $gateLines.Count -and $gateLines[$i + 1] -match '^Add-Type -AssemblyName') { $stop = $i + 1 }
        break
    }
}
if ($start -lt 0 -or $stop -lt 0) { Write-Host '[t2] в воротах не найден блок Add-Type'; exit 2 }
$typeChunk = ($gateLines[$start..$stop] -join "`n")

Invoke-Expression $typeChunk

# Константы ворот: все операторы $script:* верхнего уровня, каждый до баланса
# скобок (список $script:pages многострочный). Внутри функций присваивания
# сдвинуты в столбцы, поэтому в этот список они не попадают.
$constCount = 0
$i = 0
while ($i -lt $gateLines.Count) {
    if ($gateLines[$i] -match '^\$script:[A-Za-z0-9_]+\s*=') {
        $stmt = @($gateLines[$i])
        $depth = ([regex]::Matches($gateLines[$i], '[\(\{\[]')).Count - ([regex]::Matches($gateLines[$i], '[\)\}\]]')).Count
        while ($depth -gt 0 -and ($i + 1) -lt $gateLines.Count) {
            $i++
            $stmt += $gateLines[$i]
            $depth += ([regex]::Matches($gateLines[$i], '[\(\{\[]')).Count - ([regex]::Matches($gateLines[$i], '[\)\}\]]')).Count
        }
        Invoke-Expression ($stmt -join "`n")
        $constCount++
    }
    $i++
}

# Функции ворот: каждый блок от 'function NAME' до строки '}' в нулевом столбце.
# Вокруг функций в том же файле лежат операторы верхнего уровня (проверка
# аргументов, ветки -AllPages), поэтому файл целиком выполнять нельзя — это
# вторая причина, по которой нужна проба, а не просто 'вызвать ворота'.
$names = @()
for ($i = 0; $i -lt $gateLines.Count; $i++) {
    if ($gateLines[$i] -match '^function ([A-Za-z0-9\-]+)') {
        $name = $Matches[1]
        if ($name -in @('Stop-AppProcess', 'Start-AppProcess', 'Invoke-Smoke', 'Invoke-DeterminismRuns', 'Switch-AppPage', 'Set-AppForeground', 'Restore-AppThemeMode', 'Restore-AppFontScale', 'Set-AppFontScale', 'Set-AppThemeMode')) { continue }
        $end = -1
        for ($j = $i + 1; $j -lt $gateLines.Count; $j++) {
            if ($gateLines[$j] -eq '}') { $end = $j; break }
        }
        if ($end -lt 0) { Write-Host ("[t2] функция {0} не закрыта" -f $name); exit 2 }
        Invoke-Expression ($gateLines[$i..$end] -join "`n")
        $names += $name
    }
}
Write-Host ("[t2] ворота: {0}; типы со строк {1}..{2}; констант {3}; выполнено функций {4}: {5}" -f `
    $Gate, ($start + 1), ($stop + 1), $constCount, $names.Count, ($names -join ','))

# --- состояние в реестре: сохранить, поставить, вернуть ------------------------
$script:themePath = 'HKCU:\Software\MrProper\UI'
$saved = @{}
foreach ($name in @('nav.current', 'ThemeMode', 'window.placement')) {
    $value = $null
    if (Test-Path -LiteralPath $script:themePath) {
        $value = (Get-Item -LiteralPath $script:themePath).GetValue($name, $null)
    }
    $saved[$name] = $value
}
function Restore-Registry {
    foreach ($name in $saved.Keys) {
        $was = $saved[$name]
        if ($null -eq $was) {
            Remove-ItemProperty -LiteralPath $script:themePath -Name $name -ErrorAction SilentlyContinue
        } else {
            New-ItemProperty -Path $script:themePath -Name $name -Value $was -PropertyType String -Force | Out-Null
        }
    }
    Write-Host ('[t2] реестр возвращён: ' + (($saved.Keys | ForEach-Object {
        "$_=" + $(if ($null -eq $saved[$_]) { '<не было>' } else { $saved[$_] }) }) -join '; '))
}
if (-not (Test-Path -LiteralPath $script:themePath)) {
    New-Item -Path $script:themePath -Force | Out-Null
}
New-ItemProperty -Path $script:themePath -Name 'nav.current' -Value $Page -PropertyType String -Force | Out-Null
if ($ThemeMode -ne '') {
    New-ItemProperty -Path $script:themePath -Name 'ThemeMode' -Value $ThemeMode -PropertyType String -Force | Out-Null
}
Write-Host ("[t2] страница={0} тема={1} окно={2}x{3} бинарник={4} (собран {5})" -f `
    $Page, $(if ($ThemeMode -eq '') { '<как в системе>' } else { $ThemeMode }), $Width, $Height, `
    $Exe, (Get-Item -LiteralPath $Exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))
Write-Host ('[t2] начало пробы: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

$proc = $null
$code = 0
try {
    $proc = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Seconds $SettleSeconds
    $hwnd = [IntPtr]::Zero
    for ($try = 1; $try -le $WindowFindTries; $try++) {
        $hwnd = Find-AppWindow $proc.Id
        if ($hwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 700
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        Write-Host ('[t2] ПРОВАЛ: окно не найдено за {0} попытками' -f $WindowFindTries)
        $code = 5
    } else {
        [void][MrWin]::MoveWindow($hwnd, 40, 40, $Width, $Height, $true)
        Start-Sleep -Seconds 2
        [void][MrWin]::RedrawWindow($hwnd, [IntPtr]::Zero, [IntPtr]::Zero, $script:redrawAll)
        [void][MrWin]::UpdateWindow($hwnd)
        Start-Sleep -Seconds 2
        $rect = New-Object MrWin+RECT
        [void][MrWin]::GetWindowRect($hwnd, [ref]$rect)
        $w = $rect.R - $rect.L
        $h = $rect.B - $rect.T
        Write-Host ("[t2] окно после MoveWindow: {0}x{1}" -f $w, $h)
        if ($w -lt 200 -or $h -lt 200) { Write-Host '[t2] ПРОВАЛ: окно меньше 200 px'; $code = 3 }
        else {
            $shot = Save-WindowShot $hwnd $rect $ShotPath
            $contentHost = Find-ContentHost $hwnd $rect
            if (-not $contentHost.Found) { Write-Host '[t2] ПРОВАЛ: хост содержимого не найден'; $code = 9 }
            else {
                Write-Host ("[t2] хост содержимого: {0}x{1} в ({2},{3}) снимка" -f $contentHost.W, $contentHost.H, $contentHost.X, $contentHost.Y)
                $client = New-Object MrWin+RECT
                [void][MrWin]::GetClientRect($contentHost.Hwnd, [ref]$client)
                $origin = New-Object MrWin+POINT
                $origin.X = 0; $origin.Y = 0
                [void][MrWin]::ClientToScreen($contentHost.Hwnd, [ref]$origin)
                $pageX = $origin.X - $rect.L
                $pageY = $origin.Y - $rect.T
                $pageBackground = Measure-DominantColor $shot.Bytes $shot.Stride $pageX $pageY `
                    ($pageX + ($client.R - $client.L) - 1) ($pageY + ($client.B - $client.T) - 1) 4
                Write-Host ("[t2] фон страницы: {0}" -f $pageBackground)
                $lists = 0
                $hidden = 0
                $broken = 0
                foreach ($root in Get-DirectChild $contentHost.Hwnd) {
                    if (-not [MrWin]::IsWindowVisible($root)) { continue }
                    $queue = New-Object System.Collections.ArrayList
                    [void]$queue.Add($root)
                    $cb = [MrWin+EnumProc] { param($h, $l) [void]$queue.Add($h); return $true }
                    [void][MrWin]::EnumChildWindows($root, $cb, [IntPtr]::Zero)
                    foreach ($item in $queue) {
                        if (-not [MrWin]::IsWindowVisible($item)) { continue }
                        $cls = New-Object Text.StringBuilder 256
                        [void][MrWin]::GetClassNameW($item, $cls, 256)
                        $className = $cls.ToString()
                        if ($className -ne $script:classListView -and $className -ne $script:classTreeView) { continue }
                        $itemRect = New-Object MrWin+RECT
                        [void][MrWin]::GetWindowRect($item, [ref]$itemRect)
                        $lists++
                        $itemLog = Measure-ListItem $shot $rect $item $className $pageBackground $LayoutTolerance
                        Write-Host ("[t2] {0} {1}x{2} в ({3},{4})" -f $className, `
                            ($itemRect.R - $itemRect.L), ($itemRect.B - $itemRect.T), `
                            ($itemRect.L - $rect.L), ($itemRect.T - $rect.T))
                        Write-Host $itemLog.Log
                        if ($itemLog.ColumnWhy -ne '') { $broken++; Write-Host ("[t2]   СТОЛБЦЫ: " + $itemLog.ColumnWhy) }
                        if ($itemLog.RowsWhy -ne '') {
                            $hidden++
                            Write-Host ("[t2]   НЕВИДИМЫЕ СТРОКИ: " + $itemLog.RowsWhy)
                        }
                    }
                }
                Write-Host ("[t2] ИТОГ: списков/деревьев {0}, невидимых строк {1}, сломанных столбцов {2}" -f `
                    $lists, $hidden, $broken)
                if ($lists -eq 0) { $code = 9 }
                elseif ($broken -gt 0) { $code = 10 }
                elseif ($hidden -gt 0) { $code = 14 }
            }
        }
    }
} finally {
    if ($null -ne $proc -and -not $proc.HasExited) {
        [void]$proc.CloseMainWindow()
        Start-Sleep -Seconds 3
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    }
    Restore-Registry
}
Write-Host ("[t2] код пробы: {0}" -f $code)
Write-Host ('[t2] конец пробы: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
exit $code