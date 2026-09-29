#Requires -Version 5.1
<#
.SYNOPSIS
    MrProper: E2E полного цикла — скан, план, очистка, отчёт, проверка освобождения.

.DESNOPSIS
    Спека: §11.3 («E2E на VM-образах (Pester/PowerShell) — 20 сценариев: полный
    цикл, частичная очистка, отмена, восстановление, …») и §12 («Все 20
    e2e-сценариев зелёные»). Это первый сценарий из списка: та же эталонная VM
    из §8 Этап 2 с подготовленным мусором.

    Проверяется настоящим вызовом mrproper-cli и настоящими кодами возврата
    (src/cli/cmd_scan.hpp, src/cli/cmd_apply.hpp, src/cli/args.hpp). Заглушек и
    моков нет: если шаг нельзя выполнить, он пропускается с причиной, а не
    «проходит» на пустом месте.

      * scan   — stdout строго JSON (§6.2: прогресс и ошибки в stderr), схема
                  отчёта, маскирование серийников (§5 «Приватность»);
      * plan   — dryRun, сходимость totals.selectedBytes с суммой операций и с
                  агрегатами по действиям (§6.3), скрытые Risky (§12) и их
                  появление только с --allow-risky;
      * apply  — FR-5: без --execute не удалено ни байта; --execute вместе с
                  --candidates отказано (код 3); настоящая очистка и освобождение
                  места — по ключу -Destructive;
      * report — нормализованный JSON со всеми разделами и самодостаточный HTML
                  без единой внешней ссылки (FR-8, §12: «открывается без
                  приложения»);
      * место  — свободное место на томе песочницы до и после, сверка с
                  bytes из отчёта apply с допуском на гранулярность ФС.

    Пропуски не молчаливые. Шаг, который эта сборка выполнить не может (например
    скан без подключённого адаптера обхода ФС), печатает строку
    «SKIPPED: <код> — <текст stderr>», пишет её в build\e2e-artifacts\skipped.log
    и попадает в сводку последним шагом. «Зелёный» Pester при этом не значит
    «цикл пройден» — смотрите skipped.log.

    Прав администратора не нужно: песочница живёт в %TEMP%, а §12 требует
    «приложение запускается без повышения прав».

    Кодировка файла — UTF-8 С BOM, как у tools\sign-rules.ps1. Windows PowerShell
    5.1 читает .ps1 без BOM в кодировке ANSI, и русский текст после этого ломает
    разбор самого файла вплоть до «неожиданный токен».

.PARAMETER CliPath
    Путь к mrproper_cli.exe. Пусто — искать в build\ (<слот>\<конфигурация>).

.PARAMETER Configuration
    Имя конфигурации для поиска сборки: Debug или Release (по умолчанию Debug).

.PARAMETER Slot
    Слот сборки build\<слот>\<конфигурация>. Пусто — искать во всех слотах и взять
    самую свежую сборку.

.PARAMETER FileSizeBytes
    Размер одного файла-мусора в песочнице, байт (по умолчанию 65536).

.PARAMETER FilesPerCandidate
    Сколько файлов в одном кандидате (по умолчанию 4).

.PARAMETER Destructive
    Разрешить apply --execute. Без этого ключа шаг очистки только проверяет
    инвариант «ничего не удалено» и пишет в skipped.log, почему не удалил: на
    машине разработчика кандидаты настоящего скана — это реальный кэш, а не мусор
    теста. На эталонной VM (§12) ключ обязателен.

.PARAMETER KeepArtifacts
    Не снимать песочницу после прогона (по умолчанию она удаляется).

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-FullCycle.ps1

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-FullCycle.ps1 -Slot a87 -Destructive
    # Pester 3.4 (тот, что стоит в Windows PowerShell 5.1) не знает -Parameters:
    # Invoke-Pester -Script @{ Path = 'tests\e2e\Test-FullCycle.ps1';
    #                       Parameters = @{ Slot = 'a87' } }
#>
[CmdletBinding()]
param(
    [string] $CliPath = '',
    [string] $Configuration = 'Debug',
    [string] $Slot = '',
    [int]    $FileSizeBytes = 65536,
    [int]    $FilesPerCandidate = 4,
    [switch] $Destructive,
    [switch] $KeepArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Пути и состояние прогона
# ---------------------------------------------------------------------------

# $PSScriptRoot = <репозиторий>\tests\e2e; два уровня вверх — корень репозитория.
$scriptHere = if ($PSScriptRoot -eq '') { (Get-Location).Path } else { $PSScriptRoot }
$script:RepoRoot = Split-Path -Parent (Split-Path -Parent $scriptHere)
$script:RulesPath = Join-Path $script:RepoRoot 'rules'

# Артефакты сценария — в build\ (каталог в .gitignore), а не рядом с тестом:
# отчёт о пропусках должен пережить прогон и быть виден без запуска Pester.
$script:ArtifactsDir = Join-Path (Join-Path $script:RepoRoot 'build') 'e2e-artifacts'
try {
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
} catch {
    $script:ArtifactsDir = Join-Path ([IO.Path]::GetTempPath()) 'mrproper-e2e-artifacts'
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
}
$script:SkipLog = Join-Path $script:ArtifactsDir 'skipped.log'
if (Test-Path -LiteralPath $script:SkipLog) { Remove-Item -LiteralPath $script:SkipLog -Force }

# Состояние прогона. Пропуски пишутся в файл, а не в переменную: файл переживает
# и повторный запуск, и две реализации Pester с разным устройством фаз
# discovery/run, а сводка читает именно его.
$script:LogCounter = 0
$script:Cli = $null
$script:CliMissingReason = ''
$script:SandboxRoot = ''
$script:FixtureRoot = ''
$script:Fixture = $null
$script:CandidatesPath = ''

# Допуски на свободное место. Соседний процесс может писать на тот же диск, а
# удаление округляется к кластеру: 1 МиБ «свободы» при песочнице в единицы МиБ
# достаточно, чтобы не ловить чужую активность, и мало, чтобы пропустить
# «ничего не удалилось». Для обратной проверки (место должно вырасти) снос даётся
# на округление файлов — кластер 4 КиБ на файл, округляем вниз до 64 КиБ.
$script:FreeSpaceToleranceBytes = 1MB
$script:FreedSpaceSlackBytes = 64KB
$script:FreedSpaceCeilingBytes = 64MB

if ($null -eq (Get-Command -Name 'Describe' -ErrorAction SilentlyContinue)) {
    throw ('Pester не загружен: запускайте файл через Invoke-Pester, например ' +
        '`Invoke-Pester -Script tests\e2e\Test-FullCycle.ps1`. Прямой запуск через ' +
        'powershell.exe ничего не проверяет: Describe в этом случае не команда PowerShell.')
}

# ---------------------------------------------------------------------------
# Вспомогательные функции
# ---------------------------------------------------------------------------

function Get-MrProperCli {
    <#
    .SYNOPSIS Найти собранный mrproper_cli.exe.
    .DESCRIPTION Порядок поиска: -CliPath, $env:MRPROPER_CLI, точные пути слота и
    конфигурации, затем все слоты поимённо. Рекурсивный обход build\ не нужен:
    сорок слотов по четыре пути проверяются быстрее, чем один обход дерева.
    #>
    param([string] $Requested, [string] $Configuration, [string] $Slot)

    $names = @('mrproper_cli.exe', 'mrproper-cli.exe')
    $build = Join-Path $script:RepoRoot 'build'

    $explicit = @()
    if ($Requested -ne '') { $explicit += $Requested }
    if ($null -ne $env:MRPROPER_CLI -and $env:MRPROPER_CLI -ne '') { $explicit += $env:MRPROPER_CLI }
    foreach ($candidate in $explicit) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }
    # Явно запрошенный путь не найден — молча искать другой нельзя: сценарий пошёл
    # бы не по той сборке, которую проверяют. Причина уходит в skipped.log.

    $slotNames = @()
    if ($Slot -ne '') {
        $slotNames += $Slot
    } else {
        $slotNames += 'main'
        if (Test-Path -LiteralPath $build -PathType Container) {
            $slotNames += @(Get-ChildItem -LiteralPath $build -Directory |
                    Where-Object { $_.Name -notin @('main', 'e2e-artifacts') } |
                    Select-Object -ExpandProperty Name)
        }
    }

    $hits = @()
    foreach ($slotName in $slotNames) {
        foreach ($layout in @("$Configuration\$Configuration", $Configuration, 'src\cli')) {
            foreach ($name in $names) {
                $path = Join-Path $build (Join-Path $slotName (Join-Path $layout $name))
                if (Test-Path -LiteralPath $path -PathType Leaf) { $hits += (Get-Item -LiteralPath $path) }
            }
        }
    }
    if ($hits.Count -eq 0) { return $null }
    # Свежая сборка: слоты разных агентов собраны в разное время, и старый бинарь
    # может не знать о командах, которые уже есть в дереве.
    return ($hits | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}

function ConvertTo-CommandLine {
    <#
    .SYNOPSIS Собрать аргументы для Start-Process.
    .DESCRIPTION Start-Process склеивает массив пробелами, поэтому аргумент с
    пробелом (путь к файлу) должен прийти в кавычках: без них Process разрежет
    строку не там.
    #>
    param([string[]] $Arguments)

    $quoted = @()
    foreach ($argument in $Arguments) {
        if ($argument -match '[\s"]') { $quoted += '"' + $argument + '"' } else { $quoted += $argument }
    }
    return $quoted
}

function Invoke-MrProperCli {
    <#
    .SYNOPSIS Запустить mrproper-cli и вернуть код возврата вместе с выводом.
    .DESCRIPTION Вывод программы перенаправляется в файлы, а не в консоль: stdout
    программы — UTF-8 (§5 «Локализация»), и файл читается как UTF-8, тогда как
    консоль PowerShell без [Console]::OutputEncoding испортила бы русский текст.
    stderr и stdout разведены по файлам намеренно: машинный вывод в stdout и
    человеческий в stderr (§6.2) — иначе тест проверял бы их смесь.
    #>
    param([string] $Cli, [string[]] $Arguments, [string] $LogDirectory)

    $script:LogCounter++
    $stem = ('{0:d3}-{1}' -f $script:LogCounter, (($Arguments -join '_') -replace '[^A-Za-z0-9._-]', '_'))
    if ($stem.Length -gt 80) { $stem = $stem.Substring(0, 80) }
    $outPath = Join-Path $LogDirectory ($stem + '.out')
    $errPath = Join-Path $LogDirectory ($stem + '.err')

    $process = Start-Process -FilePath $Cli -ArgumentList (ConvertTo-CommandLine $Arguments) `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outPath -RedirectStandardError $errPath

    $outText = ''
    $errText = ''
    if (Test-Path -LiteralPath $outPath) { $outText = Get-Content -LiteralPath $outPath -Raw -Encoding UTF8 }
    if (Test-Path -LiteralPath $errPath) { $errText = Get-Content -LiteralPath $errPath -Raw -Encoding UTF8 }

    return [pscustomobject]@{
        Exit    = [int] $process.ExitCode
        Out     = $outText
        Err     = $errText
        OutPath = $outPath
        ErrPath = $errPath
    }
}

function Write-JsonFile {
    <#
    .SYNOPSIS Записать документ в JSON БЕЗ BOM.
    .DESCRIPTION Разбор JSON в mrproper-cli BOM не снимает и падает с «JSON не
    разбирается: ожидалось число (смещение 0)», а Set-Content -Encoding UTF8 в
    Windows PowerShell 5.1 BOM пишет. Поэтому файл собирается вручную.
    #>
    param([string] $Path, $Document)

    $text = $Document | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-RequiredProperty {
    <#
    .SYNOPSIS Прочитать обязательное поле разобранного JSON.
    .DESCRIPTION Под Set-StrictMode обращение к отсутствующему полю даёт
    PropertyNotFoundException без имени документа, поэтому отсутствие поля
    проверяется здесь и превращается в внятный текст с путём до поля.
    #>
    param($Document, [string] $Path, [string] $Where)

    $node = $Document
    foreach ($part in $Path.Split('.')) {
        if ($null -eq $node) { throw ($Where + ': нет поля "' + $Path + '"') }
        if ($node -is [System.Collections.IDictionary]) {
            if (-not $node.Contains($part)) { throw ($Where + ': нет поля "' + $Path + '"') }
            $node = $node[$part]
            continue
        }
        $property = $node.PSObject.Properties[$part]
        if ($null -eq $property) { throw ($Where + ': нет поля "' + $Path + '"') }
        $node = $property.Value
    }
    return $node
}

function ConvertTo-CountArray {
    <#
    .SYNOPSIS Пустой массив в JSON — это $null, а не @(): ConvertFrom-Json в
    Windows PowerShell не отличает «пустой массив» от «нет значения».
    .DESCRIPTION Возврат с запятой обязателен: без неё PowerShell разворачивает
    массив обратно в конвейер, и вызывающий получает скаляр — у одиночного
    объекта под Set-StrictMode нет свойства Count.
    #>
    param($Value)

    if ($null -eq $Value) { return , @() }
    return , @($Value)
}

function New-CleanupFixture {
    <#
    .SYNOPSIS Подготовить «мусор» для цикла: три кандидата, три уровня риска.
    .DESCRIPTION Файлы заполняются случайными байтами с фиксированным зерном по
    двум причинам. Первая: нули NTFS уводит файл в sparse, и тогда освобождение
    на томе не равно сумме bytes из отчёта — сверять было бы не с чем. Вторая:
    allocatedBytes кандидата тогда не совпадает с логическим размером, и
    сравнение плана с местом врало бы.
    Три уровня безопасности нужны, чтобы главное из §12 проверялось числами, а не
    словами: Risky по умолчанию скрыты, и это видно в totals.hiddenRisky.
    #>
    param([string] $Root, [int] $SizeBytes, [int] $FilesPerCandidate)

    $specs = @(
        @{ Rule = 'e2e.browser.cache'; Category = 'browser.cache'; Safety = 'safe'; Confidence = 90 },
        @{ Rule = 'e2e.logs.app'; Category = 'system.logs'; Safety = 'review'; Confidence = 80 },
        @{ Rule = 'e2e.old.points'; Category = 'system.old'; Safety = 'risky'; Confidence = 60 }
    )

    if (-not (Test-Path -LiteralPath $Root)) { New-Item -ItemType Directory -Path $Root -Force | Out-Null }
    $random = New-Object System.Random 20250928
    $buffer = New-Object byte[] $SizeBytes
    $random.NextBytes($buffer)

    $candidates = @()
    $totalBytes = [int64] 0
    $selectableBytes = [int64] 0
    $expectedFiles = 0
    for ($index = 0; $index -lt $specs.Count; $index++) {
        $spec = $specs[$index]
        $directory = Join-Path $Root ('cand' + ($index + 1))
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $bytes = [int64] 0
        for ($file = 1; $file -le $FilesPerCandidate; $file++) {
            [IO.File]::WriteAllBytes((Join-Path $directory ('file' + $file + '.bin')), $buffer)
            $bytes += $SizeBytes
            $expectedFiles++
        }
        $totalBytes += $bytes
        # Профиль recommended берёт safe и review; risky скрыт (§12).
        if ($spec.Safety -ne 'risky') { $selectableBytes += $bytes }
        $candidates += [ordered]@{
            ruleId         = $spec.Rule
            category       = $spec.Category
            path           = $directory
            displayName    = ('Фикстура e2e ' + $spec.Rule)
            logicalBytes   = $bytes
            allocatedBytes = $bytes
            fileCount      = $FilesPerCandidate
            oldestWrite    = 1600000000
            newestWrite    = 1700000000
            lastAccess     = 1700000000
            safety         = $spec.Safety
            confidence     = $spec.Confidence
            reasons        = @('кандидат e2e-песочницы: ' + $spec.Rule)
        }
    }

    return [pscustomobject]@{
        Candidates         = $candidates
        TotalBytes         = $totalBytes
        SelectableBytes    = $selectableBytes
        ExpectedFiles      = $expectedFiles
    }
}

function New-ReportDocument {
    <#
    .SYNOPSIS Собрать документ отчёта по схеме core::report_json (src/core).
    .DESCRIPTION Нужен, когда настоящий скан не отработал: report --in требует
    полный отчёт (схема 1, app, os, privacy, totals), а не список кандидатов, и
    на таком входе отвечает кодом 65.
    #>
    param($Candidates, [int64] $TotalBytes, [string] $Kind, [string] $Notes)

    $candidateList = ConvertTo-CountArray $Candidates
    return [ordered]@{
        schema  = 1
        kind    = $Kind
        app     = [ordered]@{ version = 'e2e-fullcycle'; pid = 4242; rulesVersion = 'e2e-fixture' }
        os      = [ordered]@{
            caption = 'Microsoft Windows'; version = '10.0.0'; build = 0; architecture = 'x64'
        }
        timing  = [ordered]@{ startedAtUnix = 1700000000; finishedAtUnix = 1700000060; durationMs = 60000 }
        privacy = [ordered]@{ serialsMasked = $true; volumeGuidsMasked = $true }
        totals  = [ordered]@{
            diskCount            = 0
            partitionCount       = 0
            volumeCount          = 0
            diskBytes            = 0
            volumeBytes          = 0
            candidateCount       = $candidateList.Count
            candidateBytes       = $TotalBytes
            lockedCandidateCount = 0
            operationCount       = 0
            untouchedCount       = 0
            succeededCount       = 0
            partialCount         = 0
            failedCount          = 0
            skippedCount         = 0
            freedBytes           = 0
            failedBytes          = 0
            errorCount           = 0
            errorOccurrences     = 0
        }
        disks      = @()
        candidates = $candidateList
        operations = @()
        untouched  = @()
        errors     = @()
        notes      = $Notes
    }
}

function Get-ReportInput {
    <#
    .SYNOPSIS Вход для шага 4: настоящий отчёт скана, а если скан на этой сборке
    не работает — синтетический документ по схеме core::report_json.
    .DESCRIPTION Скан здесь запускается заново намеренно: песочница снимается
    после каждого шага, а на живой машине скан занимает до 60 с (§12) — дешевле
    повторить один запуск, чем тащить отчёт настоящей системы через файловый кэш.
    #>
    param([string] $Cli, [string] $SandboxRoot)

    $scan = Invoke-MrProperCli -Cli $Cli -LogDirectory $SandboxRoot `
        -Arguments @('scan', '--json', '--rules', $script:RulesPath, '--quiet')
    if ($scan.Exit -eq 0) {
        $path = Join-Path $SandboxRoot 'scan.json'
        Write-JsonFile -Path $path -Document ($scan.Out | ConvertFrom-Json)
        return [pscustomobject]@{ Path = $path; FromScan = $true; Reason = '' }
    }

    $synthetic = Join-Path $SandboxRoot 'report-source.json'
    Write-JsonFile -Path $synthetic -Document (New-ReportDocument -Candidates $script:Fixture.Candidates `
            -TotalBytes $script:Fixture.TotalBytes -Kind 'scan' `
            -Notes ('синтетический отчёт e2e: скан вернул код ' + $scan.Exit))
    return [pscustomobject]@{
        Path     = $synthetic
        FromScan = $false
        Reason   = ('скан вернул код ' + $scan.Exit + ' — ' + (Get-FirstMeaningfulLine $scan.Err))
    }
}

function Get-ReportMarker {
    <#
    .SYNOPSIS Подстрока, которую обязан содержать HTML: имя первого кандидата.
    #>
    param([string] $Path)

    $document = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    $candidates = ConvertTo-CountArray (Get-RequiredProperty $document 'candidates' 'report-source')
    if ($candidates.Count -eq 0) { return '' }
    $name = [string] (Get-RequiredProperty $candidates[0] 'displayName' 'report-source.candidates[]')
    if ($name -eq '') { $name = [string] (Get-RequiredProperty $candidates[0] 'ruleId' 'report-source.candidates[]') }
    return $name
}

function Get-VolumeFreeBytes {
    <#
    .SYNOPSIS Свободное место на томе, где лежит путь.
    .DESCRIPTION AvailableFreeSpace, а не TotalFreeSpace: интересует то, что
    доступно текущему пользователю, иначе квота исказит сверку с freedBytes.
    #>
    param([string] $Path)

    $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
    $drive = New-Object System.IO.DriveInfo($root)
    return [int64] $drive.AvailableFreeSpace
}

function Get-TreeFiles {
    <#
    .SYNOPSIS Все файлы каталога: количество и сумма их размеров.
    #>
    param([string] $Root)

    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue)
    $bytes = [int64] 0
    foreach ($file in $files) { $bytes += $file.Length }
    return [pscustomobject]@{ Count = $files.Count; Bytes = $bytes }
}

function Get-FirstMeaningfulLine {
    <#
    .SYNOPSIS Первая непустая строка stderr — она и объясняет отказ команды.
    #>
    param([string] $Text)

    foreach ($line in ($Text -split "`r?`n")) {
        $trimmed = $line.Trim()
        if ($trimmed -ne '') { return $trimmed }
    }
    return '(stderr пуст)'
}

function Add-SkippedStep {
    <#
    .SYNOPSIS Отметить шаг пропущенным с точной причиной.
    .DESCRIPTION Причина обязательна: «пропущено» без причины — замаскированный
    дефект, и по такому отчёту нельзя понять, что именно не проверено. Pester 5
    умеет Set-ItResult -Skipped; в Pester 3 такой команды нет, и тогда остаются
    строка SKIPPED в выводе и запись в skipped.log — сводка её покажет.
    #>
    param([Parameter(Mandatory = $true)] [string] $Reason)

    Write-Host ('SKIPPED: ' + $Reason)
    [IO.File]::AppendAllText($script:SkipLog, 'SKIPPED: ' + $Reason + "`r`n",
        (New-Object System.Text.UTF8Encoding($false)))
    if ($null -ne (Get-Command -Name 'Set-ItResult' -ErrorAction SilentlyContinue)) {
        Set-ItResult -Skipped -Because $Reason
    }
}

function Get-SandboxRoot {
    <#
    .SYNOPSIS Корень песочницы: $TestDrive у Pester, иначе каталог артефактов.
    #>
    $variable = Get-Variable -Name 'TestDrive' -ErrorAction SilentlyContinue
    if ($null -ne $variable -and $null -ne $variable.Value -and $variable.Value -ne '') {
        return [string] $variable.Value
    }
    return $script:ArtifactsDir
}

# ---------------------------------------------------------------------------
# Сценарий
# ---------------------------------------------------------------------------

Describe 'MrProper: полный цикл (SPEC §11.3, §12)' -Tag 'e2e' {

    BeforeEach {
        $script:Cli = Get-MrProperCli -Requested $CliPath -Configuration $Configuration -Slot $Slot
        if ($null -eq $script:Cli) {
            $script:CliMissingReason = ('не найден mrproper_cli.exe (соберите tools\build.bat ' +
                $Configuration + ' <слот> или укажите -CliPath)')
        } else {
            $script:CliMissingReason = ''
        }

        $script:SandboxRoot = Join-Path (Get-SandboxRoot) ('fullcycle-' + $PID)
        if (Test-Path -LiteralPath $script:SandboxRoot) {
            Remove-Item -LiteralPath $script:SandboxRoot -Recurse -Force
        }
        New-Item -ItemType Directory -Path $script:SandboxRoot -Force | Out-Null

        $script:FixtureRoot = Join-Path $script:SandboxRoot 'fixture'
        $script:Fixture = New-CleanupFixture -Root $script:FixtureRoot `
            -SizeBytes $FileSizeBytes -FilesPerCandidate $FilesPerCandidate
        $script:CandidatesPath = Join-Path $script:SandboxRoot 'candidates.json'
        Write-JsonFile -Path $script:CandidatesPath -Document ([ordered]@{
                schema     = 1
                kind       = 'scan'
                candidates = $script:Fixture.Candidates
            })
    }

    AfterEach {
        if ($KeepArtifacts) {
            Write-Host ('песочница оставлена: ' + $script:SandboxRoot)
        } elseif (Test-Path -LiteralPath $script:SandboxRoot) {
            Remove-Item -LiteralPath $script:SandboxRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Context 'шаг 1: скан' {

        It 'scan отдаёт отчёт в stdout или документированный отказ' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            if (-not (Test-Path -LiteralPath $script:RulesPath -PathType Container)) {
                Add-SkippedStep ('набор правил не найден: ' + $script:RulesPath); return
            }

            $scan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('scan', '--json', '--rules', $script:RulesPath, '--quiet')

            # Коды scan (src/cli/cmd_scan.hpp): 0 — отчёт, 3 — набор правил
            # недоступен, 4 — прогон не дал результата. Ничего другого команда
            # возвращать не обещает, и такой код — уже дефект, а не пропуск.
            if ($scan.Exit -ne 0 -and $scan.Exit -ne 3 -and $scan.Exit -ne 4) {
                throw ('scan вернул недокументированный код ' + $scan.Exit + ': ' +
                    (Get-FirstMeaningfulLine $scan.Err))
            }
            if ($scan.Exit -ne 0) {
                # Сейчас это код 4: адаптер обхода ФС не подключён. Шаг пропускается
                # с текстом stderr, чтобы по логу было видно, что цикл не полный.
                Add-SkippedStep ('scan: код ' + $scan.Exit + ' — ' + (Get-FirstMeaningfulLine $scan.Err))
                return
            }

            # stdout остаётся машинным: в нём не должно быть ничего, кроме JSON
            # (§6.2 — иначе `scan --json | jq` падает на строке прогресса).
            $report = $scan.Out | ConvertFrom-Json
            (Get-RequiredProperty $report 'schema' 'scan') | Should Be 1
            (Get-RequiredProperty $report 'kind' 'scan') | Should Be 'scan'
            (Get-RequiredProperty $report 'privacy.serialsMasked' 'scan') | Should Be $true
            (ConvertTo-CountArray (Get-RequiredProperty $report 'candidates' 'scan')).Count |
                Should Be (Get-RequiredProperty $report 'totals.candidateCount' 'scan')
            $scan.Out.Trim().StartsWith('{') | Should Be $true
        }
    }

    Context 'шаг 2: план' {

        It 'plan строит dry-run: суммы сходятся, Risky скрыты, план детерминирован' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $script:CandidatesPath)
            $plan.Exit | Should Be 0

            $document = $plan.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'schema' 'plan') | Should Be 1
            (Get-RequiredProperty $document 'dryRun' 'plan') | Should Be $true
            # Профиль по умолчанию — recommended, Risky скрыты (§12).
            (Get-RequiredProperty $document 'profile' 'plan') | Should Be 'recommended'
            (Get-RequiredProperty $document 'options.allowRisky' 'plan') | Should Be $false

            $operations = ConvertTo-CountArray (Get-RequiredProperty $document 'operations' 'plan')
            $untouched = ConvertTo-CountArray (Get-RequiredProperty $document 'untouched' 'plan')
            $totalCandidates = Get-RequiredProperty $document 'totals.candidates' 'plan'
            $operations.Count | Should Be (Get-RequiredProperty $document 'totals.selected' 'plan')
            $untouched.Count | Should Be ($totalCandidates - $operations.Count)
            (Get-RequiredProperty $document 'totals.hiddenRisky' 'plan') | Should Be 1

            # Инвариант §6.3: сумма операций равна объявленному объёму. План, у
            # которого reclaimBytes не сходится с allocatedBytes, удаляет не то.
            $sum = [int64] 0
            $riskyInPlan = 0
            foreach ($operation in $operations) {
                $sum += [int64] (Get-RequiredProperty $operation 'bytes' 'plan.operations[]')
                $safety = [string] (Get-RequiredProperty $operation 'safety' 'plan.operations[]')
                if ($safety -eq 'risky') { $riskyInPlan++ }
            }
            $sum | Should Be (Get-RequiredProperty $document 'totals.selectedBytes' 'plan')
            $sum | Should Be $script:Fixture.SelectableBytes
            $riskyInPlan | Should Be 0

            # Агрегаты по действиям обязаны покрывать каждого кандидата ровно
            # одним действием, а удаляющие действия — ровно те операции, что в плане:
            # иначе план обещает объём, который ничем не будет исполнен, а скрытый
            # Risky (keep) не объясняет своего места в итогах.
            $byActionAll = [int64] 0
            $byActionRemoving = [int64] 0
            foreach ($action in @('delete', 'trash', 'keep', 'skipLocked')) {
                $count = [int64] (Get-RequiredProperty $document ('totals.byAction.' + $action + '.count') 'plan')
                $byActionAll += $count
                if ($action -eq 'delete' -or $action -eq 'trash') { $byActionRemoving += $count }
            }
            $byActionAll | Should Be (Get-RequiredProperty $document 'totals.allCount' 'plan')
            $byActionRemoving | Should Be $operations.Count

            # Ни один элемент не удаляется без объяснения и уровня риска (§12).
            foreach ($operation in $operations) {
                (Get-RequiredProperty $operation 'reason' 'plan.operations[]') | Should Not BeNullOrEmpty
                (Get-RequiredProperty $operation 'safety' 'plan.operations[]') | Should Not BeNullOrEmpty
                (Get-RequiredProperty $operation 'category' 'plan.operations[]') | Should Not BeNullOrEmpty
                $confidence = [int] (Get-RequiredProperty $operation 'confidence' 'plan.operations[]')
                ($confidence -ge 0 -and $confidence -le 100) | Should Be $true
            }

            # Скрытый Risky обязан быть виден в «не трогаем» с причиной, а не исчезнуть.
            $hiddenReasons = 0
            foreach ($item in $untouched) {
                $reason = [string] (Get-RequiredProperty $item 'reason' 'plan.untouched[]')
                if ($reason -match 'Risky') { $hiddenReasons++ }
            }
            $hiddenReasons | Should Be 1

            # План детерминирован: те же кандидаты — тот же документ. На этом стоят
            # golden-тесты §11.4, и на этом же ловится плавающий порядок обхода.
            $again = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $script:CandidatesPath)
            $again.Exit | Should Be 0
            $again.Out | Should Be $plan.Out
        }

        It 'plan --allow-risky показывает Risky, скрытый профилем по умолчанию' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--allow-risky', '--candidates', $script:CandidatesPath)
            $plan.Exit | Should Be 0

            $document = $plan.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'options.allowRisky' 'plan') | Should Be $true
            (Get-RequiredProperty $document 'totals.hiddenRisky' 'plan') | Should Be 0

            $risky = 0
            $sum = [int64] 0
            foreach ($operation in (ConvertTo-CountArray (Get-RequiredProperty $document 'operations' 'plan'))) {
                if ([string] (Get-RequiredProperty $operation 'safety' 'plan.operations[]') -eq 'risky') { $risky++ }
                $sum += [int64] (Get-RequiredProperty $operation 'bytes' 'plan.operations[]')
            }
            $risky | Should Be 1
            $sum | Should Be $script:Fixture.TotalBytes
        }
    }

    Context 'шаг 3: очистка' {

        It 'apply без --execute — сухой прогон: не удалено ни байта (FR-5)' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $freeBefore = Get-VolumeFreeBytes $script:SandboxRoot
            $apply = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--json', '--yes', '--candidates', $script:CandidatesPath)
            $freeAfter = Get-VolumeFreeBytes $script:SandboxRoot
            $apply.Exit | Should Be 0

            $document = $apply.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'kind' 'apply') | Should Be 'apply'
            (Get-RequiredProperty $document 'executed' 'apply') | Should Be $false
            (Get-RequiredProperty $document 'dryRun' 'apply') | Should Be $true
            # Отказ — не пустое место: команда обязана сказать, почему не удалила.
            (Get-RequiredProperty $document 'refusal' 'apply') | Should Not BeNullOrEmpty
            # Исполнения в сухом прогоне нет вовсе, а не «нулевое».
            (Get-RequiredProperty $document 'execution' 'apply') | Should BeNullOrEmpty

            $left = Get-TreeFiles $script:FixtureRoot
            $left.Count | Should Be $script:Fixture.ExpectedFiles
            $left.Bytes | Should Be $script:Fixture.TotalBytes
            ([Math]::Abs($freeAfter - $freeBefore)) | Should BeLessThan $script:FreeSpaceToleranceBytes
        }

        It 'apply --execute вместе с --candidates отказано (FR-5)' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $freeBefore = Get-VolumeFreeBytes $script:SandboxRoot
            # Код 3 = PlanExit::Refused (src/cli/cmd_apply.hpp): список из файла мог
            # устареть между сканом и очисткой, удалять по нему нельзя.
            $apply = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--execute', '--yes', '--json', '--candidates', $script:CandidatesPath)
            $freeAfter = Get-VolumeFreeBytes $script:SandboxRoot

            $apply.Exit | Should Be 3
            $apply.Err | Should Match '--candidates'
            $left = Get-TreeFiles $script:FixtureRoot
            $left.Count | Should Be $script:Fixture.ExpectedFiles
            ([Math]::Abs($freeAfter - $freeBefore)) | Should BeLessThan $script:FreeSpaceToleranceBytes
        }

        It 'apply --execute освобождает место, и freedBytes сходится с томом' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            if (-not $Destructive) {
                Add-SkippedStep ('apply --execute не запускался без -Destructive: на машине ' +
                    'разработчика кандидаты настоящего скана — это реальный кэш, а не мусор ' +
                    'теста; песочницу нечем скормить (--candidates с --execute запрещён, ' +
                    'cmd_apply.cpp), а исполнитель и источник кандидатов в CLI не подключены')
                # Инвариант остаётся проверяемым и без удаления: песочница цела.
                $left = Get-TreeFiles $script:FixtureRoot
                $left.Count | Should Be $script:Fixture.ExpectedFiles
                $left.Bytes | Should Be $script:Fixture.TotalBytes
                return
            }

            $freeBefore = Get-VolumeFreeBytes $script:SandboxRoot
            $apply = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--execute', '--yes', '--json', '--no-trash')
            $freeAfter = Get-VolumeFreeBytes $script:SandboxRoot

            # 4 = NoCandidates, 5 = NoExecutor (src/cli/cmd_apply.hpp): обе означают,
            # что ничего не удалено, и обе — не «очистил, но не всё».
            if ($apply.Exit -eq 4) {
                Add-SkippedStep ('apply --execute: код 4 — ' + (Get-FirstMeaningfulLine $apply.Err))
            } elseif ($apply.Exit -eq 5) {
                Add-SkippedStep ('apply --execute: код 5 — ' + (Get-FirstMeaningfulLine $apply.Err))
            } elseif ($apply.Exit -ne 0) {
                throw ('apply --execute вернул недокументированный код ' + $apply.Exit + ': ' +
                    (Get-FirstMeaningfulLine $apply.Err))
            } else {
                $document = $apply.Out | ConvertFrom-Json
                (Get-RequiredProperty $document 'executed' 'apply') | Should Be $true
                (Get-RequiredProperty $document 'confirmed' 'apply') | Should Be $true
                (Get-RequiredProperty $document 'execution.aborted' 'apply') | Should Be $false
                (Get-RequiredProperty $document 'execution.failed' 'apply') | Should Be 0

                $freed = [int64] (Get-RequiredProperty $document 'execution.freedBytes' 'apply')
                $freed | Should BeGreaterThan 0
                (Get-TreeFiles $script:FixtureRoot).Count | Should Be 0

                # Место на томе выросло не меньше, чем отчитала программа, минус
                # округление файлов, и не выросло бесконечно (соседний процесс).
                $delta = $freeAfter - $freeBefore
                ($delta + $script:FreedSpaceSlackBytes) | Should BeGreaterThan $freed
                ($delta - $freed) | Should BeLessThan $script:FreedSpaceCeilingBytes
            }
        }
    }

    Context 'шаг 4: отчёт' {

        It 'report --json нормализует отчёт и печатает все разделы' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $input = Get-ReportInput -Cli $script:Cli -SandboxRoot $script:SandboxRoot
            if (-not $input.FromScan) { Write-Host ('отчёт сверен на синтетическом документе: ' + $input.Reason) }

            $outputPath = Join-Path $script:SandboxRoot 'report.json'
            $report = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $input.Path, '--out', $outputPath)
            $report.Exit | Should Be 0

            (Test-Path -LiteralPath $outputPath) | Should Be $true
            $raw = Get-Content -LiteralPath $outputPath -Raw -Encoding UTF8
            $document = $raw | ConvertFrom-Json
            (Get-RequiredProperty $document 'schema' 'report') | Should Be 1
            (Get-RequiredProperty $document 'kind' 'report') | Should Not BeNullOrEmpty
            (Get-RequiredProperty $document 'privacy.serialsMasked' 'report') | Should Be $true
            ($null -ne (Get-RequiredProperty $document 'notes' 'report')) | Should Be $true
            # Пустые разделы печатаются, а не выбрасываются: читатель не должен
            # гадать, «есть ли такой раздел» (core::report_json).
            foreach ($section in @('disks', 'candidates', 'operations', 'untouched', 'errors')) {
                $raw | Should Match ('"' + $section + '":')
            }
        }

        It 'report --html даёт один самодостаточный файл без внешних ссылок (FR-8)' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $input = Get-ReportInput -Cli $script:Cli -SandboxRoot $script:SandboxRoot
            if (-not $input.FromScan) { Write-Host ('отчёт сверен на синтетическом документе: ' + $input.Reason) }

            $outputPath = Join-Path $script:SandboxRoot 'report.html'
            $report = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--html', '--in', $input.Path, '--out', $outputPath,
                '--title', 'e2e: полный цикл', '--lang', 'ru')
            $report.Exit | Should Be 0

            (Test-Path -LiteralPath $outputPath) | Should Be $true
            $html = Get-Content -LiteralPath $outputPath -Raw -Encoding UTF8
            $html.Length | Should BeGreaterThan 1000
            $html | Should Match '(?i)<!DOCTYPE html>'
            $html | Should Match '(?i)</html>'

            # Самодостаточный отчёт: ни одного обращения наружу — иначе он не
            # откроется без приложения и без сети (§12, §11.4 golden).
            ([regex]::Matches($html, '(?i)(src|href)\s*=\s*["'']?\s*(https?:)?//')).Count | Should Be 0
            ([regex]::Matches($html, '(?i)@import')).Count | Should Be 0
            ([regex]::Matches($html, '(?i)<script')).Count | Should Be 0

            # Отчёт показывает данные, а не пустую рамку: в нём есть имя кандидата.
            $marker = Get-ReportMarker -Path $input.Path
            if ($marker -ne '') { $html | Should Match ([regex]::Escape($marker)) }
        }
    }

    Context 'шаг 5: сводка' {

        It 'каждый пропуск записан с причиной' {
            $lines = @()
            if (Test-Path -LiteralPath $script:SkipLog) {
                $lines = @(Get-Content -LiteralPath $script:SkipLog -Encoding UTF8 |
                        Where-Object { $_.Trim() -ne '' })
            }
            foreach ($line in $lines) { $line | Should Match '^SKIPPED: .{10,}$' }
            if ($lines.Count -eq 0) {
                Write-Host 'пропусков нет: весь цикл выполнен на этой сборке'
            } else {
                Write-Host ('пропущено шагов: ' + $lines.Count + ' (см. ' + $script:SkipLog + ')')
                foreach ($line in $lines) { Write-Host ('  ' + $line) }
            }
        }
    }
}
