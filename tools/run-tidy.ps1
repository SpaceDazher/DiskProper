# MrProper: clang-tidy по портативному ядру и Win32-слою (SPEC §9.1, §12 DoD).
# Владелец: задача P1 (волна mrproper-quality-wave).
#
#   tools\run-tidy.ps1 [-Slot a1] [-Config Release] [-Paths src/core,src/platform]
#
# Зачем скрипт, а не «clang-tidy -p build»: генератор Visual Studio не пишет
# compile_commands.json (это свойство CMake, а не проекта, и оно зафиксировано
# в верхнем CMakeLists.txt), поэтому база компиляции собирается отдельной
# конфигурацией — Ninja + clang-cl. Флаги, include-пути и определения берутся
# из CMakeLists.txt, а не из копии правил в скрипте: иначе анализ проверял бы
# не тот код, который собирается.
#
# Портативность. LLVM ставится архивом
# clang+llvm-<ver>-x86_64-pc-windows-msvc.tar.xz в build\tools\llvm — это
# распаковка, а не установщик, поэтому права администратора не нужны и запроса
# на повышение не возникает. То же самое делает job tidy в .github/workflows/ci.yml,
# только там образ GitHub уже несёт LLVM. Здесь скрипт ищет clang-tidy в
# build\tools\llvm\bin, затем в PATH, затем в каталоге сборки — и если не
# нашёл, печатает точную причину и код 2, а не «анализ прошёл».
#
# Правила анализа живут в .clang-tidy в корне репозитория; скрипт их не
# переопределяет. WarningsAsErrors намеренно пуст: локальный прогон должен
# показать находки, а не упасть на первой. Жёсткий режим включается ключом
# -WarningsAsErrors (в CI он уже есть отдельным решением).
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
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repo 'build'
$tidyDir = Join-Path $buildRoot ("tidy-" + $Slot)
$logFile = Join-Path $tidyDir 'clang-tidy.log'
$jsonFile = Join-Path $tidyDir 'tidy-summary.json'
$dbFile = Join-Path $tidyDir 'compile_commands.json'

function Write-Info([string]$Text) {
    if (-not $Quiet) { Write-Host $Text }
}

# ---------------------------------------------------------------------------
# 1. Поиск инструментов. Порядок: явный аргумент -> переносный LLVM в
#    build\tools\llvm -> PATH -> каталог сборки. Ничего не «доказывается
#    вслепую»: каждый найденный инструмент печатается с версией.
# ---------------------------------------------------------------------------
function Resolve-Exe {
    param([string]$Name, [string[]]$HintDirs, [string]$Explicit)
    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) {
            throw "Явный путь не существует: $Explicit"
        }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    foreach ($dir in $HintDirs) {
        if (-not $dir) { continue }
        $guess = Join-Path $dir "$Name.exe"
        if (Test-Path -LiteralPath $guess) { return $guess }
    }
    return ''
}

# Переносный LLVM: то, что распаковал скрипт, и то, что уже могло лежать рядом.
$llvmHints = New-Object System.Collections.Generic.List[string]
foreach ($root in @((Join-Path $buildRoot 'tools\llvm\bin'), (Join-Path $buildRoot 'llvm\bin'), (Join-Path $repo 'tools\llvm\bin'), 'C:\Program Files\LLVM\bin')) {
    $llvmHints.Add($root)
}
$searchRoots = @($llvmHints.ToArray())

# cmake и ninja в этом образе не в PATH, а лежат рядом с Visual Studio.
$vsRoots = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community',
    'D:\BuildTools'
)
$cmakeHints = New-Object System.Collections.Generic.List[string]
$ninjaHints = New-Object System.Collections.Generic.List[string]
foreach ($vs in $vsRoots) {
    $cmakeHints.Add((Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'))
    $ninjaHints.Add((Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'))
}
foreach ($dir in @($searchRoots)) { $cmakeHints.Add($dir); $ninjaHints.Add($dir) }

$tidyPath = Resolve-Exe -Name 'clang-tidy' -HintDirs $searchRoots -Explicit $TidyExe
if (-not $tidyPath) {
    Write-Host '[tidy] КРИТИЧНО: clang-tidy не найден.' -ForegroundColor Red
    Write-Host '[tidy] Искал в build\tools\llvm\bin, tools\llvm\bin, C:\Program Files\LLVM\bin и в PATH.'
    Write-Host '[tidy] Портативная установка без прав администратора:'
    Write-Host '[tidy]   1) скачать clang+llvm-<ver>-x86_64-pc-windows-msvc.tar.xz с github.com/llvm/llvm-project'
    Write-Host '[tidy]   2) распаковать в build\tools\llvm (архив, установщик не нужен)'
    exit 2
}
# clang-cl берём из того же LLVM, иначе флаги и заголовки могут разойтись.
$clangClPath = Resolve-Exe -Name 'clang-cl' -HintDirs (@((Split-Path -Parent $tidyPath)) + $searchRoots) -Explicit $ClangCl
if (-not $clangClPath) {
    Write-Host "[tidy] КРИТИЧНО: рядом с $tidyPath нет clang-cl — без него CMake не соберёт базу компиляции под clang." -ForegroundColor Red
    exit 2
}
$cmakePath = Resolve-Exe -Name 'cmake' -HintDirs $cmakeHints.ToArray() -Explicit $CMake
if (-not $cmakePath) {
    Write-Host '[tidy] КРИТИЧНО: cmake не найден ни в PATH, ни в каталоге Visual Studio.' -ForegroundColor Red
    exit 2
}
$ninjaPath = Resolve-Exe -Name 'ninja' -HintDirs $ninjaHints.ToArray() -Explicit $Ninja
if (-not $ninjaPath) {
    Write-Host '[tidy] КРИТИЧНО: ninja не найден — без него CMake не умеет писать compile_commands.json.' -ForegroundColor Red
    exit 2
}

$clangExe = Join-Path (Split-Path -Parent $clangClPath) 'clang++.exe'
Write-Info ('[tidy] clang-tidy : ' + $tidyPath)
Write-Info ('[tidy] clang-cl   : ' + $clangClPath)
Write-Info ('[tidy] cmake      : ' + $cmakePath)
Write-Info ('[tidy] ninja      : ' + $ninjaPath)
foreach ($pair in @(@($clangExe, '--version'), @($cmakePath, '--version'), @($ninjaPath, '--version'))) {
    if (Test-Path -LiteralPath $pair[0]) {
        $v = (& $pair[0] $pair[1] 2>&1 | Select-Object -First 1)
        Write-Info ('[tidy] версия     : ' + $v)
    }
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
    $found = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($found) {
        foreach ($inst in $found) {
            $c = Join-Path $inst 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $c) { $vcvars = $c; break }
        }
    }
}
if (-not $vcvars -and (Test-Path -LiteralPath 'D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat')) {
    $vcvars = 'D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
}
if (-not $vcvars) { foreach ($vs in $vsRoots) { $c = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'; if (Test-Path -LiteralPath $c) { $vcvars = $c; break } } }

if ($vcvars) {
    Write-Info ('[tidy] vcvars64    : ' + $vcvars)
    $envLines = & cmd.exe /d /s /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $envLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
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
    New-Item -ItemType Directory -Force -Path $tidyDir | Out-Null
    Write-Info ('[tidy] конфигурация: ' + $tidyDir + ' (Ninja + clang-cl, ' + $Config + ')')
    $configureLog = Join-Path $tidyDir 'configure.log'
    $args = @(
        '-S', $repo,
        '-B', $tidyDir,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninjaPath",
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_CXX_COMPILER=$clangClPath",
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DMRPROPER_BUILD_TESTS=OFF',
        '-DMRPROPER_ENABLE_CPACK=OFF',
        '-DMRPROPER_INSTALL_TESTS=OFF'
    )
    if (Test-Path -LiteralPath (Join-Path $clangClPath '..\clang++.exe')) {
        $args += "-DCMAKE_CXX_COMPILER_LAUNCHER="
    }
    & $cmakePath @args *> $configureLog
    $cfgCode = $LASTEXITCODE
    if ($cfgCode -ne 0) {
        Write-Host "[tidy] ОШИБКА: cmake configure вернул код $cfgCode. Последние строки:" -ForegroundColor Red
        Get-Content -LiteralPath $configureLog -Tail 40
        exit 3
    }
}

if (-not (Test-Path -LiteralPath $dbFile)) {
    Write-Host "[tidy] ОШИБКА: нет $dbFile — базы компиляции не существует, анализировать нечего." -ForegroundColor Red
    exit 3
}

# ---------------------------------------------------------------------------
# 4. Отбор единиц компиляции. Файлы берутся из самой базы, а не склеиваются
#    из путей: только так проверится ровно то, что CMake собрал в базу.
# ---------------------------------------------------------------------------
$db = Get-Content -LiteralPath $dbFile -Raw | ConvertFrom-Json
$normPaths = $Paths | ForEach-Object { ($_ -replace '/', '\').TrimEnd('\') }
$units = @($db | Where-Object {
        $f = ($_.file -replace '/', '\')
        foreach ($p in $normPaths) { if ($f -like "*$p\*") { return $true } }
        return $false
    } | ForEach-Object { $_.file } | Sort-Object -Unique)

Write-Info ''
Write-Info ('[tidy] база компиляции: ' + (@($db).Count) + ' единиц всего, ' + $units.Count + ' в проверенных путях')
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
New-Item -ItemType Directory -Force -Path $tidyDir | Out-Null
$tidyArgs = @('-p', $tidyDir, '--quiet')
if ($WarningsAsErrors) { $tidyArgs += "--warnings-as-errors=$WarningsAsErrors" }
$tidyArgs += $units

$sw = [Diagnostics.Stopwatch]::StartNew()
& $tidyPath @tidyArgs *> $logFile
$tidyCode = $LASTEXITCODE
$sw.Stop()
$seconds = [int][Math]::Round($sw.Elapsed.TotalSeconds)
Write-Info ''
Write-Info ('[tidy] clang-tidy завершился за ' + $seconds + ' с, код ' + $tidyCode + ', протокол: ' + $logFile)

# ---------------------------------------------------------------------------
# 6. Отчёт. Считаем по check, а не по строкам лога: одно замечание может
#    печататься в несколько строк, а «сколько находок» должно означать
#    число конкретных мест в коде.
# ---------------------------------------------------------------------------
$lines = Get-Content -LiteralPath $logFile
$pattern = '^(?<file>.+?):(?<line>\d+):(?<col>\d+):\s+(?<sev>warning|error|note):\s+(?<msg>.*?)\s+\[(?<check>[A-Za-z0-9_.-]+)\]\s*$'
$byCheck = @{}
$bySev = @{ warning = 0; error = 0; note = 0 }
$findings = New-Object System.Collections.Generic.List[object]
foreach ($raw in $lines) {
    $m = [regex]::Match($raw, $pattern)
    if (-not $m.Success) { continue }
    $check = $m.Groups['check'].Value
    $sev = $m.Groups['sev'].Value
    $bySev[$sev]++
    if (-not $byCheck.ContainsKey($check)) { $byCheck[$check] = 0 }
    $byCheck[$check]++
    $findings.Add([pscustomobject]@{
            check = $check; sev = $sev
            file = $m.Groups['file'].Value
            line = [int]$m.Groups['line'].Value
            msg = $m.Groups['msg'].Value
        })
}

# Диагностика компилятора (clang-diagnostic-*) — это не находки tidy, а
# сообщения clang о разборе кода; считается отдельно, чтобы не выдать
# ошибку чужого компилятора за правило статического анализа.
$diagCount = 0
foreach ($k in $byCheck.Keys) { if ($k -like 'clang-*') { $diagCount += $byCheck[$k] } }

$total = $findings.Count
$bySev.warning + $bySev.error + $bySev.note | Out-Null
$nonDiagnostic = $total - $diagCount

Write-Host ''
Write-Host ('=== clang-tidy: ' + $units.Count + ' единиц компиляции, ' + $seconds + ' с ===') -ForegroundColor Cyan
Write-Host ('находок всего (с диагностикой компилятора): ' + $total)
Write-Host ('находок правил анализа:                    ' + $nonDiagnostic)
Write-Host ('из них warning: ' + $bySev.warning + ', error: ' + $bySev.error + ', note: ' + $bySev.note)
Write-Host ('диагностика clang-diagnostic-*:             ' + $diagCount)
if ($total -eq 0) {
    Write-Host ''
    Write-Host 'Ни одной находки. Проверьте, что база не пуста: число единиц компиляции выше.' -ForegroundColor Green
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
        $byFile[$f.file]++
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
    compileUnitsAll = @($db).Count
    seconds         = $seconds
    exitCode        = $tidyCode
    findings        = $total
    ruleFindings    = $nonDiagnostic
    warnings        = $bySev.warning
    errors          = $bySev.error
    clangDiagnostic = $diagCount
    byCheck         = [pscustomobject]$byCheck
    byFile          = [pscustomobject]$byFile
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $jsonFile -Encoding UTF8
Write-Info ('[tidy] сводка в JSON: ' + $jsonFile)
exit 0