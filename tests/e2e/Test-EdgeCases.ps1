#Requires -Version 5.1
<#
.SYNOPSIS
    MrProper: e2e-сценарии защиты — занятые файлы, ошибки ввода-вывода вместо
    битых секторов, зашифрованный том, отсутствие прав.

.DESCRIPTION
    Спека: §11, п. 3 («E2E на VM-образах (Pester/PowerShell) — 20 сценариев:
    полный цикл, частичная очистка, отмена, восстановление, приложение держит
    файл, зашифрованный том, диск с битыми секторами в эмуляции, отсутствие
    прав, первый запуск, обновление правил») и §12 («0 крашей и 0 необработанных
    исключений на сценариях 2–6; все ошибки в логе с путём и HRESULT»,
    «Приложение запускается без повышения прав; повышение запрашивается только
    на операции»). Заметка о нумерации: заголовка «11.3» в SPEC.md нет, есть
    §11 с нумерованным списком; нужный пункт — третий, он и разбирается здесь.
    Соседний сценарий того же списка (полный цикл) лежит в Test-FullCycle.ps1.

    Четыре сценария, каждый на настоящих кодах возврата и настоящих файловых
    отказах Windows. Заглушек и моков нет; там, где сборка не может дойти до
    нужного места, шаг пропускается с точным текстом отказа (см. Add-SkippedStep).

      1. Занятые файлы (приложение держит файл, FR-5 Skip (locked), FR-6).
         Три независимые проверки, потому что «занят» — это три разных факта:
           * физическая блокировка: файл держит живой процесс с FileShare.None,
             и ни plan, ни apply не могут заставить его исчезнуть;
           * блокировка в данных: поле lockedBy в дампе скана (его заполняет
             Restart Manager) обязано дать действие skip-locked, и ни
             --allow-risky, ни --profile everything этого не отменяют —
             core::plan::decideCandidate проверяет lockedBy первым;
           * неполные данные: `locked: true` без lockedBy — отвергаются, а не
             превращаются в кандидата к удалению (cmd_apply.cpp).
      2. Ошибки ввода-вывода вместо битых секторов. Настоящий битый диск в
         эмуляции не воспроизвести, но класс отказов тот же, что даёт диск:
         Win32 5 (отказано в доступе), 2 (файла нет), 206 (слишком длинный путь),
         4390 (репarse-петля), плюс оборванный и пустой вход. Проверяется не
         «скрипт отработал», а два свойства: отказ назван кодом и путём, и ни
         один отказ не превращается ни в необработанное исключение (код 70),
         ни в ложный ноль.
      3. Зашифрованный том. EFS проверяется по-настоящему (cipher /e плюс
         cipher /c: том поддерживает шифрование, файл помечен «U»). BitLocker
         читается manage-bde, которому без прав администратора нельзя, — это
         единственный шаг, который на машине разработчика уходит в пропуск с
         текстом самой утилиты; на эталонной VM (§12) он обязан отработать.
      4. Отсутствие прав. Проверяется то, что можно проверить без повышения:
         программа стартует и работает, инвентаризация помечает устройство
         недоступным вместо выдуманной карты (FR-1), отказ по правам приходит
         документированным кодом с путём, и ничто не делается «в обход» ACL.

    Инвариант «ничего не удалилось» проверяется снимком ДЕРЕВА ПЕСОЧНИЦЫ
    (New-FixtureSnapshot + Assert-FixtureUnchanged), а не разностью свободного
    места на томе. Том общий: рядом собираются другие агенты, и прогон G4
    поймал именно это — место уехало на 1.36 МиБ при допуске 1 МиБ, хотя
    утилита ничего не удаляла. Взамен проверка стала строже: снимок ловит любое
    удаление и любое создание в песочнице точно, без допуска, а свободное место
    остаётся наблюдением в preconditions.json.

    Пропуски не молчаливые: строка «SKIPPED: <причина>» печатается в консоль,
    пишется в build\e2e-artifacts\edge\skipped.log (свой каталог, чтобы
    параллельный прогон Test-FullCycle.ps1 не затирал свой журнал) и попадает в
    сводку последним шагом. «Зелёный» Pester при этом не значит «все четыре
    сценария пройдены» — смотрите skipped.log и preconditions.json.

    Прав администратора не нужно: deny-ACE ставится на каталоги, созданные
    самим скриптом из-под текущего пользователя, а перед уборкой песочницы
    ACE снимается (icacls /remove:d /T). Скрипт ничего не пишет вне
    %TEMP% и build\e2e-artifacts.

    Кодировка файла — UTF-8 С BOM, как у tools\sign-rules.ps1: Windows PowerShell
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
    Размер одного файла-фикстуры, байт (по умолчанию 65536).

.PARAMETER KeepArtifacts
    Не снимать песочницу после прогона (по умолчанию она удаляется).

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-EdgeCases.ps1

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-EdgeCases.ps1 -Slot a90
    # Pester 3.4 (тот, что стоит в Windows PowerShell 5.1) не знает -Parameters:
    # Invoke-Pester -Script @{ Path = 'tests\e2e\Test-EdgeCases.ps1';
    #                       Parameters = @{ Slot = 'a90' } }
#>
[CmdletBinding()]
param(
    [string] $CliPath = '',
    [string] $Configuration = 'Debug',
    [string] $Slot = '',
    [int]    $FileSizeBytes = 65536,
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

# Артефакты — в build\ (каталог в .gitignore), отдельной папкой от Test-FullCycle:
# два агента, гоняющих сценарии одновременно, не должны затирать друг другу
# ни журнал пропусков, ни снимок предусловий.
$script:ArtifactsDir = Join-Path (Join-Path $script:RepoRoot 'build') 'e2e-artifacts\edge'
try {
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
} catch {
    $script:ArtifactsDir = Join-Path ([IO.Path]::GetTempPath()) 'mrproper-e2e-edge'
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
}
$script:SkipLog = Join-Path $script:ArtifactsDir 'skipped.log'
$script:PreconditionsPath = Join-Path $script:ArtifactsDir 'preconditions.json'
if (Test-Path -LiteralPath $script:SkipLog) { Remove-Item -LiteralPath $script:SkipLog -Force }

# Состояние прогона. Пропуски пишутся в файл, а не в переменную: файл переживает
# и повторный запуск, и две реализации Pester с разным устройством фаз
# discovery/run, а сводка читает именно его.
$script:LogCounter = 0
$script:Cli = $null
$script:CliMissingReason = ''
$script:SandboxRoot = ''
$script:Fixture = $null
$script:Locker = $null
$script:LockerMarker = ''
$script:LockerStop = ''
$script:UserName = ''
$script:IsElevated = $false
# Путь к самому скрипту: на нём строится самопроверка разбора. $PSCommandPath
# кладём в переменную один раз, потому что внутри блока It значение может
# пересчитываться уже относительно самого Pester.
$script:ScriptPath = $PSCommandPath
if ($script:ScriptPath -eq '') { $script:ScriptPath = $MyInvocation.MyCommand.Path }

if ($null -eq (Get-Command -Name 'Describe' -ErrorAction SilentlyContinue)) {
    throw ('Pester не загружен: запускайте файл через Invoke-Pester, например ' +
        '`Invoke-Pester -Script tests\e2e\Test-EdgeCases.ps1`. Прямой запуск через ' +
        'powershell.exe ничего не проверяет: Describe в этом случае не команда PowerShell.')
}

# ---------------------------------------------------------------------------
# Вспомогательные функции
# ---------------------------------------------------------------------------

function Get-MrProperCli {
    <#
    .SYNOPSIS Найти собранный mrproper_cli.exe.
    .DESCRIPTION Порядок поиска: -CliPath, $env:MRPROPER_CLI, точные пути слота и
    конфигурации, затем все слоты поимённо. Явно запрошенный путь, которого нет,
    молча заменять другим нельзя: сценарий пошёл бы не по той сборке, которую
    проверяют, — причина уходит в skipped.log.
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

function Invoke-ExternalTool {
    <#
    .SYNOPSIS Запустить внешнюю утилиту Windows и вернуть код и вывод.
    .DESCRIPTION Нужна тем же способом, что и для CLI: вывод утилит (cipher,
    icacls, manage-bde) — в консоли PowerShell без [Console]::OutputEncoding он
    приходит в виде кракозябр, а по тексту отказа сценарий и строит причину
    пропуска. Вывод читается по отдельным файлам, поэтому смесь stdout/stderr
    утилит здесь не мешает.
    #>
    param([string] $FilePath, [string[]] $Arguments, [string] $LogDirectory, [string] $Tag)

    $outPath = Join-Path $LogDirectory ($Tag + '.tool-out')
    $errPath = Join-Path $LogDirectory ($Tag + '.tool-err')
    $process = Start-Process -FilePath $FilePath -ArgumentList (ConvertTo-CommandLine $Arguments) `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outPath -RedirectStandardError $errPath

    $outText = ''
    $errText = ''
    if (Test-Path -LiteralPath $outPath) { $outText = Get-Content -LiteralPath $outPath -Raw -Encoding UTF8 }
    if (Test-Path -LiteralPath $errPath) { $errText = Get-Content -LiteralPath $errPath -Raw -Encoding UTF8 }

    return [pscustomobject]@{
        Exit = [int] $process.ExitCode
        Out  = $outText
        Err  = $errText
        All  = (($outText + "`r`n" + $errText).Trim())
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

    $text = $Document | ConvertTo-Json -Depth 16
    [IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

function Write-TextFile {
    <#
    .SYNOPSIS Записать текст в UTF-8 без BOM (для битых входов и дочерних скриптов).
    #>
    param([string] $Path, [string] $Text)

    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
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

function Get-FirstMeaningfulLine {
    <#
    .SYNOPSIS Первая непустая строка вывода — она и объясняет отказ.
    #>
    param([string] $Text)

    foreach ($line in ($Text -split "`r?`n")) {
        $trimmed = $line.Trim()
        if ($trimmed -ne '') { return $trimmed }
    }
    return '(вывод пуст)'
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

function Add-Precondition {
    <#
    .SYNOPSIS Записать в preconditions.json состояние машины, на которой шёл прогон.
    .DESCRIPTION Сценарии §11 п. 3 живут на VM-образах, и результат «зелёный»
    без состояния машины ни о чём не говорит: EFS на томе может быть недоступен,
    BitLocker выключен, а прогон шёл с повышенными правами и проверял совсем другую
    ветку поведения. Снимок пишется по ходу прогона и остаётся рядом с
    skipped.log, чтобы отчёт о прогоне читался без самого Pester.
    #>
    param([string] $Key, $Value)

    $document = [ordered]@{
        cli           = $script:Cli
        configuration = $Configuration
        slot          = $Slot
        user          = $script:UserName
        elevated      = $script:IsElevated
        psVersion     = $PSVersionTable.PSVersion.ToString()
        recordedAt    = (Get-Date).ToString('o')
        facts         = [ordered]@{}
    }
    if (Test-Path -LiteralPath $script:PreconditionsPath -PathType Leaf) {
        try {
            $existing = Get-Content -LiteralPath $script:PreconditionsPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($null -ne $existing.facts) {
                foreach ($property in $existing.facts.PSObject.Properties) {
                    $document.facts[$property.Name] = $property.Value
                }
            }
        } catch {
            # Битый снимок не должен ронять прогон: он диагностический, а не вход.
        }
    }
    $document.facts[$Key] = $Value
    Write-JsonFile -Path $script:PreconditionsPath -Document $document
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

function New-Candidate {
    <#
    .SYNOPSIS Кандидат в формате, который понимает cmd_apply::parseCandidate.
    .DESCRIPTION Набор полей взят из parseCandidate (src/cli/cmd_apply.cpp):
    обязателен только path, остальное имеет осмысленные умолчания. safety —
    safe|review|risky, confidence — 0..100 (больше 100 кандидат отвергается),
    lockedBy — массив {pid, name}. Сценарии защиты добавляют сюда lockedBy и
    locked, поэтому общий конструктор должен уметь и то и другое.
    #>
    param(
        [string] $Path,
        [string] $RuleId = 'e2e.edge',
        [string] $Category = 'system.logs',
        [int64] $Bytes = 4096,
        [string] $Safety = 'safe',
        [int] $Confidence = 95,
        [int] $FileCount = 1,
        $LockedBy = $null,
        [bool] $LockedOnly = $false
    )

    $candidate = [ordered]@{
        ruleId         = $RuleId
        category       = $Category
        path           = $Path
        displayName    = ('Фикстура e2e ' + $RuleId)
        logicalBytes   = $Bytes
        allocatedBytes = $Bytes
        fileCount      = $FileCount
        oldestWrite    = 1600000000
        newestWrite    = 1700000000
        lastAccess     = 1700000000
        safety         = $Safety
        confidence     = $Confidence
        reasons        = @('кандидат e2e-песочницы сценариев защиты')
    }
    if ($LockedOnly) { $candidate['locked'] = $true }
    if ($null -ne $LockedBy) { $candidate['lockedBy'] = $LockedBy }
    return $candidate
}

function New-CandidateDocument {
    <#
    .SYNOPSIS Дамп кандидатов для --candidates (схема 1, kind "scan").
    #>
    param([string] $Path, $Candidates)

    return [ordered]@{
        schema     = 1
        kind       = 'scan'
        candidates = (ConvertTo-CountArray $Candidates)
    }
}

function New-CleanupReportDocument {
    <#
    .SYNOPSIS Отчёт «очистка» по схеме core::report_json с неудачной операцией.
    .DESCRIPTION Нужен, чтобы проверить путь отказа (FR-6, FR-8): ошибка обязана
    дойти до отчёта с путём и HRESULT, итоги обязаны её посчитать, а HTML —
    показать. Кандидатов и операций два: один отказ по правам (0x80070005) и
    один пропуск занятого файла, чтобы в одном документе были обе защиты.
    #>
    param([string] $DeniedPath, [string] $BusyPath, [string] $MissingPath)

    $denied = New-Candidate -Path $DeniedPath -RuleId 'e2e.denied' -Bytes 4096 -FileCount 2
    $busy = New-Candidate -Path $BusyPath -RuleId 'e2e.busy' -Bytes 4096 `
        -LockedBy @([ordered]@{ pid = 4242; name = 'msedge' })
    $gone = New-Candidate -Path $MissingPath -RuleId 'e2e.gone' -Bytes 4096

    return [ordered]@{
        schema  = 1
        kind    = 'cleanup'
        app     = [ordered]@{ version = 'e2e-edge'; pid = 4242; rulesVersion = 'e2e-fixture' }
        os      = [ordered]@{
            caption = 'Microsoft Windows'; version = '10.0.0'; build = 0; architecture = 'x64'
        }
        timing  = [ordered]@{ startedAtUnix = 1700000000; finishedAtUnix = 1700000060; durationMs = 60000 }
        privacy = [ordered]@{ serialsMasked = $true; volumeGuidsMasked = $true }
        candidates = (ConvertTo-CountArray @($denied, $busy, $gone))
        operations = @(
            [ordered]@{
                candidateIndex = 0; action = 'delete'; status = 'failed'
                category = 'system.logs'; displayName = 'Фикстура e2e e2e.denied'
                path = $DeniedPath; bytes = 4096; safety = 'safe'; confidence = 95
                attempts = 3; detail = '0x80070005: Отказано в доступе'
            },
            [ordered]@{
                candidateIndex = 1; action = 'keep'; status = 'skipped'
                category = 'system.logs'; displayName = 'Фикстура e2e e2e.busy'
                path = $BusyPath; bytes = 0; safety = 'safe'; confidence = 95
                attempts = 1; detail = 'файлы держат 1 элемент: msedge'
            },
            [ordered]@{
                candidateIndex = 2; action = 'delete'; status = 'failed'
                category = 'system.logs'; displayName = 'Фикстура e2e e2e.gone'
                path = $MissingPath; bytes = 4096; safety = 'safe'; confidence = 95
                attempts = 2; detail = '0x80070002: Система не может найти указанный файл'
            }
        )
        untouched = @()
        errors    = @(
            [ordered]@{
                scope = 'execute'; code = '0x80070005'
                message = 'Отказано в доступе'; path = $DeniedPath
                operation = 'delete'; atUnix = 1700000030; count = 3
            },
            [ordered]@{
                scope = 'execute'; code = '0x80070002'
                message = 'Система не может найти указанный файл'; path = $MissingPath
                operation = 'delete'; atUnix = 1700000040; count = 2
            }
        )
        notes = 'e2e: отказы по правам и по исчезнувшему файлу'
    }
}

function New-BrokenInputs {
    <#
    .SYNOPSIS Набор заведомо битых входов: чем «отдаёт ошибку» диск.
    .DESCRIPTION Шесть входов, каждый со своим классом отказа: файл не читается
    (ACL, Win32 5), JSON оборван на середине, JSON пустой, файла нет (Win32 2),
    путь длиннее MAX_PATH (Win32 206) и документ, который не является объектом.
    Ни один из них не имеет права превратиться в код 70 (необработанное
    исключение) или в ноль («всё хорошо»).
    #>
    param([string] $Directory, [string] $LongPath)

    $unreadable = Join-Path $Directory 'denied-in.json'
    Write-TextFile -Path $unreadable -Text '{"schema":1,"kind":"scan"}'
    # | Out-Null обязателен: Set-DenyAce возвращает результат утилиты, и без
    # этого в возвращаемый массив попал бы он, а список сместился бы на один.
    Set-DenyAce -Path $unreadable -Rights 'R' | Out-Null

    $truncated = Join-Path $Directory 'truncated.json'
    Write-TextFile -Path $truncated -Text '{"schema":1,"kind":"scan","candidates":[{"ruleId":"a","categ'

    $empty = Join-Path $Directory 'empty.json'
    Write-TextFile -Path $empty -Text ''

    $missing = Join-Path $Directory 'missing.json'
    $scalar = Join-Path $Directory 'scalar.json'
    Write-TextFile -Path $scalar -Text '[]'

    return @(
        [pscustomobject]@{ Name = 'нечитаемый файл (ACL, Win32 5)'; Path = $unreadable; Expected = 66 }
        [pscustomobject]@{ Name = 'оборванный JSON'; Path = $truncated; Expected = 65 }
        [pscustomobject]@{ Name = 'пустой файл'; Path = $empty; Expected = 65 }
        [pscustomobject]@{ Name = 'отсутствующий файл (Win32 2)'; Path = $missing; Expected = 66 }
        [pscustomobject]@{ Name = 'документ-массив вместо объекта'; Path = $scalar; Expected = 65 }
        [pscustomobject]@{ Name = 'путь длиннее MAX_PATH (Win32 206)'; Path = (Join-Path $LongPath 'out.json'); Expected = 66 }
    )
}

function Set-DenyAce {
    <#
    .SYNOPSIS Запретить текущему пользователю права (ACL) на пути.
    .DESCRIPTION Права вешаются на объекты, созданные скриптом из-под текущего
    пользователя, поэтому повышения не нужно: deny упорнее allow даже на своём
    файле. Права: R — чтение, F — всё, WD/AD/WEA/WA — запись в каталог, добавление
    файлов, изменение и удаление записей. Последний набор выбран узким намеренно:
    он запрещает изменения, но оставляет перечисление каталога, иначе сценарий
    не смог бы доказать, что файл на месте, и «ничего не пропало» пришлось бы
    заменить догадкой.
    #>
    param([string] $Path, [string] $Rights)

    $tool = Invoke-ExternalTool -FilePath 'icacls.exe' -LogDirectory $script:SandboxRoot -Tag 'icacls-deny' `
        -Arguments @($Path, '/deny', ($script:UserName + ':(' + $Rights + ')'))
    return $tool
}

function Remove-DenyAce {
    <#
    .SYNOPSIS Снять запреты текущего пользователя с дерева песочницы.
    .DESCRIPTION Без этого шага песочница не удаляется: у каталога с deny (F)
    нет прав на удаление, и сценарий оставлял бы мусор в %TEMP%. /T обязателен —
    deny может висеть на созданном после него файле, а снятие только с
    каталога дерево не разблокирует. /C — чтобы один упрямый объект не сорвал
    уборку остальных.
    #>
    param([string] $Root)

    if ($script:UserName -eq '') { return }
    $null = Invoke-ExternalTool -FilePath 'icacls.exe' -LogDirectory $script:ArtifactsDir -Tag 'icacls-clean' `
        -Arguments @($Root, '/remove:d', $script:UserName, '/T', '/C')
}

function Test-AccessDenied {
    <#
    .SYNOPSIS Запрет на пути действительно действует — самопроверка фикстуры.
    .DESCRIPTION Без этой проверки сценарий «нет прав» проверял бы воздух: если
    deny не сработал, отказ программы ничего не доказывает. Возвращает $true,
    когда операция действительно упала.
    #>
    param([string] $Path, [ValidateSet('read', 'write')] [string] $Mode)

    try {
        if ($Mode -eq 'read') {
            $null = [IO.File]::ReadAllText($Path)
        } else {
            $null = [IO.File]::WriteAllText($Path, 'e2e probe')
        }
        return $false
    } catch {
        return $true
    }
}

function Test-FileLocked {
    <#
    .SYNOPSIS Файл действительно занят другим процессом.
    .DESCRIPTION Фикстура «занятого файла» проверяется открытием с
    FileShare.None: если открыть удалось, никакой блокировки нет и сценарий
    «приложение держит файл» был бы пустым.
    #>
    param([string] $Path)

    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        return $false
    } catch {
        return $true
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }
}

function Start-FileLocker {
    <#
    .SYNOPSIS Запустить процесс, который держит файл эксклюзивно.
    .DESCRIPTION Держатель — отдельный powershell.exe: держать файл должен
    ДРУГОЙ процесс, иначе «занятость» доказывает только сам тест. Ребёнок
    открывает файл с FileShare.None, ставит маркер (по нему родитель понимает,
    что блокировка взята) и ждёт, пока появится стоп-маркер; стоп-маркер
    гарантирует, что ребёнок завершится сам, а Kill останется страховкой на
    случай, если он зависнет. Время жизни ребёнка ограничено, «забытый»
    держатель не должен висеть после прогона.
    #>
    param([string] $Path, [string] $Marker, [string] $Stop, [string] $ScriptPath)

    $code = @"
`$ErrorActionPreference = 'Stop'
`$handle = [IO.File]::Open('$Path', [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    [IO.File]::WriteAllText('$Marker', 'held')
    while (-not (Test-Path -LiteralPath '$Stop')) { Start-Sleep -Milliseconds 100 }
} finally {
    `$handle.Dispose()
}
"@
    Write-TextFile -Path $ScriptPath -Text $code

    $process = Start-Process -FilePath 'powershell.exe' `
        -ArgumentList (ConvertTo-CommandLine @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath)) `
        -PassThru -WindowStyle Hidden

    # Маркер ждём до 15 с: холодный старт powershell.exe на VM-образе бывает
    # медленным, и молчаливый выход «держателя не дождались» сделал бы шаг пустым.
    $ready = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if (Test-Path -LiteralPath $Marker) { $ready = $true; break }
        if ($process.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    return [pscustomobject]@{ Process = $process; Ready = $ready }
}

function Stop-FileLocker {
    <#
    .SYNOPSIS Отпустить держателя файла и дождаться его выхода.
    #>
    param($Handle)

    if ($null -eq $Handle -or $null -eq $Handle.Process) { return }
    try {
        if ($script:LockerStop -ne '') { Write-TextFile -Path $script:LockerStop -Text 'stop' }
        if (-not $Handle.Process.WaitForExit(15000)) {
            $Handle.Process.Kill()
            $Handle.Process.WaitForExit(5000) | Out-Null
        }
    } catch {
        # Держатель уже мёртв — это не ошибка сценария, а его нормальное состояние.
    }
}

function Remove-ReparseLoop {
    <#
    .SYNOPSIS Снять junction-петлю, не заходя внутрь неё.
    .DESCRIPTION rmdir на junction удаляет саму точку, а не цель: Remove-Item
    -Recurse по такому каталогу уходит вглубь на двенадцать уровней и тратит на
    это секунды, поэтому петля снимается первой и явно.
    #>
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path)) { return }
    $null = Invoke-ExternalTool -FilePath 'cmd.exe' -LogDirectory $script:ArtifactsDir -Tag 'rmdir-loop' `
        -Arguments @('/c', 'rmdir', $Path)
}

function New-EdgeFixture {
    <#
    .SYNOPSIS Подготовить песочницу сценариев защиты.
    .DESCRIPTION Четыре настоящих отказа, а не заглушки:
      * busy     — каталог с файлом под эксклюзивным держателем (Win32 32/33);
      * denied   — каталог с deny (F) текущему пользователю (Win32 5);
      * encrypted— каталог под EFS (cipher /e), файл помечается атрибутом «U»;
      * longpath — вложенный путь длиннее 260 символов (Win32 206);
      * loop     — junction, указывающий на своего родителя (Win32 4390);
      * bad      — шесть битых входов (см. New-BrokenInputs).
    Файлы заполняются не нулями: нули NTFS уводят файл в sparse, и тогда
    освобождение на томе не равно сумме bytes — сверять было бы не с чем.
    #>
    param([string] $Root, [int] $SizeBytes)

    if (-not (Test-Path -LiteralPath $Root)) { New-Item -ItemType Directory -Path $Root -Force | Out-Null }
    $random = New-Object System.Random 20250928
    $buffer = New-Object byte[] $SizeBytes
    $random.NextBytes($buffer)

    # Занятый файл: каталог-кандидат и файл внутри него.
    $busyDir = Join-Path $Root 'busy'
    New-Item -ItemType Directory -Path $busyDir -Force | Out-Null
    $busyFile = Join-Path $busyDir 'in-use.bin'
    [IO.File]::WriteAllBytes($busyFile, $buffer)

    # Недоступный каталог: файл кладём ДО deny, иначе запрет на запись не даст
    # его создать. На каталог вешаем запрет на изменение, на файл — на чтение:
    # так запрет настоящий (создать, изменить и прочитать нельзя), а перечисление
    # каталога остаётся доступным, и сценарий может проверить, что файл цел.
    $deniedDir = Join-Path $Root 'denied'
    New-Item -ItemType Directory -Path $deniedDir -Force | Out-Null
    $deniedFile = Join-Path $deniedDir 'system.log'
    [IO.File]::WriteAllBytes($deniedFile, $buffer)
    Set-DenyAce -Path $deniedDir -Rights 'WD,AD,WEA,WA' | Out-Null
    Set-DenyAce -Path $deniedFile -Rights 'R' | Out-Null

    # EFS: сначала каталог под шифрованием, и только потом файл. Обратный порядок
    # даёт каталог с пометкой «новые файлы будут зашифрованы» и незашифрованный
    # файл, то есть фикстуру, которая выглядит зашифрованной и ничего не проверяет.
    $encryptedDir = Join-Path $Root 'encrypted'
    New-Item -ItemType Directory -Path $encryptedDir -Force | Out-Null
    $cipher = Invoke-ExternalTool -FilePath 'cipher.exe' -LogDirectory $script:SandboxRoot `
        -Tag 'cipher-e' -Arguments @('/e', $encryptedDir)
    $encryptedFile = Join-Path $encryptedDir 'secret.bin'
    [IO.File]::WriteAllBytes($encryptedFile, $buffer)
    $efsOk = $false
    if (Test-Path -LiteralPath $encryptedFile -PathType Leaf) {
        # Признак шифрования — бит FILE_ATTRIBUTE_ENCRYPTED, а не текст вывода
        # cipher: тот локализован, и на немецкой или китайской Windows сценарий
        # иначе объявил бы рабочий том негодным.
        $efsOk = [bool] ((Get-Item -LiteralPath $encryptedFile).Attributes -band [IO.FileAttributes]::Encrypted)
    }

    # Длинный путь: каждый сегмент короче 255, а весь путь — длиннее 260.
    $longDir = $Root
    $segment = 's' * 60
    $longPathCreated = $true
    for ($level = 0; $level -lt 5; $level++) {
        $longDir = Join-Path $longDir $segment
        try {
            New-Item -ItemType Directory -Path $longDir -Force | Out-Null
        } catch {
            $longPathCreated = $false
            break
        }
    }

    # Репarse-петля: junction на каталог, в котором он сам и лежит.
    $loopPath = Join-Path $Root 'loop'
    $loopMade = (Test-Path -LiteralPath $loopPath)
    if (-not $loopMade) {
        $null = Invoke-ExternalTool -FilePath 'cmd.exe' -LogDirectory $script:SandboxRoot -Tag 'mklink' `
            -Arguments @('/c', 'mklink', '/J', $loopPath, $Root)
        $loopMade = (Test-Path -LiteralPath $loopPath)
    }

    $badDir = Join-Path $Root 'bad-inputs'
    New-Item -ItemType Directory -Path $badDir -Force | Out-Null
    $bad = New-BrokenInputs -Directory $badDir -LongPath $longDir

    return [pscustomobject]@{
        Root          = $Root
        SizeBytes     = $SizeBytes
        BusyDir       = $busyDir
        BusyFile      = $busyFile
        DeniedDir     = $deniedDir
        DeniedFile    = $deniedFile
        EncryptedDir  = $encryptedDir
        EncryptedFile = $encryptedFile
        EfsOk         = $efsOk
        EfsReason     = $(if ($efsOk) { '' } else { 'cipher /e вернул ' + $cipher.Exit + ': ' + (Get-FirstMeaningfulLine $cipher.All) })
        LongPath      = $longDir
        LongPathOk    = $longPathCreated
        LoopPath      = $loopPath
        LoopOk        = $loopMade
        BadInputs     = $bad
        BadDir        = $badDir
    }
}

function Get-VolumeFreeBytes {
    <#
    .SYNOPSIS Свободное место на томе, где лежит путь.
    .DESCRIPTION AvailableFreeSpace, а не TotalFreeSpace: интересует то, что
    доступно текущему пользователю, иначе квота исказит сверку.
    #>
    param([string] $Path)

    $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
    $drive = New-Object System.IO.DriveInfo($root)
    return [int64] $drive.AvailableFreeSpace
}

function Get-TreeFiles {
    <#
    .SYNOPSIS Все файлы каталога: количество и сумма их размеров.
    .DESCRIPTION -ErrorAction SilentlyContinue не для красоты: каталоги под deny и
    reparse-петлю обходить нельзя, и сценарий обязан увидеть «сколько смог»,
    а не падать на первом отказе.
    #>
    param([string] $Root)

    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue)
    $bytes = [int64] 0
    foreach ($file in $files) { $bytes += $file.Length }
    return [pscustomobject]@{ Count = $files.Count; Bytes = $bytes }
}

function New-FixtureSnapshot {
    <#
    .SYNOPSIS Снимок состояния, которым владеет сценарий: дерево фикстуры и место на томе.
    .DESCRIPTION «Ничего не удалилось» — это утверждение о песочнице, а не обо всём
    томе. Том общий: соседние агенты пишут в него своими сборками, и прогон G4
    упал ровно на этом — свободное место уехало на 1.36 МиБ при допуске 1 МиБ,
    хотя утилита ничего не удаляла (допуск в 1 МиБ при фикстуре 64 КиБ и не ловил
    ничего: за ним не видно ни удаления файла, ни переноса в корзину). Дерево
    песочницы — то, что сценарий создал сам и что никто чужой не трогает.
    Свободное место в снимке остаётся: это полезное наблюдение о машине, оно
    уходит в preconditions.json через Assert-FixtureUnchanged.
    #>
    param($Fixture)

    return [pscustomobject]@{
        Tree      = (Get-TreeFiles $Fixture.Root)
        FreeBytes = [int64] (Get-VolumeFreeBytes $script:SandboxRoot)
    }
}

function Assert-FixtureUnchanged {
    <#
    .SYNOPSIS Проверить, что песочница не изменилась, и записать место на томе.
    .DESCRIPTION Проверка точная, без допуска: любое удаление, любой перенос в
    корзину и любое создание файла меняют либо количество файлов, либо сумму
    байт. Проверять надо и после освобождения блокировки держателем, и до него:
    сценарий без этого проверял бы только «файл не удалили», а не «и не должны
    были удалять, пока держат».
    #>
    param($Before, $Fixture, [string] $Label)

    $after = New-FixtureSnapshot -Fixture $Fixture
    $after.Tree.Count | Should Be $Before.Tree.Count
    $after.Tree.Bytes | Should Be $Before.Tree.Bytes
    Add-Precondition -Key ('volumeFreeSpace.' + $Label) -Value ([ordered]@{
            before = $Before.FreeBytes
            after  = $after.FreeBytes
            delta  = ($after.FreeBytes - $Before.FreeBytes)
        })
}

# ---------------------------------------------------------------------------
# Сценарии
# ---------------------------------------------------------------------------

Describe 'MrProper: сценарии защиты (SPEC §11 п.3, §12)' -Tag 'e2e' {

    BeforeEach {
        $script:Cli = Get-MrProperCli -Requested $CliPath -Configuration $Configuration -Slot $Slot
        if ($null -eq $script:Cli) {
            $script:CliMissingReason = ('не найден mrproper_cli.exe (соберите tools\build.bat ' +
                $Configuration + ' <слот> или укажите -CliPath)')
        } else {
            $script:CliMissingReason = ''
        }

        # Личность процесса — часть предусловий, а не украшение: сценарий
        # «отсутствие прав» имеет смысл только на непривилегированном прогоне,
        # а на повышенном он проверял бы совсем другую ветку.
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $script:UserName = $identity.Name
        $principal = New-Object Security.Principal.WindowsPrincipal($identity)
        $script:IsElevated = [bool] $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
        Add-Precondition -Key 'identity' -Value ([ordered]@{
                user     = $script:UserName
                elevated = $script:IsElevated
                cli      = $script:Cli
            })

        $script:Locker = $null
        $script:SandboxRoot = Join-Path (Get-SandboxRoot) ('edge-' + $PID)
        if (Test-Path -LiteralPath $script:SandboxRoot) {
            Remove-DenyAce -Root $script:SandboxRoot
            Remove-Item -LiteralPath $script:SandboxRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        New-Item -ItemType Directory -Path $script:SandboxRoot -Force | Out-Null

        $script:LockerMarker = Join-Path $script:SandboxRoot 'held.marker'
        $script:LockerStop = Join-Path $script:SandboxRoot 'stop.marker'
        $script:Fixture = New-EdgeFixture -Root (Join-Path $script:SandboxRoot 'edge') -SizeBytes $FileSizeBytes
    }

    AfterEach {
        if ($null -ne $script:Locker) { Stop-FileLocker -Handle $script:Locker; $script:Locker = $null }
        if ($KeepArtifacts) {
            Write-Host ('песочница оставлена: ' + $script:SandboxRoot)
            return
        }
        # Порядок уборки обязателен: сначала петля (иначе Remove-Item уходит в
        # неё вглубь), потом deny — с запретами песочница не удалится и следующий
        # прогон упрётся в её остатки.
        if ($null -ne $script:Fixture) { Remove-ReparseLoop -Path $script:Fixture.LoopPath }
        Remove-DenyAce -Root $script:SandboxRoot
        if (Test-Path -LiteralPath $script:SandboxRoot) {
            Remove-Item -LiteralPath $script:SandboxRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Context 'фикстуры и сам сценарий' {

        It 'скрипт разбирается парсером PowerShell без ошибок' {
            # Критерий приёмки самой задачи. Проверяем не «запустилось», а именно
            # разбор: незакрытая скобка или русский текст без BOM дают ошибку
            # парсера уже при загрузке файла, и до первого It дело не доходит.
            $tokens = $null
            $parseErrors = $null
            $null = [System.Management.Automation.Language.Parser]::ParseFile(
                $script:ScriptPath, [ref] $tokens, [ref] $parseErrors)
            $parseErrors.Count | Should Be 0
            $tokens.Count | Should BeGreaterThan 0
        }

        It 'песочница создана: файлы на месте, байты совпадают с ожиданием' {
            $fixture = $script:Fixture
            (Test-Path -LiteralPath $fixture.BusyFile -PathType Leaf) | Should Be $true
            (Test-Path -LiteralPath $fixture.DeniedFile -PathType Leaf) | Should Be $true
            (Test-Path -LiteralPath $fixture.EncryptedFile -PathType Leaf) | Should Be $true
            (Get-Item -LiteralPath $fixture.BusyFile).Length | Should Be $FileSizeBytes
            (Get-Item -LiteralPath $fixture.EncryptedFile).Length | Should Be $FileSizeBytes
            # Файл под deny обязан существовать: иначе сценарий «нет прав»
            # проверял бы несуществующий путь и проходил бы вхолостую.
            (Get-Item -LiteralPath $fixture.DeniedFile).Length | Should Be $FileSizeBytes
        }

        It 'фикстуры блокировки, запрета и шифрования настоящие' {
            $fixture = $script:Fixture

            # Запрет на каталоге действительно действует: создать файл нельзя
            # (Win32 5), а перечисление и чтение атрибутов — можно.
            (Test-AccessDenied -Path (Join-Path $fixture.DeniedDir 'probe.tmp') -Mode 'write') | Should Be $true
            (Test-AccessDenied -Path $fixture.DeniedFile -Mode 'read') | Should Be $true
            (Get-Item -LiteralPath $fixture.DeniedFile).Length | Should Be $FileSizeBytes

            # Шифрование тома поддерживается, и файл действительно зашифрован.
            # Признак — бит FILE_ATTRIBUTE_ENCRYPTED: текст cipher /c локализован
            # и в другой локали не нашёлся бы вовсе.
            if (-not $fixture.EfsOk) {
                Add-Precondition -Key 'efs' -Value ('не поддерживается: ' + $fixture.EfsReason)
                Add-SkippedStep ('EFS недоступен на томе песочницы: ' + $fixture.EfsReason)
            } else {
                $null = Invoke-ExternalTool -FilePath 'cipher.exe' -LogDirectory $script:SandboxRoot `
                    -Tag 'cipher-c' -Arguments @('/c', $fixture.EncryptedFile)
                Add-Precondition -Key 'efs' -Value 'каталог зашифрован cipher /e, бит Encrypted на файле проверен'
                ((Get-Item -LiteralPath $fixture.EncryptedFile).Attributes -band
                    [IO.FileAttributes]::Encrypted) | Should Not Be 0
            }

            # Репarse-точка — junction, а не обычный каталог: иначе «петля» в
            # сценарии про репarse была бы подменой.
            if ($fixture.LoopOk) {
                (Get-Item -LiteralPath $fixture.LoopPath).LinkType | Should Not BeNullOrEmpty
            }
        }
    }

    Context 'сценарий 1: приложение держит файл' {

        It 'эксклюзивный держатель действительно держит файл, и прогон его не убирает' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            $locker = Start-FileLocker -Path $fixture.BusyFile -Marker $script:LockerMarker `
                -Stop $script:LockerStop -ScriptPath (Join-Path $script:SandboxRoot 'lock-holder.ps1')
            $script:Locker = $locker
            if (-not $locker.Ready) {
                Add-SkippedStep ('держатель файла не взял блокировку на ' + $fixture.BusyFile +
                    ' — сценарий «приложение держит файл» без настоящей блокировки не проверяет ничего')
                return
            }
            (Test-FileLocked $fixture.BusyFile) | Should Be $true

            $candidatesPath = Join-Path $script:SandboxRoot 'busy-candidates.json'
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath `
                    -Candidates @(New-Candidate -Path $fixture.BusyDir -RuleId 'e2e.busy' -Bytes $FileSizeBytes))

            $snapshot = New-FixtureSnapshot -Fixture $fixture
            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $candidatesPath)
            $apply = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--yes', '--json', '--candidates', $candidatesPath)

            # Планирование и сухой прогон обязаны отработать: они не трогают ФС.
            $plan.Exit | Should Be 0
            $apply.Exit | Should Be 0
            $applyDocument = $apply.Out | ConvertFrom-Json
            (Get-RequiredProperty $applyDocument 'executed' 'apply') | Should Be $false
            # Исполнения в сухом прогоне нет вовсе, а не «нулевое». Проверка
            # идёт булевым выражением, а не Should BeNullOrEmpty: пустая строка
            # и $null до Pester через конвейер не доходят, и проверка молча
            # падала бы на «нет значения» вместо самого утверждения.
            ($null -eq (Get-RequiredProperty $applyDocument 'execution' 'apply')) | Should Be $true

            # Главный инвариант: чужой процесс держит файл — значит, файл стоит
            # на месте, сколько бы прогон ни шёл. Проверять это надо и после
            # освобождения блокировки: сценарий без этого проверял бы только
            # «файл не удалили», а не «и не должен были удалять, пока держат».
            (Test-Path -LiteralPath $fixture.BusyFile -PathType Leaf) | Should Be $true
            (Get-Item -LiteralPath $fixture.BusyFile).Length | Should Be $FileSizeBytes
            Assert-FixtureUnchanged -Before $snapshot -Fixture $fixture -Label 'busy'

            Stop-FileLocker -Handle $locker
            $script:Locker = $null
            (Test-FileLocked $fixture.BusyFile) | Should Be $false
        }

        It 'lockedBy из отчёта сканера: план помечает skip-locked, и никакой профиль этого не отменяет' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            $candidatesPath = Join-Path $script:SandboxRoot 'locked-candidates.json'
            # fileCount задан явно и обязан совпадать с числом файлов в каталоге:
            # секции manifest в дампе нет (её пишет настоящий скан), поэтому
            # список разрешённого CLI восстанавливает перечислением корня — но
            # берёт его ТОЛЬКО при совпадении с объявленным fileCount
            # (src/cli/cmd_apply.cpp, synthesizeManifest). Расхождение означало бы
            # needs-enumeration у обоих элементов: план снулём 0 операций вместо
            # «занятое пропустили, свободное в корзину». Второй кандидат —
            # контрольный: он свободен, и его судьба отличает «занятое пропустили»
            # от «ничего не выбрали вообще».
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath -Candidates @(
                    New-Candidate -Path $fixture.BusyDir -RuleId 'e2e.busy' -Bytes $FileSizeBytes -FileCount 1 `
                    -LockedBy @([ordered]@{ pid = 4242; name = 'msedge' }, [ordered]@{ pid = 4343; name = 'Photos' })
                    New-Candidate -Path $fixture.DeniedDir -RuleId 'e2e.free' -Bytes $FileSizeBytes -FileCount 1
                ))

            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $candidatesPath)
            $plan.Exit | Should Be 0
            $document = $plan.Out | ConvertFrom-Json

            # FR-5 Skip (locked) / FR-6: файл держит приложение — элемент не
            # удаляется, место не обещается, причина называет держателя.
            (Get-RequiredProperty $document 'totals.byAction.skipLocked.count' 'plan') | Should Be 1
            (Get-RequiredProperty $document 'totals.byAction.skipLocked.bytes' 'plan') | Should Be 0
            (Get-RequiredProperty $document 'totals.byAction.trash.count' 'plan') | Should Be 1
            $untouched = ConvertTo-CountArray (Get-RequiredProperty $document 'untouched' 'plan')
            $untouched.Count | Should Be 1
            (Get-RequiredProperty $untouched[0] 'action' 'plan.untouched[]') | Should Be 'skip-locked'
            (Get-RequiredProperty $untouched[0] 'skipReason' 'plan.untouched[]') | Should Be 'locked'
            (Get-RequiredProperty $untouched[0] 'bytes' 'plan.untouched[]') | Should Be 0
            $reason = [string] (Get-RequiredProperty $untouched[0] 'reason' 'plan.untouched[]')
            $reason | Should Match 'msedge'
            $reason | Should Match 'Photos'
            # В operations элемента быть не должно: skip-locked — это «не трогаем».
            (ConvertTo-CountArray (Get-RequiredProperty $document 'operations' 'plan')).Count | Should Be 1

            # Повышенные права пользователя (--allow-risky) и профиль «выбрать
            # всё» не отменяют блокировку: decideCandidate смотрит на lockedBy
            # первым, и осознанно — «выбрать всё» не значит «удалять занятое».
            $forced = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--allow-risky', '--profile', 'everything',
                '--no-trash', '--candidates', $candidatesPath)
            $forced.Exit | Should Be 0
            $forcedDocument = $forced.Out | ConvertFrom-Json
            (Get-RequiredProperty $forcedDocument 'totals.byAction.skipLocked.count' 'plan') | Should Be 1
            (Get-RequiredProperty $forcedDocument 'totals.selected' 'plan') | Should Be 1
            (Get-RequiredProperty $forcedDocument 'options.allowRisky' 'plan') | Should Be $true

            # Тот же план глазами apply: исполнения нет, refused нет, skip-locked на месте.
            $apply = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--yes', '--json', '--candidates', $candidatesPath)
            $apply.Exit | Should Be 0
            $applyDocument = $apply.Out | ConvertFrom-Json
            (Get-RequiredProperty $applyDocument 'plan.totals.byAction.skipLocked.count' 'apply') | Should Be 1
            (Get-RequiredProperty $applyDocument 'executed' 'apply') | Should Be $false

            # Файл занят другим процессом — он и после всех прогонов на месте.
            (Test-Path -LiteralPath $fixture.BusyFile -PathType Leaf) | Should Be $true
        }

        It 'locked: true без lockedBy — кандидат отвергнут, а не выбран к удалению' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            $candidatesPath = Join-Path $script:SandboxRoot 'locked-flag-candidates.json'
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath `
                    -Candidates @(New-Candidate -Path $fixture.BusyDir -RuleId 'e2e.busy' -LockedOnly $true))

            $snapshot = New-FixtureSnapshot -Fixture $fixture
            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--allow-risky', '--profile', 'everything', '--candidates', $candidatesPath)

            # 4 = NoCandidates (src/cli/cmd_apply.hpp): список не разобран, и это
            # отказ, а не план с нулём операций. Главное — элемент не попал в
            # план на удаление: «занято» без указания, кем, непроверяемо.
            $plan.Exit | Should Be 4
            ([string]::IsNullOrEmpty($plan.Out)) | Should Be $true
            $plan.Err | Should Match 'lockedBy'
            (Test-Path -LiteralPath $fixture.BusyFile -PathType Leaf) | Should Be $true
            Assert-FixtureUnchanged -Before $snapshot -Fixture $fixture -Label 'lockedWithoutLockedBy'
        }
    }

    Context 'сценарий 2: ошибки ввода-вывода вместо битых секторов' {

        It 'нечитаемый вход: отказ назван кодом 66 и путём, а не 70 и не нулём' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture
            # $input — автоматическая переменная PowerShell, а не наш счётчик:
            # обращение $input.Expected к ней не работает, и ошибка выглядит
            # как «параметр Expected не найден» вместо отказа программы.
            $badCase = $fixture.BadInputs[0]
            $badCase.Expected | Should Be 66

            (Test-AccessDenied -Path $badCase.Path -Mode 'read') | Should Be $true
            $report = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $badCase.Path, '--out', (Join-Path $script:SandboxRoot 'out.json'))

            # 66 = нет входа (src/cli/cmd_report.cpp): файл не открылся на
            # чтение. Отказ обязан называть путь — иначе в логе не видно, что
            # именно сломалось (§12: «все ошибки в логе с путём и HRESULT»).
            $report.Exit | Should Be 66
            $report.Err | Should Match ([regex]::Escape($badCase.Path))
            ([string]::IsNullOrEmpty($report.Err)) | Should Be $false
            (Test-Path -LiteralPath (Join-Path $script:SandboxRoot 'out.json')) | Should Be $false
        }

        It 'оборванный, пустой и не-объектный JSON: код 65, а не 70' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $bad = $script:Fixture.BadInputs

            # 65 = данные не годятся (src/cli/cmd_report.cpp). Диск, отдавший
            # половину файла, выглядит ровно так же — и разбираться он не должен.
            #
            # Про путь в stderr: при разборе JSON команда называет ПРИЧИНУ, а не
            # файл (путь и так известен вызывающему — он его и передал), и путь
            # в stderr появляется при отказе в открытии и при негодной схеме.
            # Поэтому «назван путь» проверяется там, где команда правда обещает
            # его назвать, а здесь — что отказ назван причиной и что отказы
            # разных классов не сливаются в одно «данные не годятся».
            $truncated = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $bad[1].Path, '--out', (Join-Path $script:SandboxRoot 'o1.json'))
            $truncated.Exit | Should Be 65
            $truncated.Err | Should Match 'JSON'
            ([string]::IsNullOrEmpty($truncated.Out)) | Should Be $true
            # Ничего «наполовину успешного»: оборванный вход не даёт ни отчёта,
            # ни файла на выходе.
            (Test-Path -LiteralPath (Join-Path $script:SandboxRoot 'o1.json')) | Should Be $false

            $empty = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $bad[2].Path, '--out', (Join-Path $script:SandboxRoot 'o2.json'))
            $empty.Exit | Should Be 65
            $empty.Err | Should Match 'JSON'
            ($truncated.Err -ne $empty.Err) | Should Be $true

            $scalar = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $bad[4].Path, '--out', (Join-Path $script:SandboxRoot 'o3.json'))
            $scalar.Exit | Should Be 65
            $scalar.Err | Should Match 'JSON'
            # Три разных класса отказа дали три разные причины: оборванный и
            # пустой вход ломают разбор, а массив вместо объекта проходит разбор
            # и отвергается проверкой схемы. Сценарий обязан различать их, иначе
            # «данные не годятся» остаётся безличной строкой.
            ($truncated.Err -ne $scalar.Err) | Should Be $true
        }

        It 'отсутствующий вход и путь длиннее MAX_PATH: 66, и stderr называет путь' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            $missing = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $fixture.BadInputs[3].Path,
                '--out', (Join-Path $script:SandboxRoot 'o4.json'))
            $missing.Exit | Should Be 66
            $missing.Err | Should Match ([regex]::Escape($fixture.BadInputs[3].Path))

            if (-not $fixture.LongPathOk) {
                Add-SkippedStep ('путь длиннее 260 символов не создался на этом томе: ' + $fixture.LongPath +
                    ' — сценарий Win32 206 не проверяется')
                return
            }
            # Каталог создался, а запись в него — нет: тот же отказ, что даёт
            # длинный путь на реальном мусоре. На вход кладётся заведомо годный
            # документ: иначе команда откажется на разборе входа (65) и до
            # записи не дойдёт, а проверять тут именно запись.
            ($fixture.LongPath.Length -gt 260) | Should Be $true
            $validSource = Join-Path $fixture.BadDir 'valid-source.json'
            Write-JsonFile -Path $validSource -Document (New-CandidateDocument -Path $validSource `
                    -Candidates @(New-Candidate -Path $fixture.BusyDir -RuleId 'e2e.busy' -Bytes 4096))
            $long = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $validSource, '--out', (Join-Path $fixture.LongPath 'out.json'))
            $long.Exit | Should Be 66
            $long.Err | Should Match ([regex]::Escape('out.json'))
        }

        It 'файл исчез между сканом и очисткой: план строится, а отчёт хранит HRESULT и путь' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            # Кандидат указывает на путь, которого уже нет (Win32 2). Планировщик
            # работает по дампу скана, поэтому обязан построить план, а не упасть.
            $gonePath = Join-Path (Join-Path $fixture.Root 'busy') 'vanished.bin'
            $candidatesPath = Join-Path $script:SandboxRoot 'gone-candidates.json'
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath `
                    -Candidates @(New-Candidate -Path $gonePath -RuleId 'e2e.gone' -Bytes 4096))
            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $candidatesPath)
            $plan.Exit | Should Be 0
            (Test-Path -LiteralPath $gonePath) | Should Be $false

            # Дальше — путь отказа в отчёт: ошибка обязана дойти до артефакта с
            # путём и HRESULT, попасть в итоги и быть видна в HTML (FR-8, §12).
            $reportSource = Join-Path $script:SandboxRoot 'cleanup-source.json'
            Write-JsonFile -Path $reportSource -Document (New-CleanupReportDocument `
                    -DeniedPath $fixture.DeniedDir -BusyPath $fixture.BusyDir -MissingPath $gonePath)
            $outputPath = Join-Path $script:SandboxRoot 'cleanup-report.json'
            $report = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $reportSource, '--out', $outputPath)
            $report.Exit | Should Be 0
            $document = Get-Content -LiteralPath $outputPath -Raw -Encoding UTF8 | ConvertFrom-Json

            $errors = ConvertTo-CountArray (Get-RequiredProperty $document 'errors' 'report')
            $errors.Count | Should Be 2
            $codes = @()
            foreach ($item in $errors) {
                $codes += [string] (Get-RequiredProperty $item 'code' 'report.errors[]')
                ([string] (Get-RequiredProperty $item 'path' 'report.errors[]')) | Should Not BeNullOrEmpty
            }
            # Коды склеиваются в строку сами: Should Match по массиву проверяет
            # элементы по очереди и падает на первом же несовпадении, а нужна
            # проверка «оба кода в отчёте есть».
            ($codes -join ' ') | Should Match '0x80070005'
            ($codes -join ' ') | Should Match '0x80070002'
            # Повторы посчитаны, а не схлопнуты (FR-6: retry с backoff).
            (Get-RequiredProperty $document 'totals.errorCount' 'report') | Should Be 2
            (Get-RequiredProperty $document 'totals.errorOccurrences' 'report') | Should Be 5
            (Get-RequiredProperty $document 'totals.failedCount' 'report') | Should Be 2
            (Get-RequiredProperty $document 'totals.skippedCount' 'report') | Should Be 1
            # Занятый файл учтён как занятый, а не как удалённый.
            (Get-RequiredProperty $document 'totals.lockedCandidateCount' 'report') | Should Be 1

            $htmlPath = Join-Path $script:SandboxRoot 'cleanup-report.html'
            $html = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--html', '--in', $reportSource, '--out', $htmlPath,
                '--title', 'e2e: отказы по защите', '--lang', 'ru')
            $html.Exit | Should Be 0
            $markup = Get-Content -LiteralPath $htmlPath -Raw -Encoding UTF8
            $markup | Should Match ([regex]::Escape($fixture.DeniedDir))
            $markup | Should Match '0x80070005'
            $markup | Should Match ([regex]::Escape($gonePath))
            # Отчёт с ошибками остаётся самодостаточным (§12: открывается без
            # приложения) — иначе баг-репорт о «нет прав» не отправить.
            ([regex]::Matches($markup, '(?i)(src|href)\s*=\s*["'']?\s*(https?:)?//')).Count | Should Be 0
        }

        It 'шесть битых входов подряд: ни одного необработанного исключения и ни одного ложного нуля' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            $seen = @()
            foreach ($badCase in $fixture.BadInputs) {
                $result = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                    -Arguments @('report', '--json', '--in', $badCase.Path,
                    '--out', (Join-Path $script:SandboxRoot ('o-' + $badCase.Expected + '-' + $seen.Count + '.json')))
                $seen += $result.Exit
                $result.Exit | Should Not Be 70
                $result.Exit | Should Not Be 0
                $result.Exit | Should Be $badCase.Expected
                ([string]::IsNullOrEmpty($result.Err)) | Should Be $false
            }
            Add-Precondition -Key 'brokenInputs' -Value ([ordered]@{
                    count = $fixture.BadInputs.Count
                    codes = $seen
                })
            ($seen | Sort-Object -Unique).Count | Should BeGreaterThan 1
        }

        It 'репarse-петля: фикстура настоящая, а обход честно пропущен' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            if (-not $fixture.LoopOk) {
                Add-SkippedStep ('junction-петля не создалась в ' + $fixture.LoopPath +
                    ' (нужны права на создание reparse-точки; на машине разработчика создать junction можно, ' +
                    'на эталонной VM — обязательно)')
                return
            }
            $link = Get-Item -LiteralPath $fixture.LoopPath
            $link.LinkType | Should Not BeNullOrEmpty
            $link.Target | Should Not BeNullOrEmpty

            # Скан — единственное, что может наступить на петлю, и его код
            # возврата различает три разных исхода, а не два (как было раньше,
            # где ЛЮБОЙ ненулевой код уходил в пропуск и прогон G4 это поймал:
            # прогон в слоте a5 вернул -1073741819 = 0xC0000005, то есть упал с
            # нарушением доступа, а шаг отрапортовал это как «обход не подключён»).
            $scan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('scan', '--json', '--rules', $script:RulesPath, '--quiet')
            $scan.Exit | Should Not Be 70
            $reason = Get-FirstMeaningfulLine $scan.Err
            if ($scan.Exit -eq 0) {
                Write-Host ('скан отработал, петля пережита: ' + $fixture.LoopPath)
                return
            }
            # Пропуск законен только там, где обход действительно не отработал
            # и это сказано кодом из таблицы ScanExit (src/cli/cmd_scan.hpp):
            # 3 RulesUnavailable, 4 ScanFailed, 130 Interrupted. Всё остальное —
            # в том числе краш (отрицательный код вида 0xC0000005) и 70
            # (необработанное исключение) — §12 называет дефектом, и такой код
            # обязан валить шаг с настоящим кодом в сообщении, а не прятаться
            # за формулировкой «петлю обойти нечем».
            $honest = @(3, 4, 130)
            if ($honest -notcontains $scan.Exit) {
                throw ('scan упал на репarse-петле с кодом ' + $scan.Exit + ' (' + $reason +
                    '). Обход ФС обязан пережить петлю (FR-6, пропуск reparse points), ' +
                    'а §12 требует 0 крашей на сценариях 2-6: это дефект, не пропуск.')
            }
            Add-SkippedStep ('обход ФС не отработал — репarse-петлю обойти нечем (Win32 4390 не проверен). ' +
                'scan вернул код ' + $scan.Exit + ': ' + $reason)
        }
    }

    Context 'сценарий 3: зашифрованный том' {

        It 'EFS: файл действительно зашифрован, и ни один прогон его не тронул' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            $status = Invoke-ExternalTool -FilePath 'cipher.exe' -LogDirectory $script:SandboxRoot `
                -Tag 'cipher-state' -Arguments @('/c', $fixture.EncryptedFile)
            if (-not $fixture.EfsOk) {
                Add-SkippedStep ('том песочницы не поддерживает EFS: ' + $fixture.EfsReason +
                    ' — сценарий «зашифрованный том» на этой машине не проверяется')
                return
            }

            $candidatesPath = Join-Path $script:SandboxRoot 'efs-candidates.json'
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath `
                    -Candidates @(New-Candidate -Path $fixture.EncryptedDir -RuleId 'e2e.efs' `
                    -Bytes $FileSizeBytes -FileCount 1))

            $snapshot = New-FixtureSnapshot -Fixture $fixture
            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $candidatesPath)
            $apply = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--yes', '--json', '--candidates', $candidatesPath)

            # Зашифрованный файл — обычный кандидат для планировщика: он не
            # ломает план и не исчезает. Проверять тут нечего, кроме того, что
            # приложение не падает и не «чистит» то, что читает не владелец.
            $plan.Exit | Should Be 0
            $apply.Exit | Should Be 0
            (Get-RequiredProperty ($apply.Out | ConvertFrom-Json) 'executed' 'apply') | Should Be $false
            (Test-Path -LiteralPath $fixture.EncryptedFile -PathType Leaf) | Should Be $true
            (Get-Item -LiteralPath $fixture.EncryptedFile).Length | Should Be $FileSizeBytes
            Assert-FixtureUnchanged -Before $snapshot -Fixture $fixture -Label 'efs'

            # Отчёт по зашифрованному тому не должен выдавать наружу то, чего
            # не должен (§5 «Приватность»): маскирование серийников включено.
            $reportSource = Join-Path $script:SandboxRoot 'efs-report.json'
            Write-JsonFile -Path $reportSource -Document (New-CandidateDocument -Path $reportSource `
                    -Candidates @(New-Candidate -Path $fixture.EncryptedDir -RuleId 'e2e.efs' -Bytes $FileSizeBytes))
            $outputPath = Join-Path $script:SandboxRoot 'efs-out.json'
            $report = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $reportSource, '--out', $outputPath)
            $report.Exit | Should Be 0
            $document = Get-Content -LiteralPath $outputPath -Raw -Encoding UTF8 | ConvertFrom-Json
            (Get-RequiredProperty $document 'privacy.serialsMasked' 'report') | Should Be $true
            (Get-RequiredProperty $document 'privacy.volumeGuidsMasked' 'report') | Should Be $true
            $candidates = ConvertTo-CountArray (Get-RequiredProperty $document 'candidates' 'report')
            $candidates.Count | Should Be 1
            (Get-RequiredProperty $candidates[0] 'path' 'report.candidates[]') | Should Be $fixture.EncryptedDir
        }

        It 'BitLocker: состояние тома фиксируется в артефактах, а без прав админа — честный пропуск' {
            $tool = Invoke-ExternalTool -FilePath 'manage-bde.exe' -LogDirectory $script:SandboxRoot `
                -Tag 'bde' -Arguments @('-status')

            # Состояние шифрования тома — предусловие, а не результат: без его
            # записи отчёт о прогоне нельзя потом отличить «том не зашифрован»
            # от «проверять было нечем».
            if ($tool.Exit -eq 0) {
                Add-Precondition -Key 'bitlocker' -Value (Get-FirstMeaningfulLine $tool.Out)
                $tool.Out | Should Not BeNullOrEmpty
                return
            }
            Add-Precondition -Key 'bitlocker' -Value ('не прочитан: код ' + $tool.Exit + ' — ' +
                (Get-FirstMeaningfulLine $tool.All))
            Add-SkippedStep ('состояние BitLocker не читается без прав администратора: manage-bde -status ' +
                'вернул код ' + $tool.Exit + ' — ' + (Get-FirstMeaningfulLine $tool.All) +
                '. На эталонной VM (§12) прогоняйте с повышенными правами')
        }
    }

    Context 'сценарий 4: отсутствие прав' {

        It 'приложение стартует и работает без повышения прав' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            if (-not (Test-Path -LiteralPath $script:RulesPath -PathType Container)) {
                Add-SkippedStep ('набор правил не найден: ' + $script:RulesPath); return
            }
            $fixture = $script:Fixture

            # §12: «Приложение запускается без повышения прав». На повышенном
            # прогоне этот шач не имеет смысла, и это говорится прямо, а не
            # делается вид, что непривилегированная ветка проверена. Шаг
            # заканчивается здесь: в Pester 4 Set-ItResult -Skipped помечает
            # проверку пропущенной, но НЕ прерывает её, и четыре команды ниже
            # отработали бы вхолостую — отчёт показал бы пропуск при полном
            # списке выполненных проверок.
            if ($script:IsElevated) {
                Add-SkippedStep ('прогон идёт с повышенными правами (' + $script:UserName +
                    ') — сценарий «отсутствие прав» проверяет непривилегированную ветку, ' +
                    'запустите Pester из обычного сеанса')
                return
            }

            $candidatesPath = Join-Path $script:SandboxRoot 'rights-candidates.json'
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath `
                    -Candidates @(New-Candidate -Path $fixture.BusyDir -RuleId 'e2e.busy' -Bytes $FileSizeBytes))

            # Четыре команды, доступные без прав: правила, план, отчёт, версия.
            $rules = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('rules', 'validate', $script:RulesPath)
            $rules.Exit | Should Be 0

            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $candidatesPath)
            $plan.Exit | Should Be 0

            $report = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $candidatesPath, '--out',
                (Join-Path $script:SandboxRoot 'rights-out.json'))
            $report.Exit | Should Be 0

            $version = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot -Arguments @('--version')
            $version.Exit | Should Be 0
            $version.Out | Should Not BeNullOrEmpty
        }

        It 'disks без прав: устройство помечено недоступным, а не выдумано (FR-1)' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $disks = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot -Arguments @('disks', '--json')

            # 3 = InventoryUnavailable, 4 = Degraded (src/cli/cmd_disks.hpp).
            # Оба — честные коды; 5 был бы необработанным отказом, а 0 при
            # неполной карте — враньём, которого §12 не допускает.
            $disks.Exit | Should Not Be 70
            $disks.Exit | Should Not Be 5
            ($disks.Exit -eq 0 -or $disks.Exit -eq 3 -or $disks.Exit -eq 4) | Should Be $true
            $disks.Out.Trim().StartsWith('{') | Should Be $true

            $document = $disks.Out | ConvertFrom-Json
            $inventory = Get-RequiredProperty $document 'inventory' 'disks'
            if ($disks.Exit -eq 3) {
                # Ни одного диска с данными: карта обязана это сказать, а не
                # показать нули как будто это правда. Замечаний может быть
                # несколько, поэтому раздел consistency — список, и нужный код
                # ищется по нему, а не берётся первым элементом.
                $consistency = ConvertTo-CountArray (Get-RequiredProperty $document 'consistency' 'disks')
                $consistency.Count | Should BeGreaterThan 0
                $codes = @()
                foreach ($item in $consistency) { $codes += [string] (Get-RequiredProperty $item 'code' 'disks.consistency[]') }
                ($codes -join ' ') | Should Match 'disk.no_data'
                (Get-RequiredProperty $inventory 'degraded' 'disks.inventory') | Should Be $true
                $issues = ConvertTo-CountArray (Get-RequiredProperty $inventory 'issues' 'disks.inventory')
                $issues.Count | Should BeGreaterThan 0
                # Каждая заметка о недоступности несёт код Win32 — §12 требует
                # HRESULT в логе, а не «что-то с диском».
                $withCode = 0
                foreach ($issue in $issues) {
                    if ([string] $issue -match 'win32=0x[0-9A-Fa-f]{8}') { $withCode++ }
                }
                $withCode | Should BeGreaterThan 0
                $disks.Err | Should Match 'win32=0x[0-9A-Fa-f]{8}'
                # И по диску: нет данных — значит, нули, а не правдоподобные числа.
                foreach ($disk in (ConvertTo-CountArray (Get-RequiredProperty $document 'disks' 'disks'))) {
                    if (-not (Get-RequiredProperty $disk 'hasDiskData' 'disks.disks[]')) {
                        (Get-RequiredProperty $disk 'sizeBytes' 'disks.disks[]') | Should Be 0
                    }
                }
                Add-Precondition -Key 'disksWithoutRights' -Value ('код ' + $disks.Exit + ', недоступно устройств: ' +
                    (Get-RequiredProperty $inventory 'unavailableDevices' 'disks.inventory'))
            } else {
                Add-Precondition -Key 'disksWithoutRights' -Value ('код ' + $disks.Exit + ' — карта получена')
            }
        }

        It 'deny-каталог: план строится по дампу, а отказ по правам не выглядит успехом' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            # Фикстура запрета проверяется отдельно: без неё шаг ничего не
            # доказывал бы о правах.
            (Test-AccessDenied -Path (Join-Path $fixture.DeniedDir 'probe.tmp') -Mode 'write') | Should Be $true
            (Test-AccessDenied -Path $fixture.DeniedFile -Mode 'read') | Should Be $true
            $aclBefore = Invoke-ExternalTool -FilePath 'icacls.exe' -LogDirectory $script:SandboxRoot `
                -Tag 'icacls-before' -Arguments @($fixture.DeniedDir)
            $treeBefore = Get-TreeFiles $fixture.Root

            $candidatesPath = Join-Path $script:SandboxRoot 'denied-candidates.json'
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath `
                    -Candidates @(New-Candidate -Path $fixture.DeniedDir -RuleId 'e2e.denied' `
                    -Bytes $FileSizeBytes -FileCount 1))

            # Планирование идёт по дампу скана и прав на ФС не требует.
            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $candidatesPath)
            $plan.Exit | Should Be 0

            # А вот попытка что-то выполнить обязана честно отказать: 4 = нет
            # источника кандидатов, 5 = исполнитель не подключён. Оба означают
            # «не удалено», и оба не должны выглядеть как очистка.
            $apply = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--execute', '--yes', '--json', '--no-trash')
            ($apply.Exit -eq 4 -or $apply.Exit -eq 5) | Should Be $true
            $apply.Err | Should Not BeNullOrEmpty
            if ($apply.Exit -eq 4) {
                $apply.Err | Should Match ([regex]::Escape('--candidates'))
            }
            $applyDocument = $apply.Out | ConvertFrom-Json
            (Get-RequiredProperty $applyDocument 'executed' 'apply') | Should Be $false

            # Отчёт, который лежит под запретом, не читается: 66 и путь в stderr.
            $read = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('report', '--json', '--in', $fixture.DeniedFile,
                '--out', (Join-Path $script:SandboxRoot 'denied-out.json'))
            $read.Exit | Should Be 66
            $read.Err | Should Match ([regex]::Escape($fixture.DeniedFile))

            # Ничего не пропало и права не «починились» сами собой: приложение
            # не имеет права менять ACL пользователя, чтобы обойти отказ. Снимок
            # дерева до и после — проверка на «ничего не удалилось и ничего не
            # создалось», а не только на путь одного файла.
            $treeAfter = Get-TreeFiles $fixture.Root
            (Test-Path -LiteralPath $fixture.DeniedFile -PathType Leaf) | Should Be $true
            (Get-Item -LiteralPath $fixture.DeniedFile).Length | Should Be $FileSizeBytes
            $treeAfter.Count | Should Be $treeBefore.Count
            $treeAfter.Bytes | Should Be $treeBefore.Bytes
            $aclAfter = Invoke-ExternalTool -FilePath 'icacls.exe' -LogDirectory $script:SandboxRoot `
                -Tag 'icacls-after' -Arguments @($fixture.DeniedDir)
            ($aclAfter.Out -replace '\s+', ' ') | Should Be ($aclBefore.Out -replace '\s+', ' ')
        }

        It 'файл кандидатов под запретом: команда не читает его в обход ACL' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $fixture = $script:Fixture

            $candidatesPath = Join-Path $script:SandboxRoot 'locked-read-candidates.json'
            Write-JsonFile -Path $candidatesPath -Document (New-CandidateDocument -Path $candidatesPath `
                    -Candidates @(New-Candidate -Path $fixture.BusyDir -RuleId 'e2e.busy' -Bytes $FileSizeBytes))
            Set-DenyAce -Path $candidatesPath -Rights 'R' | Out-Null
            (Test-AccessDenied -Path $candidatesPath -Mode 'read') | Should Be $true

            $snapshot = New-FixtureSnapshot -Fixture $fixture
            $plan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('plan', '--json', '--candidates', $candidatesPath)

            # 4 = NoCandidates с текстом «не открылся файл со списком
            # кандидатов». Молчаливый пустой план здесь был бы опаснее отказа:
            # по нему удалять нечего, но человек увидел бы «мусора нет».
            $plan.Exit | Should Be 4
            ([string]::IsNullOrEmpty($plan.Out)) | Should Be $true
            $plan.Err | Should Match ([regex]::Escape($candidatesPath))
            (Test-Path -LiteralPath $fixture.BusyFile -PathType Leaf) | Should Be $true
            Assert-FixtureUnchanged -Before $snapshot -Fixture $fixture -Label 'deniedCandidatesFile'
        }
    }

    Context 'сводка' {

        It 'каждый пропуск записан с причиной' {
            $lines = @()
            if (Test-Path -LiteralPath $script:SkipLog) {
                $lines = @(Get-Content -LiteralPath $script:SkipLog -Encoding UTF8 |
                        Where-Object { $_.Trim() -ne '' })
            }
            foreach ($line in $lines) { $line | Should Match '^SKIPPED: .{10,}$' }
            if ($lines.Count -eq 0) {
                Write-Host 'пропусков нет: все четыре сценария выполнены на этой машине'
            } else {
                Write-Host ('пропущено шагов: ' + $lines.Count + ' (см. ' + $script:SkipLog + ')')
                foreach ($line in $lines) { Write-Host ('  ' + $line) }
            }
            # Снимок предусловий — часть отчёта о прогоне: без него «зелёный»
            # результат на машине разработчика нельзя отличить от результата на
            # эталонной VM (§11 п. 3).
            (Test-Path -LiteralPath $script:PreconditionsPath -PathType Leaf) | Should Be $true
        }
    }
}
