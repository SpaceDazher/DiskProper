# MrProper: clang-tidy по портативному ядру и Win32-слою (SPEC §9.1, §12 DoD).
# Владелец: задача P1 (волна mrproper-quality-wave).
#
#   tools\run-tidy.ps1 [-Slot a1] [-Config Release] [-Paths src/core,src/platform]
#
# Зачем скрипт, а не «clang-tidy -p build»: генератор Visual Studio не пишет
# compile_commands.json — это свойство CMake, а не проекта, и оно уже
# зафиксировано в верхнем CMakeLists.txt. Поэтому база компиляции собирается
# отдельной конфигурацией — Ninja + clang-cl. Флаги, include-пути и определения
# берутся из CMakeLists.txt, а не из копии правил в скрипте: иначе анализ
# проверял бы не тот код, который собирается.
#
# Портативность. LLVM ставится архивом
# clang+llvm-<ver>-x86_64-pc-windows-msvc.tar.xz в build\tools\llvm — это
# распаковка, а не установщик, поэтому права администратора не нужны и запроса
# на повышение не возникает. Ровно то же делает job tidy в ci.yml, только там
# образ GitHub уже несёт LLVM. Здесь скрипт ищет clang-tidy в build\tools\llvm\bin,
# затем в PATH, затем в C:\Program Files\LLVM\bin — и если не нашёл, печатает
# точную причину и код 2, а не «анализ прошёл».
#
# Правила анализа живут в .clang-tidy в корне репозитория; скрипт их не
# переопределяет. WarningsAsErrors намеренно пуст: локальный прогон должен
# показать находки, а не упасть на первой. Жёсткий режим включается ключом
# -WarningsAsErrors (в CI это отдельное решение).
param(
    [string]$Slot = 'a1',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [string[]]$Paths = @('src/core', 'src/platform'),
    [string]$TidyExe = '',
    [string]$ClangCl = '',
    [string]$Ninja = '',
    [string]$CMake = '',
    [switch]$SkipConfigure,
    [string]$WarningsAsErrors = '',
    # Пересчитать отчёт по уже сохранённому протоколу, не запуская clang-tidy
    # заново. Нужно, когда меняется только классификация находок, а прогон на
    # 34 единицах компиляции занимает десяток минут.
    [switch]$ReportOnly,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
# Русский текст отчёта должен доходить до терминала читаемым: без этого Windows
# отдаёт консоли OEM-кодировку, и весь отчёт превращается в мусор. Та же пара
# строк стоит в tools\sign-rules.ps1 — это принятый в проекте порядок.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repo 'build'
$tidyDir = Join-Path $buildRoot ('tidy-' + $Slot)
$logFile = Join-Path $tidyDir 'clang-tidy.log'
$jsonFile = Join-Path $tidyDir 'tidy-summary.json'
$dbFile = Join-Path $tidyDir 'compile_commands.json'

function Write-Info([string]$Text) {
    if (-not $Quiet) { Write-Host $Text }
}

# Запуск внешней программы с перенаправлением потоков в файлы.
#Start-Process, а не «& exe *> file»: под $ErrorActionPreference='Stop' stderr
#нативной команды превращается в terminating ErrorRecord, и любой диагностический
#вывод clang-tidy обрывал бы прогон на первой строке предупреждения компилятора.
function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [string]$StdOut,
        [string]$StdErr
    )
    $quoted = @()
    foreach ($a in $Arguments) {
        if ($a -match '[\s]') { $quoted += ('"' + $a + '"') } else { $quoted += $a }
    }
    $p = Start-Process -FilePath $Exe -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $StdOut -RedirectStandardError $StdErr
    return $p.ExitCode
}

# ---------------------------------------------------------------------------
# 1. Поиск инструментов. Порядок: явный аргумент -> переносный LLVM в
#    build\tools\llvm -> PATH -> каталог сборки. Ничего не «доказывается
#    вслепую»: каждый найденный инструмент печатается вместе с версией.
# ---------------------------------------------------------------------------
function Resolve-Exe {
    param([string]$Name, [string[]]$HintDirs, [string]$Explicit)
    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) { throw ("Явный путь не существует: " + $Explicit) }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    foreach ($dir in $HintDirs) {
        if (-not $dir) { continue }
        $guess = Join-Path $dir ($Name + '.exe')
        if (Test-Path -LiteralPath $guess) { return $guess }
    }
    return ''
}

$llvmHints = @(
    (Join-Path $buildRoot 'tools\llvm\bin'),
    (Join-Path $buildRoot 'llvm\bin'),
    (Join-Path $repo 'tools\llvm\bin'),
    'C:\Program Files\LLVM\bin'
)

# cmake и ninja в этом образе не в PATH, а лежат рядом с Visual Studio.
$vsRoots = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community',
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community',
    'D:\BuildTools'
)
$cmakeHints = @()
$ninjaHints = @()
foreach ($vs in $vsRoots) {
    $cmakeHints += (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin')
    $ninjaHints += (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
}
$cmakeHints += $llvmHints
$ninjaHints += $llvmHints

$tidyPath = Resolve-Exe -Name 'clang-tidy' -HintDirs $llvmHints -Explicit $TidyExe
if (-not $tidyPath) {
    Write-Host '[tidy] КРИТИЧНО: clang-tidy не найден.' -ForegroundColor Red
    Write-Host '[tidy] Искал в build\tools\llvm\bin, build\llvm\bin, tools\llvm\bin, C:\Program Files\LLVM\bin и в PATH.'
    Write-Host '[tidy] Портативная установка БЕЗ прав администратора:'
    Write-Host '[tidy]   1) скачать clang+llvm-<ver>-x86_64-pc-windows-msvc.tar.xz (релиз llvm/llvm-project);'
    Write-Host '[tidy]   2) распаковать содержимое в build\tools\llvm — это архив, установщик .exe не нужен.'
    exit 2
}
# clang-cl берём из того же LLVM, иначе заголовки и диагностика могут разойтись.
$clangClPath = Resolve-Exe -Name 'clang-cl' -HintDirs (@((Split-Path -Parent $tidyPath)) + $llvmHints) -Explicit $ClangCl
if (-not $clangClPath) {
    Write-Host ('[tidy] КРИТИЧНО: рядом с ' + $tidyPath + ' нет clang-cl — без него CMake не соберёт базу компиляции под clang.') -ForegroundColor Red
    exit 2
}
$cmakePath = Resolve-Exe -Name 'cmake' -HintDirs $cmakeHints -Explicit $CMake
if (-not $cmakePath) {
    Write-Host '[tidy] КРИТИЧНО: cmake не найден ни в PATH, ни в каталоге Visual Studio.' -ForegroundColor Red
    exit 2
}
$ninjaPath = Resolve-Exe -Name 'ninja' -HintDirs $ninjaHints -Explicit $Ninja
if (-not $ninjaPath) {
    Write-Host '[tidy] КРИТИЧНО: ninja не найден — без него CMake не умеет писать compile_commands.json.' -ForegroundColor Red
    exit 2
}

New-Item -ItemType Directory -Force -Path $tidyDir | Out-Null
$verOut = Join-Path $tidyDir '_version.out'
$verErr = Join-Path $tidyDir '_version.err'
Write-Info ('[tidy] clang-tidy : ' + $tidyPath)
Write-Info ('[tidy] clang-cl   : ' + $clangClPath)
Write-Info ('[tidy] cmake      : ' + $cmakePath)
Write-Info ('[tidy] ninja      : ' + $ninjaPath)
foreach ($pair in @(@($tidyPath, '--version'), @($cmakePath, '--version'), @($ninjaPath, '--version'))) {
    Invoke-Native -Exe $pair[0] -Arguments @($pair[1]) -StdOut $verOut -StdErr $verErr | Out-Null
    $v = @(Get-Content -LiteralPath $verOut -Encoding UTF8 -ErrorAction SilentlyContinue)
    if ($v.Count -gt 0) { Write-Info ('[tidy] версия     : ' + $v[0]) }
}

# ---------------------------------------------------------------------------
# 2. Окружение MSVC. Без него clang-cl не видит заголовки и библиотеки MSVC,
#    а vcvars64 нужен ещё и CMake, который ищет набор инструментов. vcvars64
#    меняет только своё окружение, поэтому его вывод импортируется в текущий
#    процесс, а не теряется вместе с cmd.
# ---------------------------------------------------------------------------
$vcvars = ''
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $found = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    $ErrorActionPreference = $prev
    if ($found) {
        foreach ($inst in $found) {
            $c = Join-Path $inst 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $c) { $vcvars = $c; break }
        }
    }
}
if (-not $vcvars) {
    foreach ($vs in ($vsRoots + @('D:\BuildTools'))) {
        $c = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $c) { $vcvars = $c; break }
    }
}

if ($vcvars) {
    Write-Info ('[tidy] vcvars64    : ' + $vcvars)
    $vcOut = Join-Path $tidyDir '_vcvars.out'
    $vcErr = Join-Path $tidyDir '_vcvars.err'
    Invoke-Native -Exe $env:ComSpec -Arguments @('/d', '/s', '/c', ('"' + $vcvars + '" >nul 2>&1 && set')) `
        -StdOut $vcOut -StdErr $vcErr | Out-Null
    $imported = 0
    foreach ($line in (Get-Content -LiteralPath $vcOut -Encoding UTF8 -ErrorAction SilentlyContinue)) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            $imported++
        }
    }
    Write-Info ('[tidy] переменных окружения импортировано из vcvars64: ' + $imported)
    if ($imported -lt 10) {
        Write-Host '[tidy] ВНИМАНИЕ: vcvars64 не дал окружение MSVC — clang-cl может не найти заголовки.' -ForegroundColor Yellow
    }
} else {
    Write-Host '[tidy] ВНИМАНИЕ: vcvars64.bat не найден, окружение MSVC не импортировано.' -ForegroundColor Yellow
    Write-Host '[tidy] clang-cl попробует найти Visual Studio сам; если не выйдет — код 3 с текстом cmake.'
}

# ---------------------------------------------------------------------------
# 3. Конфигурация базы компиляции. Только configure: ни один целевой файл
#    здесь не собирается, clang-tidy работает по compile_commands.json.
# ---------------------------------------------------------------------------
if (-not $SkipConfigure) {
    Write-Info ('[tidy] конфигурация: ' + $tidyDir + ' (Ninja + clang-cl, ' + $Config + ')')
    $configureLog = Join-Path $tidyDir 'configure.log'
    $cfgOut = Join-Path $tidyDir '_configure.out'
    $cfgErr = Join-Path $tidyDir '_configure.err'
    # Пути к cmake передаются со слешем вперёд, и это не вкус: CMake 3.20
    # (единственный, что лежит рядом с Visual Studio на этой машине) пишет
    # значение CMAKE_CXX_COMPILER в CMakeCXXCompiler.cmake как строку CMake,
    # и обратный слеш в «D:\Project\...» там становится недопустимым
    # «Invalid character escape '\P'» — конфигурация падает целиком.
    $repoFwd = ($repo -replace '\\', '/')
    $tidyDirFwd = ($tidyDir -replace '\\', '/')
    # Старый кэш удаляется: сменился компилятор, и оставшийся CMakeCache.txt
    # заставил бы CMake продолжать проверять прежний.
    $cacheFile = Join-Path $tidyDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cacheFile) { Remove-Item -LiteralPath $cacheFile -Force }
    if (Test-Path -LiteralPath $dbFile) { Remove-Item -LiteralPath $dbFile -Force }

    $cmakeArgs = @(
        '-S', $repoFwd,
        '-B', $tidyDirFwd,
        '-G', 'Ninja',
        ('-DCMAKE_MAKE_PROGRAM=' + ($ninjaPath -replace '\\', '/')),
        ('-DCMAKE_BUILD_TYPE=' + $Config),
        ('-DCMAKE_CXX_COMPILER=' + ($clangClPath -replace '\\', '/')),
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        # Анализируются src/core и src/platform, а не тесты и не упаковщик:
        # база компиляции становится меньше и конфигурация быстрее. Флаги
        # основной сборки (/W4 /WX /permissive-) при этом остаются: подменять
        # их ради удобства анализатора значило бы анализировать не тот код.
        '-DMRPROPER_BUILD_TESTS=OFF',
        '-DMRPROPER_INSTALL_TESTS=OFF',
        '-DMRPROPER_ENABLE_CPACK=OFF'
    )
    $cfgCode = Invoke-Native -Exe $cmakePath -Arguments $cmakeArgs -StdOut $cfgOut -StdErr $cfgErr
    $cfgText = @(Get-Content -LiteralPath $cfgOut -Encoding UTF8 -ErrorAction SilentlyContinue)
    $cfgText += @(Get-Content -LiteralPath $cfgErr -Encoding UTF8 -ErrorAction SilentlyContinue)
    Set-Content -LiteralPath $configureLog -Value $cfgText -Encoding UTF8
    if ($cfgCode -ne 0) {
        Write-Host ('[tidy] ОШИБКА: cmake configure вернул код ' + $cfgCode + '. Хвост протокола:') -ForegroundColor Red
        Get-Content -LiteralPath $configureLog -Tail 40
        exit 3
    }
}

if (-not (Test-Path -LiteralPath $dbFile)) {
    Write-Host ('[tidy] ОШИБКА: нет ' + $dbFile + ' — базы компиляции не существует, анализировать нечего.') -ForegroundColor Red
    exit 3
}

# ---------------------------------------------------------------------------
# 4. Отбор единиц компиляции. Файлы берутся из самой базы, а не склеиваются
#    из путей: только так проверится ровно то, что CMake собрал в базу.
# ---------------------------------------------------------------------------
# Разбор базы компиляции на Windows PowerShell 5.1 требует именно такой формы,
# и обе её части проверены на этом хосте:
#   * ConvertFrom-Json отдаёт JSON-массив верхнего уровня как ОДИН объект
#     (флаг -NoEnumerate), поэтому результат присваивается напрямую, а не
#     оборачивается в @( ... ) и не идёт по конвейеру: иначе «65 единиц
#     компиляции» становятся одной записью, у которой .file — массив из всех
#     путей, и фильтр по каталогам пропускает подряд всё, включая ui и cli;
#   * у каждой записи берётся только .file, а количество считается списком,
#     потому что .Count у одиночного PSCustomObject в 5.1 не существует.
$parsed = ConvertFrom-Json -InputObject (Get-Content -LiteralPath $dbFile -Raw)
$allFiles = New-Object System.Collections.Generic.List[string]
if ($parsed -is [System.Array]) {
    foreach ($e in $parsed) { $allFiles.Add([string]$e.file) }
} else {
    $allFiles.Add([string]$parsed.file)
}
$allUnits = $allFiles.Count

$normPaths = @($Paths | ForEach-Object { ($_ -replace '/', '\').TrimEnd('\') })
$units = @()
foreach ($one in $allFiles) {
    $f = ($one -replace '/', '\')
    foreach ($p in $normPaths) {
        if ($f -like ('*' + $p + '\*')) { $units += $one; break }
    }
}
$units = @($units | Sort-Object -Unique)

Write-Info ''
Write-Info ('[tidy] база компиляции: ' + $allUnits + ' единиц всего, ' + $units.Count + ' в проверенных путях')
if ($units.Count -eq 0) {
    Write-Host '[tidy] ОШИБКА: ни одна единица компиляции не попала под заданные пути — это пустой прогон, а не чистый результат.' -ForegroundColor Red
    Write-Host ('[tidy] Ключи -Paths: ' + ($normPaths -join ', '))
    exit 4
}
foreach ($u in $units) { Write-Info ('[tidy]   TU ' + $u) }

# ---------------------------------------------------------------------------
# 5. Прогон. Флаги базы не подменяются: анализ должен видеть ровно тот
#    набор /W4 /WX /permissive-, что и сборка.
# ---------------------------------------------------------------------------
$tidyArgs = @('-p', $tidyDir)
if ($WarningsAsErrors) { $tidyArgs += ('--warnings-as-errors=' + $WarningsAsErrors) }
$tidyArgs += $units
$outFile = Join-Path $tidyDir 'clang-tidy.stdout.log'
$errFile = Join-Path $tidyDir 'clang-tidy.stderr.log'

if ($ReportOnly) {
    if (-not (Test-Path -LiteralPath $logFile)) {
        Write-Host ('[tidy] ОШИБКА: нет ' + $logFile + ' — пересчитывать нечего.') -ForegroundColor Red
        exit 3
    }
    Write-Info ('[tidy] режим -ReportOnly: пересчёт по ' + $logFile)
    $seconds = 0
    if (Test-Path -LiteralPath $jsonFile) {
        $prev = ConvertFrom-Json -InputObject (Get-Content -LiteralPath $jsonFile -Raw -Encoding UTF8)
        $seconds = $prev.seconds
        $tidyCode = $prev.exitCode
    } else {
        $tidyCode = -1
    }
} else {

$sw = [Diagnostics.Stopwatch]::StartNew()
$tidyCode = Invoke-Native -Exe $tidyPath -Arguments $tidyArgs -StdOut $outFile -StdErr $errFile
$sw.Stop()
$seconds = [int][Math]::Round($sw.Elapsed.TotalSeconds)
# Потоки читаются как UTF-8 явно: clang-tidy пишет UTF-8 без BOM, а
# Windows PowerShell 5.1 без -Encoding считает файл системной ANSI-кодировкой и
# в тексте с русскими сообщениями правил превращает буквы в мусор — отчёт после
# этого нельзя ни прочитать, ни отдать в CI. Сводный протокол тоже пишется
# через Set-Content -Encoding UTF8, потому что оператор «>» в 5.1 по умолчанию
# даёт UTF-16LE, а такой файл ни grep, ни любой CI-шаг не прочтут.
$merged = @()
$merged += @(Get-Content -LiteralPath $outFile -Encoding UTF8 -ErrorAction SilentlyContinue)
$merged += @(Get-Content -LiteralPath $errFile -Encoding UTF8 -ErrorAction SilentlyContinue)
Set-Content -LiteralPath $logFile -Value $merged -Encoding UTF8
}
Write-Info ''
Write-Info ('[tidy] clang-tidy завершился за ' + $seconds + ' с, код ' + $tidyCode + ', протокол: ' + $logFile)

# ---------------------------------------------------------------------------
# 6. Отчёт. Считаем по check, а не по строкам протокола: одно замечание
#    печатается в несколько строк, а «сколько находок» должно означать
#    число конкретных мест в коде.
# ---------------------------------------------------------------------------
$lines = @(Get-Content -LiteralPath $logFile -Encoding UTF8 -ErrorAction SilentlyContinue)
$pattern = '^(?<file>.+?):(?<line>\d+):(?<col>\d+):\s+(?<sev>warning|error|note):\s+(?<msg>.*?)\s+\[(?<check>[A-Za-z0-9_.-]+)\]\s*$'
$byCheck = @{}
$bySev = @{ warning = 0; error = 0; note = 0 }
$findings = New-Object System.Collections.Generic.List[object]
foreach ($raw in $lines) {
    $m = [regex]::Match($raw, $pattern)
    if (-not $m.Success) { continue }
    $check = $m.Groups['check'].Value
    $sev = $m.Groups['sev'].Value
    $bySev[$sev] = $bySev[$sev] + 1
    if (-not $byCheck.ContainsKey($check)) { $byCheck[$check] = 0 }
    $byCheck[$check] = $byCheck[$check] + 1
    $findings.Add([pscustomobject]@{
            check = $check
            sev   = $sev
            file  = $m.Groups['file'].Value
            line  = [int]$m.Groups['line'].Value
            msg   = $m.Groups['msg'].Value
        })
}

# Находки делятся на три класса, и смешивать их нельзя: иначе отчёт врёт.
#   clang-diagnostic-* — сообщения самого компилятора при разборе кода;
#   clang-analyzer-*   — находки статического анализатора clang (в т.ч. optin.core),
#                        это правила анализа, а не сообщения компилятора;
#   всё остальное      — проверки clang-tidy.
# Раньше счётчик брал префикс «clang-*» и приписывал анализатор к компилятору:
# на прогоне из 34 единиц это давало «255 диагностик clang-diagnostic» вместо
# 6, из которых 249 относились к clang-analyzer-*.
$diagCount = 0
$analyzerCount = 0
foreach ($k in $byCheck.Keys) {
    if ($k -like 'clang-diagnostic-*') { $diagCount += $byCheck[$k] }
    elseif ($k -like 'clang-analyzer-*') { $analyzerCount += $byCheck[$k] }
}
$ruleFindings = $findings.Count - $diagCount - $analyzerCount

Write-Host ''
Write-Host ('=== clang-tidy: ' + $units.Count + ' единиц компиляции, ' + $seconds + ' с ===') -ForegroundColor Cyan
Write-Host ('находок всего:                              ' + $findings.Count)
Write-Host ('из них правила clang-tidy:                   ' + $ruleFindings)
Write-Host ('из них правила clang-analyzer-*:            ' + $analyzerCount)
Write-Host ('из них диагностика clang-diagnostic-*:       ' + $diagCount)
Write-Host ('severity: warning ' + $bySev['warning'] + ', error ' + $bySev['error'] + ', note ' + $bySev['note'])
if ($findings.Count -eq 0) {
    Write-Host ''
    Write-Host 'Ни одной находки. Это законный результат, но он ценен только рядом с числом единиц компиляции выше.' -ForegroundColor Green
}

if ($byCheck.Count -gt 0) {
    Write-Host ''
    Write-Host '--- находки по правилам (по убыванию) ---'
    foreach ($k in ($byCheck.Keys | Sort-Object { -$byCheck[$_] })) {
        Write-Host ('{0,5}  {1}' -f $byCheck[$k], $k)
    }
}

if (-not $Quiet) {
    Write-Host ''
    Write-Host '--- находки по файлам ---'
    $byFile = @{}
    foreach ($f in $findings) {
        if (-not $byFile.ContainsKey($f.file)) { $byFile[$f.file] = 0 }
        $byFile[$f.file] = $byFile[$f.file] + 1
    }
    foreach ($k in ($byFile.Keys | Sort-Object { -$byFile[$_] })) {
        Write-Host ('{0,5}  {1}' -f $byFile[$k], $k)
    }
}

$summary = [pscustomobject]@{
    slot            = $Slot
    config          = $Config
    paths           = $normPaths
    compileUnits    = $units.Count
    compileUnitsAll = $allUnits
    seconds         = $seconds
    exitCode        = $tidyCode
    findings        = $findings.Count
    ruleFindings    = $ruleFindings
    analyzerFindings = $analyzerCount
    warnings        = $bySev['warning']
    errors          = $bySev['error']
    clangDiagnostic = $diagCount
    byCheck         = $byCheck
    byFile          = $byFile
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $jsonFile -Encoding UTF8
Write-Info ('[tidy] сводка в JSON: ' + $jsonFile)
exit 0