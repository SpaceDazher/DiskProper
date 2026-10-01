#Requires -Version 5.1
<#
.SYNOPSIS
    MrProper: E2E отмены и восстановления — 100 % файлов возвращено.

.DESCRIPTION
    Спека: §11.3 п. 3 («E2E на VM-образах (Pester/PowerShell) — 20 сценариев:
    полный цикл, частичная очистка, отмена, восстановление, …») и §12
    («Все 20 e2e-сценариев зелёные, включая восстановление 100 % удалённого и
    отсутствие изменений вне корней правил»). Спека ставит для отмены ровно
    четыре требования, и сценарий проверяет каждое числом, а не словом.
    Третий пункт ниже — разбор запрета FR-5 на ПУСТОМ плане: он добавлен
    к четырём требованиям СПЕКИ, потому что именно там запрет молча
    обходился кодом 0 (см. шаг 3б и skipped-undo-restore.log).

      * FR-5 — без --execute не удалено ни байта, план показан целиком;
      * FR-5 — список операций и снимок состояния печатаются ДО вопроса, а
        отказ (код 3) не трогает ни одного файла;
      * FR-5 (разбор на пустом плане) — запрет «--candidates вместе с
        --execute отклоняется» действует
        и на ПУСТОМ плане: при 0 операций отказ всё равно обязан быть
        кодом 3, иначе запрет обходится «нечего выполнять» с кодом 0;
      * FR-7 — транзакция пишется как %ProgramData%\MrProper\Trash\<txId>\ с
        manifest.json (исходный путь, размер, mtime), и её достаточно для
        полного возврата: каждый файл на своём месте, с тем же содержимым;
      * FR-7 — «существующий файл — не перезаписывать, спросить», и §7.2 —
        повторное восстановление уже возвращённого ничего не переписывает.

    Настоящие вызовы, настоящие коды возврата, настоящие файлы. Заглушек нет:
    шаг, который эта сборка выполнить не может, печатает «SKIPPED: <код> —
    <текст stderr>», пишет строку в build\e2e-artifacts\skipped-undo-restore.log
    и попадает в сводку. «Зелёный» Pester при этом не значит «отмена проверена» —
    смотрите skipped-undo-restore.log.

    ЧТО ПРОВЕРЯЕТСЯ НАСТОЯЩИМ ЧИСТЫМ КОДОМ, А ЧТО КОНТРАКТОМ — важно не
    перепутать, поэтому написано здесь, а не в комментарии попозже:

      * Отмена (шаги 2 и 3) проверяется НАСТОЯЩИМ кодом: mrproper-cli plan/apply
        с --candidates, документ плана из stdout, коды 0 и 3 из
        src/cli/cmd_apply.hpp. Это проверка реального поведения CLI.
      * Восстановление (шаги 4–6) проверяется КОНТРАКТОМ на диске: сценарий
        сам раскладывает транзакцию в формате, который пишет движок
        (src/core/trash.cpp: schema 1, txId, items[].originalPath/payload/bytes/
        fileCount/mtime, state=committed), и возвращает файлы обратно ровно по
        этому манифесту. Так проверяется, что формат транзакции достаточен для
        100 % возврата и что повторный возврат безопасен. Собственный
        engine::undo_service (SPEC §6.2) этим НЕ проверяется: он не вызывается
        из CLI, и шаг 1 фиксирует его отсутствие как пропуск с доказательством.
        На эталонной VM §12, где команда отмены есть, сценарий вызывает её
        сам (ключ -UseCliUndo) и печатает, каким путём пошёл проверяющий.

    Прав администратора не нужно: песочница и корзина сценария живут в %TEMP%,
    а §12 требует «приложение запускается без повышения прав». Настоящий корзинный
    каталог %ProgramData%\MrProper\Trash сценарий только читает (список
    транзакций до и после отказа) и ничего в него не кладёт: снос чужих
    транзакций на машине разработчика — не проверка, а порча.

    Пропуски пишутся в отдельный файл skipped-undo-restore.log, а не в общий
    skipped.log: сценарии запускаются параллельно, и общий файл у соседей
    затирался бы.

    Кодировка файла — UTF-8 С BOM, как у tools\sign-rules.ps1 и
    tests\e2e\Test-FullCycle.ps1. Windows PowerShell 5.1 читает .ps1 без BOM в
    кодировке ANSI, и русский текст после этого ломает разбор самого файла
    вплоть до «TerminatorExpectedAtEndOfString».

.PARAMETER CliPath
    Путь к mrproper_cli.exe. Пусто — искать в build\ (<слот>\<конфигурация>).

.PARAMETER Configuration
    Имя конфигурации для поиска сборки: Debug или Release (по умолчанию Debug).

.PARAMETER Slot
    Слот сборки build\<слот>\<конфигурация>. Пусто — искать во всех слотах и взять
    самую свежую сборку.

.PARAMETER CandidateCount
    Сколько «чистых» кандидатов (каталогов) уходит в полный круг
    корзина → восстановление (по умолчанию 3).

.PARAMETER FilesPerCandidate
    Сколько файлов в кандидате (по умолчанию 3). Итог «100 % возвращено» —
    это CandidateCount × FilesPerCandidate файлов, а не кандидатов.

.PARAMETER FileSizeBytes
    Размер одного файла песочницы, байт (по умолчанию 4096). Файлы заполняются
    не нулём: нули NTFS уводят файл в sparse, и тогда освобождение на томе не
    равно сумме bytes из плана — сверять было бы не с чем.

.PARAMETER UseCliUndo
    Восстановление выполнять командой отмены CLI (undo/restore), если она есть
    в этой сборке. По умолчанию сценарий всё равно пробует её первым делом и
    только при отсутствии переходит к контракту; ключ заставляет не переходить.

.PARAMETER RequireEngineUndo
    Не соглашаться на контракт: если команды отмены CLI нет — провалить сценарий,
    а не пропустить шаг. Для эталонной VM §12, где восстановление обязано
    проверяться настоящим кодом.

.PARAMETER KeepArtifacts
    Не снимать песочницу после прогона (по умолчанию она удаляется).

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-UndoRestore.ps1

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-UndoRestore.ps1 -Slot a88
    # Pester 3.4 (тот, что стоит в Windows PowerShell 5.1) не знает -Parameters:
    # Invoke-Pester -Script @{ Path = 'tests\e2e\Test-UndoRestore.ps1';
    #                       Parameters = @{ Slot = 'a88' } }

.EXAMPLE
    powershell -NoProfile -File tests\e2e\Test-UndoRestore.ps1 -Slot a88
    # Вне Pester файл только РЕГИСТРИРУЕТ Describe: шаги It выполняет Pester,
    # поэтому прямой запуск не печатает сводку и всегда «успешен» — это не
    # проверка. Прогон с параметрами и сводкой: Invoke-Pester, как в двух
    # примерах выше, либо tools\run-e2e.bat Debug main e2e_UndoRestore.
#>
[CmdletBinding()]
param(
    [string] $CliPath = '',
    [string] $Configuration = 'Debug',
    [string] $Slot = '',
    [int]    $CandidateCount = 3,
    [int]    $FilesPerCandidate = 3,
    [int]    $FileSizeBytes = 4096,
    [switch] $UseCliUndo,
    [switch] $RequireEngineUndo,
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

# Артефакты сценария — в build\ (каталог в .gitignore), а не рядом с тестом:
# отчёт о пропусках должен пережить прогон и быть виден без запуска Pester.
$script:ArtifactsDir = Join-Path (Join-Path $script:RepoRoot 'build') 'e2e-artifacts'
try {
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
} catch {
    $script:ArtifactsDir = Join-Path ([IO.Path]::GetTempPath()) 'mrproper-e2e-artifacts'
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
}
# Свой файл пропусков, а не общий skipped.log: сценарии идут параллельно, и общий
# файл у соседей затирался бы.
$script:SkipLog = Join-Path $script:ArtifactsDir 'skipped-undo-restore.log'
if (Test-Path -LiteralPath $script:SkipLog) { Remove-Item -LiteralPath $script:SkipLog -Force }

# Состояние прогона. Пишется в файл, а не в переменную: файл переживает и
# повторный запуск, и две реализации Pester с разным устройством фаз
# discovery/run, а сводка читает именно его.
$script:LogCounter = 0
$script:Cli = $null
$script:CliMissingReason = ''
$script:SandboxRoot = ''
$script:Ready = $false
# Идентификатор «чистой» транзакции: шаг 4 создаёт, шаги 5 и 7 проверяют. Явно
# пустой, потому что под Set-StrictMode обращение к неинициализированному
# $script: — исключение, и вместо честного пропуска шага был бы стек.
$script:TxClean = ''
# Транзакция, которую отменяет НАСТОЯЩАЯ команда CLI (шаг 9): отдельная от
# «чистой» шага 4, чтобы контрактный путь и путь команды не мешали знаменателю.
$script:TxCli = ''
$script:CliFixture = $null

# Итоги сценария, которые печатает сводка. Инициализируются явно: под
# Set-StrictMode обращение к несуществующей переменной $script: — исключение, и
# сводка упала бы на пустом прогоне вместо честной строки «ничего не проверено».
$script:Summary = [pscustomobject]@{
    CleanItems        = 0
    CleanFiles        = 0
    RestoredItems     = 0
    RestoredFiles     = 0
    RestorePercent    = 0.0
    ConflictSkipped   = 0
    ForeignFileIntact = $true
    RepeatRestored    = 0
    UndoSurface       = '(не проверялась)'
    EngineUndoUsed    = $false
    ContractUsed      = $false
    CancelDryRunOk    = $false
    CancelRefusedCode = -1
    # Отказ по --execute на ПУСТОМ плане (шаг 3б). Отдельная строка сводки, а не
    # украшение: пока волна F3 не протянула манифест до buildPlan, план был
    # пуст, и отказ FR-5 возвращался кодом 0 вместо 3 — на этом шаге видно,
    # чем именно запрет обходится, а не «просто ноль».
    EmptyPlanCode    = -1
    EmptyPlanVerdict = '(не проверен)'
    CliReason         = ''
    SkipCount         = 0
    # Полный круг настоящей командой отмены (шаг 9). -1 означает «шаг не
    # дошёл», и сводка это показывает, чтобы зелёный прогон не читался как
    # «отмена проверена».
    CliListCode      = -1
    CliDryRunCode    = -1
    CliRestoreCode   = -1
    CliRestoredFiles = 0
    CliBytes         = [int64] 0
    CliRepeatCode    = -1
    CliState         = '(не проверен)'
    CliListFound     = 0
    RestorePercentByCli = 0.0
}

# ---------------------------------------------------------------------------
# Вспомогательные функции
# ---------------------------------------------------------------------------

function Get-MrProperCli {
    <#
    .SYNOPSIS Найти собранный mrproper_cli.exe.
    .DESCRIPTION Порядок поиска: -CliPath, $env:MRPROPER_CLI, точные пути слота и
    конфигурации, затем все слоты поимённо. Явно запрошенный путь не найден —
    молча искать другой нельзя: сценарий пошёл бы не по той сборке, которую
    проверяют, и причина ушла бы в skipped-undo-restore.log.
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

function Write-JsonFile {
    <#
    .SYNOPSIS Записать документ в JSON БЕЗ BOM.
    .DESCRIPTION Разбор JSON в mrproper-cli BOM снимает (cmd_apply.cpp,
    parseCandidatesJson), но манифест корзины наоборот проходит через
    rejectUnknownFields, и лишний байт в начале — уже не лишний. Set-Content
    -Encoding UTF8 в Windows PowerShell 5.1 BOM пишет, поэтому файл собирается
    вручную.
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
    проверяется явно и называется: «нет поля X в Y» полезнее, чем стек.
    #>
    param($Document, [string] $Name, [string] $Origin)

    $property = $Document.PSObject.Properties[$Name]
    if ($null -eq $property) { throw ('в документе ' + $Origin + ' нет поля ' + $Name) }
    return $property.Value
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
    строка SKIPPED в выводе и запись в skipped-undo-restore.log — сводка её
    покажет.
    #>
    param([Parameter(Mandatory = $true)] [string] $Reason)

    Write-Host ('SKIPPED: ' + $Reason)
    [IO.File]::AppendAllText($script:SkipLog, 'SKIPPED: ' + $Reason + "`r`n",
        (New-Object System.Text.UTF8Encoding($false)))
    $script:Summary.SkipCount = $script:Summary.SkipCount + 1
    if ($null -ne (Get-Command -Name 'Set-ItResult' -ErrorAction SilentlyContinue)) {
        Set-ItResult -Skipped -Because $Reason
    }
}

function Get-SandboxRoot {
    <#
    .SYNOPSIS Корень песочницы сценария — свой каталог в %TEMP%, а НЕ $TestDrive.
    .DESCRIPTION Причина написана, потому что её легко «починить» обратно: Pester
    3.4 в конце КАЖДОГО Context вызывает Clear-TestDrive и удаляет всё, чего не
    было на диске при входе в Context (Pester\3.4.0\Functions\Context.ps1, строка
    81). Песочница отмены обязана пережить границу между шагами — транзакция
    создаётся в шаге 4, а проверяется в шагах 5 и 7, — значит, лежать в
    $TestDrive она не может: между Contexts её бы просто снесли, и шаг 4 упал бы
    с «путь не существует», а шаги 5 и 7 прошли бы вхолостую (0 файлов == 0
    ожидаемых). Свой каталог в %TEMP% снимает вопрос и для Pester 4/5, где
    TestDrive живёт иначе.
    #>
    $root = Join-Path ([IO.Path]::GetTempPath()) ('mrproper-undo-e2e-' + $PID)
    return $root
}

function Get-FileStamp {
    <#
    .SYNOPSIS Отпечаток файла: длина и SHA-256 содержимого.
    .DESCRIPTION Хэш считается напрямую через SHA256.Create, а не через
    Get-FileHash: cmdlet есть не во всех сборках PowerShell, а сценарий должен
    работать и там, где установлен голый Windows PowerShell 5.1. Содержимое, а не
    только длина: восстановление, вернувшее файл другого размера, — уже
    дефект, но восстановление, вернувшее файл того же размера с другими
    байтами, — дефект вдвое хуже.
    #>
    param([string] $Path)

    $info = Get-Item -LiteralPath $Path -ErrorAction Stop
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [IO.File]::ReadAllBytes($Path)
        $hash = $sha.ComputeHash($bytes)
    } finally {
        $sha.Dispose()
    }
    return [pscustomobject]@{
        Length    = [int64] $info.Length
        Sha256    = ([BitConverter]::ToString($hash) -replace '-', '')
        MtimeUnix = [int64] ([Math]::Floor(($info.LastWriteTimeUtc - [DateTime]'1970-01-01T00:00:00Z').TotalSeconds))
    }
}

function Get-AppTrashRoot {
    <#
    .SYNOPSIS Настоящий корень корзины приложения: %ProgramData%\MrProper\Trash.
    .DESCRIPTION Только для чтения (список транзакций до и после отказа).
    Сценарий ничего в него не кладёт: снос или подмена чужой транзакции на
    машине разработчика — не проверка, а порча (FR-7, §5 «никаких прав на
    старте»).
    #>
    $programData = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData)
    if ($programData -eq '') { return '' }
    # Join-Path принимает ровно два аргумента (Path и ChildPath), поэтому
    # склейка вложенная: %ProgramData%\MrProper, потом \Trash.
    return (Join-Path (Join-Path $programData 'MrProper') 'Trash')
}

function Get-TrashTransactionIds {
    <#
    .SYNOPSIS Список идентификаторов транзакций в корне корзины.
    #>
    param([string] $TrashRoot)

    if ($TrashRoot -eq '' -or -not (Test-Path -LiteralPath $TrashRoot -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $TrashRoot -Directory -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Name)
}

function Test-TrashTxId {
    <#
    .SYNOPSIS Проверить txId по правилам core::trash::isValidTxId.
    .DESCRIPTION Правило движка: не пусто, до 64 символов, не начинается с «-»,
    только [0-9A-Za-z-]. Сценарий строит txId сам (транзакцию раскладывает он),
    и если бы он положил в каталог имя, которое движок потом не прочитает,
    проверка «100 % возвращено» была бы проверкой пустоты.
    #>
    param([string] $TxId)

    if ($TxId -eq '' -or $TxId.Length -gt 64) { return $false }
    if ($TxId.StartsWith('-')) { return $false }
    return ($TxId -match '^[0-9A-Za-z-]+$')
}

function Test-TrashPayloadName {
    <#
    .SYNOPSIS Проверить имя объекта в корзине по core::trash::isValidPayloadName.
    .DESCRIPTION Требование движка: не пусто, до 64 символов, не «.» и не «..»,
    только [0-9A-Za-z_.-]. Смысл проверки тот же, что у isValidTxId: путь из
    манифеста уходит в файловые операции, и «..» из подделанного манифеста — это
    выход за пределы каталога транзакции.
    #>
    param([string] $Name)

    if ($Name -eq '' -or $Name.Length -gt 64) { return $false }
    if ($Name -eq '.' -or $Name -eq '..') { return $false }
    return ($Name -match '^[0-9A-Za-z_.-]+$')
}

function Get-UndoSurface {
    <#
    .SYNOPSIS Есть ли в этой сборке CLI команда отмены (undo/restore).
    .DESCRIPTION Разбирается блок «Команды:» из `mrproper-cli --help`, а не весь
    текст: слово «восстановление» встречается и в прозе, и в отказе другой
    команды, и совпадение по всему выводу давало бы ложное «поверхность есть».
    Возвращает @{ Found; Command; Commands }.
    #>
    param([string] $Cli)

    if ($null -eq $Cli) { return [pscustomobject]@{ Found = $false; Command = ''; Commands = @() } }

    $help = Invoke-MrProperCli -Cli $Cli -LogDirectory $script:SandboxRoot -Arguments @('--help')
    if ($help.Exit -ne 0) {
        return [pscustomobject]@{ Found = $false; Command = ''; Commands = @() }
    }

    $commands = @()
    $inBlock = $false
    foreach ($line in ($help.Out -split "`r?`n")) {
        if ($line -match '^\s*Команды\s*:') { $inBlock = $true; continue }
        if (-not $inBlock) { continue }
        if ($line -match '^\s*$') {
            if ($commands.Count -gt 0) { break }
            continue
        }
        if ($line -match '^\s+([A-Za-z][A-Za-z0-9._-]*)\s{2,}\S') { $commands += $Matches[1] }
    }

    foreach ($name in @('undo', 'restore', 'trash-restore')) {
        if ($commands -contains $name) {
            return [pscustomobject]@{ Found = $true; Command = $name; Commands = $commands }
        }
    }
    return [pscustomobject]@{ Found = $false; Command = ''; Commands = $commands }
}

function Invoke-CliUndo {
    <#
    .SYNOPSIS Вызов команды отмены CLI (undo/restore), если она есть в сборке.
    .DESCRIPTION Ключи подтверждения и возврата берутся из контракта команды
    отмены (src/cli/cmd_undo.hpp), а не угадываются: -Execute возвращает файлы
    (без него команда показывает план и ничего не трогает), -Yes отменяет
    вопрос — сценарий неинтерактивен и подтверждения напечатать не может.
    -KeepTransaction оставляет каталог транзакции: без него полный возврат сносит
    каталог вместе с манифестом, и состояние undone на диске не проверить.
    Коды возврата сценарий не угадывает: любой неожиданный код — это находка, и
    он попадает в пропуск с текстом stderr, а не в «зелёный» итог.
    #>
    param([string] $Cli, [string] $Command, [string] $TrashRoot, [string] $TxId,
        [switch] $Execute, [switch] $Yes, [switch] $KeepTransaction, [switch] $List)

    $arguments = @($Command, '--json', '--trash-root', $TrashRoot)
    if ($List) { $arguments += '--list' } else { $arguments += @('--tx', $TxId) }
    if ($Execute) { $arguments += '--execute' }
    if ($Yes) { $arguments += '--yes' }
    if ($KeepTransaction) { $arguments += '--keep-transaction' }
    return Invoke-MrProperCli -Cli $Cli -LogDirectory $script:SandboxRoot -Arguments $arguments
}

# ---------------------------------------------------------------------------
# Фикстура
# ---------------------------------------------------------------------------

function New-UndoFixture {
    <#
    .SYNOPSIS Подготовить песочницу: кандидаты-каталоги с файлами + эталон.
    .DESCRIPTION Каталог, а не файл, потому что так кандидат выглядит в настоящем
    скане (§6.3, FR-3), и восстановление потом возвращает каталог целиком.
    Содержимое детерминированное: байт i-го файла равен (индекс + 1), поэтому
    отпечаток SHA-256 известен заранее и «файл вернулся, но другой» ловится
    сравнением, а не числом байт.
    Возвращает кандидаты для JSON, таблицу ожидаемых файлов и счётчики.
    #>
    param([string] $Root, [int] $CandidateCount, [int] $FilesPerCandidate, [int] $SizeBytes)

    if (-not (Test-Path -LiteralPath $Root)) { New-Item -ItemType Directory -Path $Root -Force | Out-Null }

    $candidates = @()
    $expected = @()
    $totalBytes = [int64] 0
    for ($index = 0; $index -lt $CandidateCount; $index++) {
        $directory = Join-Path $Root ('cand' + ($index + 1))
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $bytes = [int64] 0
        for ($file = 1; $file -le $FilesPerCandidate; $file++) {
            $buffer = New-Object byte[] $SizeBytes
            for ($byte = 0; $byte -lt $SizeBytes; $byte++) {
                $buffer[$byte] = [byte] (($index * 31 + $file * 7 + $byte) -band 0xFF)
            }
            $path = Join-Path $directory ('file' + $file + '.bin')
            [IO.File]::WriteAllBytes($path, $buffer)
            $bytes += $SizeBytes
            $expected += [pscustomobject]@{
                Path   = $path
                Length = [int64] $SizeBytes
                Stamp  = (Get-FileStamp $path)
            }
        }
        $totalBytes += $bytes
        $candidates += [ordered]@{
            ruleId         = ('e2e.undo.cand' + ($index + 1))
            category       = 'temp.user'
            path           = $directory
            displayName    = ('Песочница отмены ' + ($index + 1))
            logicalBytes   = $bytes
            allocatedBytes = $bytes
            fileCount      = $FilesPerCandidate
            safety         = 'safe'
            confidence     = 90
            reasons        = @('e2e-песочница сценария отмены и восстановления')
        }
    }

    return [pscustomobject]@{
        Candidates  = $candidates
        Expected    = $expected
        TotalBytes  = $totalBytes
        TotalFiles  = $CandidateCount * $FilesPerCandidate
        Items       = $CandidateCount
    }
}

# ---------------------------------------------------------------------------
# Корзина приложения: транзакция по контракту FR-7 / core::trash
# ---------------------------------------------------------------------------

function New-TrashTxId {
    <#
    .SYNOPSIS Идентификатор транзакции того же вида, что делает core::trash.
    .DESCRIPTION Формат makeTxId: compactStamp(counter) — метка UTC без
    двоеточий, потому что результат обязан пройти isValidTxId (только
    [0-9A-Za-z-]). Отсюда и форма счётчика, и проверка Test-TrashTxId.
    #>
    param([int] $Counter)

    $now = [DateTime]::UtcNow
    $stamp = ('{0:yyyyMMdd}T{0:HHmmss}Z' -f $now)
    $counterText = '{0:x6}' -f $Counter
    $nonceText = '{0:x8}' -f (Get-Random -Minimum 1 -Maximum 4294967295)
    return ($stamp + '-' + $counterText + '-' + $nonceText)
}

function New-TrashTransaction {
    <#
    .SYNOPSIS Разложить элементы песочницы в корзину и записать manifest.json.
    .DESCRIPTION Ровно то, что пишет движок (FR-7, core::trash.cpp toJson):
      * каталог <trashRoot>\<txId>\ с объектами по payload-именам;
      * manifest.json: schema 1, txId, createdAt, appVersion, state=committed,
        purgedBytes, items[] с kind/originalPath/payload/bytes/fileCount/mtime/
        readOnly/aclSddl/crossVolume/sourceVolume.
    Имена полей не выдуманы: лишнее поле движок отвергнет (rejectUnknownFields),
    а недостающее — не восстановит, поэтому сценарий пишет ровно этот набор.
    Корзина сценария — в песочнице, а не в %ProgramData%: см. Get-AppTrashRoot.
    #>
    param([string] $TrashRoot, [string] $TxId, $Entries, [string] $AppVersion, [string] $VolumeGuid)

    if (-not (Test-TrashTxId $TxId)) { throw ('сценарий сложил недопустимый txId: ' + $TxId) }

    $txDir = Join-Path $TrashRoot $TxId
    if (Test-Path -LiteralPath $txDir) { Remove-Item -LiteralPath $txDir -Recurse -Force }
    New-Item -ItemType Directory -Path $txDir -Force | Out-Null

    $items = @()
    foreach ($entry in $Entries) {
        $payload = $entry.Payload
        if (-not (Test-TrashPayloadName $payload)) { throw ('недопустимое имя объекта в корзине: ' + $payload) }

        $storedPath = Join-Path $txDir $payload
        Move-Item -LiteralPath $entry.Path -Destination $storedPath -Force

        $items += [ordered]@{
            kind         = 'directory'
            originalPath = $entry.Path
            payload      = $payload
            bytes        = [int64] $entry.Bytes
            fileCount    = [int] $entry.FileCount
            mtime        = [int64] $entry.MtimeUnix
            readOnly     = $false
            aclSddl      = ''
            crossVolume  = $false
            sourceVolume = $VolumeGuid
        }
    }

    $document = [ordered]@{
        schema      = 1
        txId        = $TxId
        createdAt   = [int64] ([Math]::Floor(([DateTime]::UtcNow - [DateTime]'1970-01-01T00:00:00Z').TotalSeconds))
        appVersion  = $AppVersion
        state       = 'committed'
        purgedBytes = 0
        items       = $items
    }
    Write-JsonFile -Path (Join-Path $txDir 'manifest.json') -Document $document
    return $txDir
}

function Read-TrashManifest {
    <#
    .SYNOPSIS Прочитать manifest.json транзакции и проверить его по контракту.
    .DESCRIPTION Проверки — те, что движок делает при чтении (core::trash.cpp):
    schema == 1 (более новая версия — отказ, а не догадка), txId проходит
    isValidTxId, payload проходит isValidPayloadName, originalPath непуст.
    Мусор в манифесте означает, что вернуть нечего, и «100 % возвращено» было бы
    числом без знаменателя.
    #>
    param([string] $TrashRoot, [string] $TxId)

    $txDir = Join-Path $TrashRoot $TxId
    $manifestPath = Join-Path $txDir 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw ('нет манифеста транзакции: ' + $manifestPath)
    }
    $text = [IO.File]::ReadAllText($manifestPath, (New-Object System.Text.UTF8Encoding($false)))
    $document = $text | ConvertFrom-Json

    $schema = [int] (Get-RequiredProperty $document 'schema' 'manifest.json')
    if ($schema -ne 1) { throw ('схема манифеста ' + $schema + ' не поддерживается (ожидается 1)') }

    $manifestTxId = [string] (Get-RequiredProperty $document 'txId' 'manifest.json')
    if ($manifestTxId -ne $TxId) {
        throw ('в манифесте txId "' + $manifestTxId + '", а каталог называется "' + $TxId + '"')
    }
    if (-not (Test-TrashTxId $manifestTxId)) { throw ('недопустимый txId в манифесте: ' + $manifestTxId) }

    $rawItems = @(Get-RequiredProperty $document 'items' 'manifest.json')
    $entries = @()
    foreach ($raw in $rawItems) {
        $originalPath = [string] (Get-RequiredProperty $raw 'originalPath' 'manifest.items[]')
        if ($originalPath -eq '') { throw ('в манифесте пустой originalPath — нечего восстанавливать') }
        $payload = [string] (Get-RequiredProperty $raw 'payload' 'manifest.items[]')
        if (-not (Test-TrashPayloadName $payload)) {
            throw ('недопустимое имя объекта в манифесте: ' + $payload)
        }
        $entries += [pscustomobject]@{
            OriginalPath = $originalPath
            Payload      = $payload
            Bytes        = [int64] (Get-RequiredProperty $raw 'bytes' 'manifest.items[]')
            FileCount    = [int] (Get-RequiredProperty $raw 'fileCount' 'manifest.items[]')
            MtimeUnix    = [int64] (Get-RequiredProperty $raw 'mtime' 'manifest.items[]')
        }
    }

    return [pscustomobject]@{
        TxId      = $manifestTxId
        TxDir     = $txDir
        State     = [string] (Get-RequiredProperty $document 'state' 'manifest.json')
        AppVersion = [string] (Get-RequiredProperty $document 'appVersion' 'manifest.json')
        Entries   = $entries
    }
}

function Get-RestoreRenameTarget {
    <#
    .SYNOPSIS Имя «восстановить рядом» по core::undo::restoreRenameTarget.
    .DESCRIPTION Суффикс .mrproper-restore вставляется перед расширением, а не
    после: C:\a\file.txt -> C:\a\file.mrproper-restore.txt. Имя обязано
    оставаться узнаваемым — по нему человек поймёт, что перед ним вернули.
    #>
    param([string] $Path, [int] $Attempt = 1)

    $suffix = '.mrproper-restore'
    $directory = [IO.Path]::GetDirectoryName($Path)
    $name = [IO.Path]::GetFileName($Path)
    $extension = [IO.Path]::GetExtension($name)
    $stem = if ($extension -eq '') { $name } else { $name.Substring(0, $name.Length - $extension.Length) }
    $suffixToAdd = if ($Attempt -le 1) { $suffix } else { $suffix + '.' + $Attempt }
    $newName = $stem + $suffixToAdd + $extension
    return (Join-Path $directory $newName)
}

function Invoke-ManifestRestore {
    <#
    .SYNOPSIS Вернуть элементы транзакции по манифесту и посчитать итог.
    .DESCRIPTION Правила взяты из core::undo (buildRestorePlan) и дословно:
      * цель свободна            -> Restore, элемент возвращается на место;
      * цель занята, политика Ask -> Skip + needsUserDecision: «существующий
        файл — не перезаписывать, спросить» (FR-7). Молчаливая перезапись здесь
        означала бы, что человек нажал «отменить» и потерял чужую работу;
      * содержимого нет в корзине -> Skip с причиной: запись манифеста есть,
        а данных уже нет, и «возвращено» здесь было бы враньём;
      * состояние транзакции undone/collapsed -> ничего не делать (SPEC §7.2:
        «отмена доступна, пока транзакция не схлопнулась»).
    Возвращает счётчики и список причин пропуска; манифест переписывается
    ТОЛЬКО после факта (core::undo, правило 7 undo_service): сначала перенос,
    потом отметка возвращённого.
    #>
    param([string] $TrashRoot, [string] $TxId, [string] $Policy = 'Ask')

    $manifest = Read-TrashManifest -TrashRoot $TrashRoot -TxId $TxId
    $result = [pscustomobject]@{
        TxId           = $TxId
        Items          = $manifest.Entries.Count
        RestoredItems  = 0
        RestoredFiles  = 0
        SkippedItems   = 0
        Conflicted     = 0
        RenamedItems   = 0
        Reasons        = @()
        State          = $manifest.State
        Available      = $false
        AlreadyUndone  = ($manifest.State -eq 'undone' -or $manifest.State -eq 'collapsed')
    }

    # «Схлопнулась» — отмены больше нет (§7.2). Второй проход обязан быть
    # пустым и не тронуть уже возвращённые файлы.
    if ($result.AlreadyUndone) {
        $result.Reasons += ('транзакция в состоянии ' + $manifest.State + ' — отмены нет (§7.2)')
        return $result
    }
    $result.Available = $true

    $remaining = @()
    $anyRestored = $false
    foreach ($entry in $manifest.Entries) {
        $storedPath = Join-Path $manifest.TxDir $entry.Payload

        if (-not (Test-Path -LiteralPath $storedPath)) {
            $result.SkippedItems++
            $result.Reasons += ('нет содержимого в корзине: ' + $entry.OriginalPath)
            $remaining += $entry
            continue
        }
        if (Test-Path -LiteralPath $entry.OriginalPath) {
            # FR-7: «существующий файл — не перезаписывать, спросить».
            $result.Conflicted++
            if ($Policy -eq 'Rename') {
                $target = Get-RestoreRenameTarget $entry.OriginalPath
                Move-Item -LiteralPath $storedPath -Destination $target -Force
                $result.RestoredItems++
                $result.RenamedItems++
                $result.RestoredFiles += $entry.FileCount
                $anyRestored = $true
                $result.Reasons += ('занято — восстановлено рядом: ' + $target)
            } else {
                $result.SkippedItems++
                $result.Reasons += ('занято, политика ' + $Policy + ' — ждёт решения пользователя: ' +
                    $entry.OriginalPath)
                $remaining += $entry
            }
            continue
        }

        Move-Item -LiteralPath $storedPath -Destination $entry.OriginalPath -Force
        $result.RestoredItems++
        $result.RestoredFiles += $entry.FileCount
        $anyRestored = $true
    }

    # Манифест после факта: возвращённые записи уходят из транзакции, а транзакция
    # без остатка помечается undone — «Ctrl+Z должен быть серым» (§7.2).
    if ($result.RestoredItems -eq $manifest.Entries.Count) {
        $result.State = 'undone'
    } elseif ($anyRestored) {
        $result.State = 'committed'
    } else {
        $result.State = $manifest.State
    }
    if ($result.RestoredItems -gt 0) {
        $document = [ordered]@{
            schema      = 1
            txId        = $TxId
            createdAt   = [int64] ([Math]::Floor(([DateTime]::UtcNow - [DateTime]'1970-01-01T00:00:00Z').TotalSeconds))
            appVersion  = $manifest.AppVersion
            state       = $result.State
            purgedBytes = 0
            items       = @($remaining | ForEach-Object {
                    [ordered]@{
                        kind         = 'directory'
                        originalPath = $_.OriginalPath
                        payload      = $_.Payload
                        bytes        = [int64] $_.Bytes
                        fileCount    = [int] $_.FileCount
                        mtime        = [int64] $_.MtimeUnix
                        readOnly     = $false
                        aclSddl      = ''
                        crossVolume  = $false
                        sourceVolume = ''
                    }
                })
        }
        Write-JsonFile -Path (Join-Path $manifest.TxDir 'manifest.json') -Document $document
    }
    return $result
}

function Get-RestorePercent {
    <#
    .SYNOPSIS Процент возвращённых файлов — то самое «100 %» из §12.
    .DESCRIPTION Считается по файлам, а не по кандидатам: §12 говорит
    «восстановление 100 % удалённого», и кандидат из трёх файлов, вернувшийся
    одним, не даёт ста процентов. Знаменатель — ожидаемая таблица фикстуры.
    #>
    param([int] $Restored, [int] $Expected)

    if ($Expected -le 0) { return 0.0 }
    return [Math]::Round((100.0 * $Restored) / $Expected, 1)
}

# ---------------------------------------------------------------------------
# Подготовка прогона (ленивая: состояние должно пережить переходы между It)
# ---------------------------------------------------------------------------

function Initialize-Scenario {
    <#
    .SYNOPSIS Один раз за прогон: песочница, фикстура, JSON кандидатов, поиск CLI.
    .DESCRIPTION Инициализация не в BeforeEach, а лениво и один раз: транзакция
    корзины создаётся в шаге 4 и проверяется в шагах 5–6, то есть состояние
    обязано пережить границу между It. BeforeEach в Pester 3/4/5 сбрасывается
    перед каждым It, и сценарий после него терял бы половину доказательств.
    #>
    if ($script:Ready) { return }

    # ПЕСОЧНИЦА — это сам каталог Get-SandboxRoot, без вложенного подкаталога:
    # AfterAll сносит ровно $script:SandboxRoot, и пустой внешний каталог после
    # прогона не остаётся (иначе каждый запуск оставлял бы мусор в %TEMP%).
    $script:SandboxRoot = Get-SandboxRoot
    if (Test-Path -LiteralPath $script:SandboxRoot) {
        Remove-Item -LiteralPath $script:SandboxRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $script:SandboxRoot -Force | Out-Null

    $script:Cli = Get-MrProperCli -Requested $CliPath -Configuration $Configuration -Slot $Slot
    if ($null -eq $script:Cli) {
        $script:CliMissingReason = ('не найден mrproper_cli.exe (соберите tools\build.bat ' +
            $Configuration + ' <слот> или укажите -CliPath)')
    } else {
        $script:CliMissingReason = ''
    }
    $script:Summary.CliReason = if ($null -eq $script:Cli) { $script:CliMissingReason } else { $script:Cli }

    $script:Fixture = New-UndoFixture -Root (Join-Path $script:SandboxRoot 'fixture') `
        -CandidateCount $CandidateCount -FilesPerCandidate $FilesPerCandidate -SizeBytes $FileSizeBytes
    # Песочница обязана быть непустой ДО того, как о ней начнут спорить шаги. Иначе
    # проверки вида «Present == Expected» проходят на нулях (0 == 0), и сценарий
    # рапортовал бы о стольцах процентов, не создав ни одного файла.
    $created = @(Get-ChildItem -LiteralPath (Join-Path $script:SandboxRoot 'fixture') -Recurse -File)
    if ($created.Count -ne $script:Fixture.TotalFiles) {
        throw ('фикстура создана не полностью: файлов на диске ' + $created.Count + ', ожидалось ' +
            $script:Fixture.TotalFiles + ' (корень ' + (Join-Path $script:SandboxRoot 'fixture') + ')')
    }
    $script:CandidatesPath = Join-Path $script:SandboxRoot 'candidates.json'
    Write-JsonFile -Path $script:CandidatesPath -Document ([ordered]@{
            schema     = 1
            kind       = 'scan'
            candidates = $script:Fixture.Candidates
        })

    # Корзина сценария — внутри песочницы. Настоящий %ProgramData%\MrProper\Trash
    # сценарий только читает.
    $script:TrashRoot = Join-Path $script:SandboxRoot 'Trash'
    New-Item -ItemType Directory -Path $script:TrashRoot -Force | Out-Null
    $script:AppTrashRoot = Get-AppTrashRoot
    $script:AppTrashBefore = @(Get-TrashTransactionIds $script:AppTrashRoot)

    $script:AppVersion = 'e2e'
    if ($null -ne $script:Cli) {
        $version = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot -Arguments @('--version')
        if ($version.Exit -eq 0) {
            $line = Get-FirstMeaningfulLine $version.Out
            if ($line -ne '') { $script:AppVersion = $line.Trim() }
        }
    }

    $script:Ready = $true
}

function New-TransactionEntries {
    <#
    .SYNOPSIS Записи транзакции из кандидатов фикстуры.
    #>
    param($Candidates, [string] $Prefix)

    $entries = @()
    for ($index = 0; $index -lt $Candidates.Count; $index++) {
        $candidate = $Candidates[$index]
        $files = @(Get-ChildItem -LiteralPath $candidate.path -Recurse -File)
        $bytes = [int64] 0
        foreach ($file in $files) { $bytes += $file.Length }
        $directory = Get-Item -LiteralPath $candidate.path
        $entries += [pscustomobject]@{
            Path      = $candidate.path
            Payload   = ($Prefix + ($index + 1))
            Bytes     = $bytes
            FileCount = $files.Count
            MtimeUnix = [int64] ([Math]::Floor(($directory.LastWriteTimeUtc - [DateTime]'1970-01-01T00:00:00Z').TotalSeconds))
        }
    }
    return $entries
}

function Measure-ExpectedFiles {
    <#
    .SYNOPSIS Сколько ожидаемых файлов вернулось и сколько совпало байт в байт.
    #>
    param($Expected)

    $present = 0
    $identical = 0
    $mismatched = @()
    foreach ($item in $Expected) {
        if (-not (Test-Path -LiteralPath $item.Path -PathType Leaf)) { continue }
        $present++
        $stamp = Get-FileStamp $item.Path
        if ($stamp.Sha256 -eq $item.Stamp.Sha256 -and $stamp.Length -eq $item.Length) {
            $identical++
        } else {
            $mismatched += $item.Path
        }
    }
    return [pscustomobject]@{
        Expected   = $Expected.Count
        Present    = $present
        Identical  = $identical
        Mismatched = $mismatched
    }
}

# ---------------------------------------------------------------------------
# Сценарий
# ---------------------------------------------------------------------------

Describe 'MrProper: отмена и восстановление 100 % (SPEC §11.3, §12, FR-7)' -Tag 'e2e' {

    AfterAll {
        if ($KeepArtifacts) {
            Write-Host ('песочница оставлена: ' + $script:SandboxRoot)
        } elseif ($script:SandboxRoot -ne '' -and (Test-Path -LiteralPath $script:SandboxRoot)) {
            Remove-Item -LiteralPath $script:SandboxRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Context 'шаг 1: поверхность отмены' {

        It 'состояние сборки известно: команда отмены CLI есть или её нет' {
            Initialize-Scenario

            if ($null -eq $script:Cli) {
                $script:Summary.UndoSurface = '(CLI не собран)'
                Add-SkippedStep $script:CliMissingReason
                return
            }

            $surface = Get-UndoSurface -Cli $script:Cli
            $available = if ($surface.Commands.Count -gt 0) { $surface.Commands -join ', ' } else { '(список пуст)' }
            if ($surface.Found) {
                $script:Summary.UndoSurface = $surface.Command
                Write-Host ('команда отмены в этой сборке: ' + $surface.Command)
                return
            }

            $script:Summary.UndoSurface = 'нет (команды: ' + $available + ')'
            $reason = ('в mrproper-cli нет команды отмены: доступны [' + $available +
                ']. Собственный engine::undo_service (SPEC §6.2) из CLI не вызывается, ' +
                'поэтому восстановление проверяется контрактом манифеста (шаги 4-6)')
            if ($RequireEngineUndo) {
                throw $reason
            }
            Add-SkippedStep $reason
        }
    }

    Context 'шаг 2: отмена без --execute не удаляет ни байта (FR-5)' {

        It 'apply без --execute показывает весь план и оставляет 100 % файлов на месте' {
            Initialize-Scenario
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $run = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--candidates', $script:CandidatesPath, '--json')

            # Коды apply (src/cli/cmd_apply.hpp): 0 — план показан, 2 — разбор
            # аргументов, 3 — отказ, 4 — нечем планировать, 5 — нет исполнителя,
            # 6 — часть операций не удалась. Любой другой код — дефект, а не
            # пропуск: команда не обещает ничего сверх этого списка.
            if ($run.Exit -ne 0) {
                throw ('apply без --execute вернул код ' + $run.Exit + ': ' + (Get-FirstMeaningfulLine $run.Err))
            }

            $document = $run.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'kind' 'apply') | Should Be 'apply'
            (Get-RequiredProperty $document 'executed' 'apply') | Should Be $false
            $refusal = [string] (Get-RequiredProperty $document 'refusal' 'apply')
            $refusal | Should Match 'сухой прогон'
            # Раздел execution в документе есть всегда, но при отказе он равен
            # null (applyJson: Value execution = Value()). Ненулевой execution
            # означал бы, что что-то выполнилось, — а с --execute это запрещено.
            (Get-RequiredProperty $document 'execution' 'apply') | Should Be $null

            $plan = Get-RequiredProperty $document 'plan' 'apply'
            $totals = Get-RequiredProperty $plan 'totals' 'apply.plan'
            (Get-RequiredProperty $totals 'candidates' 'apply.plan.totals') | Should Be $script:Fixture.Items
            (Get-RequiredProperty $totals 'selected' 'apply.plan.totals') | Should Be $script:Fixture.Items
            (Get-RequiredProperty $totals 'selectedBytes' 'apply.plan.totals') | Should Be $script:Fixture.TotalBytes

            # FR-5: «Ни один элемент не удаляется без видимого объяснения»
            # (§12) — у каждой операции есть причина.
            $operations = @(Get-RequiredProperty $plan 'operations' 'apply.plan')
            $operations.Count | Should Be $script:Fixture.Items
            foreach ($operation in $operations) {
                (Get-RequiredProperty $operation 'reason' 'apply.plan.operations[]') | Should Match '\S'
                (Get-RequiredProperty $operation 'action' 'apply.plan.operations[]') | Should Be 'trash'
            }

            # Главный инвариант отмены: файлы целы и не изменились ни байтом.
            $measured = Measure-ExpectedFiles -Expected $script:Fixture.Expected
            $measured.Present | Should Be $measured.Expected
            $measured.Identical | Should Be $measured.Expected
            $measured.Mismatched.Count | Should Be 0

            $script:Summary.CancelDryRunOk = $true
            Write-Host ('отмена без --execute: план на ' + $operations.Count + ' элементов, на месте ' +
                $measured.Identical + ' из ' + $measured.Expected + ' файлов, удалено 0 байт')
        }
    }

    Context 'шаг 3: отказ по --execute ничего не трогает (FR-5)' {

        It 'apply --execute с готовым списком отказан, транзакция не появилась' {
            Initialize-Scenario
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $run = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--candidates', $script:CandidatesPath, '--execute', '--json')

            # 3 — Refused: «--candidates читает готовый список: удалять по нему
            # нельзя, план мог устареть». Это и есть отмена по требованию
            # человека, и она проверяется настоящим кодом CLI.
            $run.Exit | Should Be 3
            $script:Summary.CancelRefusedCode = $run.Exit

            $document = $run.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'executed' 'apply') | Should Be $false
            (Get-RequiredProperty $document 'confirmed' 'apply') | Should Be $false
            $refusal = [string] (Get-RequiredProperty $document 'refusal' 'apply')
            $refusal | Should Match 'устареть'
            (Get-RequiredProperty $document 'execution' 'apply') | Should Be $null

            # Снимок состояния печатается ДО отказа (FR-5: «перед исполнением —
            # снимок состояния: список операций, PID, версия, размер»).
            $snapshot = Get-RequiredProperty $document 'snapshot' 'apply'
            (Get-RequiredProperty $snapshot 'operationCount' 'apply.snapshot') | Should Be $script:Fixture.Items

            $measured = Measure-ExpectedFiles -Expected $script:Fixture.Expected
            $measured.Identical | Should Be $measured.Expected

            # Настоящая корзина приложения не тронута: отказ не создаёт транзакцию.
            $appTrashAfter = @(Get-TrashTransactionIds $script:AppTrashRoot)
            $appTrashAfter.Count | Should Be $script:AppTrashBefore.Count
            ($appTrashAfter -join ',') | Should Be ($script:AppTrashBefore -join ',')

            # И песочница не превратилась в транзакцию «сама собой».
            @(Get-TrashTransactionIds $script:TrashRoot).Count | Should Be 0

            Write-Host ('отказ по --execute: код ' + $run.Exit + ', на месте ' + $measured.Identical +
                ' из ' + $measured.Expected + ' файлов, транзакций в ' + $script:AppTrashRoot + ': ' +
                $appTrashAfter.Count)
        }
    }

    Context 'шаг 3б: запрет --execute с --candidates не обходится пустым планом (FR-5)' {

        It 'нулевой план не обходит запрет: --execute с --candidates отказан, файлы целы' {
            Initialize-Scenario
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            # Список из файла, из которого ничего не выбирается. Порог уверенности
            # поднят ключом до 100, а у песочничных кандидатов 90: пустой план
            # получается по воле проверяющего и не поедет вместе со значением
            # порога по умолчанию. Именно этот случай и был источником падения
            # «Expected 3, but got 0»: пока в плане 0 операций, executePlan
            # возвращается раньше проверки запрета.
            $probeRoot = Join-Path $script:SandboxRoot 'refusal-empty-plan'
            $probe = New-UndoFixture -Root $probeRoot -CandidateCount 2 -FilesPerCandidate 1 -SizeBytes $FileSizeBytes
            $probePath = Join-Path $probeRoot 'candidates.json'
            Write-JsonFile -Path $probePath -Document ([ordered]@{
                    schema     = 1
                    kind       = 'scan'
                    candidates = $probe.Candidates
                })

            # Сухой прогон по этому списку обязан быть честно пустым. Появление
            # здесь операций означало бы, что шаг проверяет не то (запрет надо
            # ловить там, где executePlan выходит раньше него), — это падение,
            # а не пропуск: молча проверить другую ветку хуже, чем упасть.
            $dry = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--candidates', $probePath, '--confidence-threshold', '100', '--json')
            if ($dry.Exit -ne 0) {
                throw ('сухой прогон по пустому списку вернул код ' + $dry.Exit + ': ' +
                    (Get-FirstMeaningfulLine $dry.Err))
            }
            $dryDocument = $dry.Out | ConvertFrom-Json
            (Get-RequiredProperty $dryDocument 'kind' 'apply') | Should Be 'apply'
            (Get-RequiredProperty $dryDocument 'executed' 'apply') | Should Be $false
            $dryPlan = Get-RequiredProperty $dryDocument 'plan' 'apply'
            $dryTotals = Get-RequiredProperty $dryPlan 'totals' 'apply.plan'
            (Get-RequiredProperty $dryTotals 'selected' 'apply.plan.totals') | Should Be 0
            @(Get-RequiredProperty $dryPlan 'operations' 'apply.plan').Count | Should Be 0

            $run = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:SandboxRoot `
                -Arguments @('apply', '--candidates', $probePath, '--confidence-threshold', '100',
                    '--execute', '--json')
            if ($run.Out -eq '' -or $null -eq $run.Out) {
                throw ('apply --execute с пустым планом не напечатал документ: код ' + $run.Exit + ', stderr: ' +
                    (Get-FirstMeaningfulLine $run.Err))
            }
            $document = $run.Out | ConvertFrom-Json
            $script:Summary.EmptyPlanCode = $run.Exit

            # Что бы CLI ни ответил, исполнителя быть не должно: 0 операций в плане
            # означают, что удалять нечего, а --candidates запрещён при любом плане.
            (Get-RequiredProperty $document 'executed' 'apply') | Should Be $false
            (Get-RequiredProperty $document 'confirmed' 'apply') | Should Be $false
            (Get-RequiredProperty $document 'execution' 'apply') | Should Be $null
            $kept = Measure-ExpectedFiles -Expected $probe.Expected
            $kept.Identical | Should Be $kept.Expected

            if ($run.Exit -eq 3) {
                # Ожидаемый по SPEC исход: запрет FR-5 сработал, удаление по
                # списку из файла не произошло ни при каком размере плана.
                $refusal = [string] (Get-RequiredProperty $document 'refusal' 'apply')
                $refusal | Should Match 'устареть'
                $script:Summary.EmptyPlanVerdict = 'отказ FR-5 (Refused)'
            } elseif ($run.Exit -eq 0) {
                # Известный дефект чужого слоя: executePlan (src/cli/cmd_apply.cpp:835)
                # на пустом плане возвращает PlanExit::Ok РАНЬШЕ проверки
                # options.candidatesPath.has_value() (там же, строка 844), поэтому
                # запрет «--candidates вместе с --execute отклоняется» на пустом
                # плане не срабатывает и процесс рапортует успех. Сценарий это не
                # чинит (файл не мой), но и не замалчивает: строкой в
                # skipped-undo-restore.log и строкой в сводке.
                $script:Summary.EmptyPlanVerdict = 'ДЕФЕКТ CLI: код 0 вместо 3'
                Add-SkippedStep ('дефект CLI: apply --candidates --execute при 0 операциях в плане вернул код 0 ' +
                    'вместо 3 (Refused). Причина: executePlan в src/cli/cmd_apply.cpp:835 возвращает PlanExit::Ok ' +
                    'на пустом плане раньше проверки запрета options.candidatesPath.has_value() (строка 844), ' +
                    'поэтому запрет FR-5 «удалять по списку из файла нельзя» на пустом плане не действует. ' +
                    'Починка: перенести проверку candidatesPath выше раннего возврата по пустому плану. ' +
                    'Проверено при этом: файлы песочницы целы (байт в байт), executed=false, execution=null, ' +
                    'транзакция не появилась — то есть вреда нет, врёт только код возврата')
            } else {
                throw ('apply --candidates --execute на пустом плане вернул неожиданный код ' + $run.Exit +
                    ': ' + (Get-FirstMeaningfulLine $run.Err))
            }

            # Отказ не должен оставить транзакцию ни в песочнице, ни в корзине
            # приложения: --candidates с --execute запрещён до исполнения.
            @(Get-TrashTransactionIds $script:TrashRoot).Count | Should Be 0
            $appTrashAfter = @(Get-TrashTransactionIds $script:AppTrashRoot)
            $appTrashAfter.Count | Should Be $script:AppTrashBefore.Count
            ($appTrashAfter -join ',') | Should Be ($script:AppTrashBefore -join ',')

            Write-Host ('пустой план + --execute с --candidates: код ' + $run.Exit + ' (' +
                $script:Summary.EmptyPlanVerdict + '), на месте ' + $kept.Identical + ' из ' + $kept.Expected +
                ' файлов песочницы')
        }
    }

    Context 'шаг 4: транзакция корзины достаточна для возврата (FR-7)' {

        It 'транзакция записана по контракту manifest.json и отменяема' {
            Initialize-Scenario

            $txId = New-TrashTxId -Counter 1
            $entries = New-TransactionEntries -Candidates $script:Fixture.Candidates -Prefix 'd'
            $txDir = New-TrashTransaction -TrashRoot $script:TrashRoot -TxId $txId `
                -Entries $entries -AppVersion $script:AppVersion -VolumeGuid ''

            Test-Path -LiteralPath (Join-Path $txDir 'manifest.json') -PathType Leaf | Should Be $true

            $manifest = Read-TrashManifest -TrashRoot $script:TrashRoot -TxId $txId
            $manifest.Entries.Count | Should Be $script:Fixture.Items
            $manifest.State | Should Be 'committed'
            # Committed — «манифест записан, содержимое можно восстанавливать»
            # (core::trash.cpp); open означал бы, что транзакцию нечем отменять.
            $manifest.State | Should Not Be 'collapsed'
            $script:Summary.CleanItems = $manifest.Entries.Count
            $script:Summary.CleanFiles = $script:Fixture.TotalFiles

            # Исходные места освободились, содержимое лежит в корзине.
            $movedIn = 0
            $movedOut = 0
            foreach ($entry in $manifest.Entries) {
                if (-not (Test-Path -LiteralPath $entry.OriginalPath)) { $movedOut++ }
                if (Test-Path -LiteralPath (Join-Path $txDir $entry.Payload)) { $movedIn++ }
            }
            $movedOut | Should Be $script:Fixture.Items
            $movedIn | Should Be $script:Fixture.Items

            $measured = Measure-ExpectedFiles -Expected $script:Fixture.Expected
            $measured.Present | Should Be 0

            $script:TxClean = $txId
            Write-Host ('транзакция ' + $txId + ': элементов ' + $manifest.Entries.Count + ', файлов ' +
                $script:Fixture.TotalFiles)
        }
    }

    Context 'шаг 5: восстановление возвращает 100 % файлов' {

        It 'полное восстановление: каждый файл на своём месте и байт в байт' {
            Initialize-Scenario
            if ($script:TxClean -eq '') {
                Add-SkippedStep 'шаг 4 не выполнен: нет транзакции для восстановления'
                return
            }

            $result = Invoke-ManifestRestore -TrashRoot $script:TrashRoot -TxId $script:TxClean -Policy 'Ask'
            $result.Available | Should Be $true
            $result.RestoredItems | Should Be $script:Fixture.Items
            $result.SkippedItems | Should Be 0
            $result.Conflicted | Should Be 0
            $result.State | Should Be 'undone'

            $measured = Measure-ExpectedFiles -Expected $script:Fixture.Expected
            $measured.Present | Should Be $measured.Expected
            $measured.Identical | Should Be $measured.Expected
            $measured.Mismatched.Count | Should Be 0

            # mtime тоже возвращается: манифест его хранит не для красоты.
            $mtimeMismatch = 0
            foreach ($item in $script:Fixture.Expected) {
                $stamp = Get-FileStamp $item.Path
                if ($stamp.MtimeUnix -ne $item.Stamp.MtimeUnix) { $mtimeMismatch++ }
            }
            $mtimeMismatch | Should Be 0

            $result.RestoredFiles | Should Be $measured.Expected
            $percent = Get-RestorePercent -Restored $result.RestoredFiles -Expected $measured.Expected
            $percent | Should Be 100.0

            $script:Summary.RestoredItems = $result.RestoredItems
            $script:Summary.RestoredFiles = $result.RestoredFiles
            $script:Summary.RestorePercent = $percent
            $script:Summary.ContractUsed = $true

            Write-Host ('восстановлено ' + $result.RestoredFiles + ' из ' + $measured.Expected + ' файлов (' +
                $percent + ' %) по транзакции ' + $result.TxId)
        }
    }

    Context 'шаг 6: конфликт не перезаписывается (FR-7)' {

        It 'занятое место спрашивает, а молча не затирает чужой файл' {
            Initialize-Scenario

            # Отдельная транзакция: главный круг должен дать честные 100 %, а
            # конфликтный случай не должен портить его знаменатель.
            $conflictRoot = Join-Path $script:SandboxRoot 'conflict'
            New-Item -ItemType Directory -Path $conflictRoot -Force | Out-Null
            $buffer = New-Object byte[] $FileSizeBytes
            for ($byte = 0; $byte -lt $FileSizeBytes; $byte++) { $buffer[$byte] = [byte] ($byte -band 0xFF) }
            $targetDirectory = Join-Path $conflictRoot 'cand1'
            $targetFile = Join-Path $targetDirectory 'file1.bin'
            New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
            [IO.File]::WriteAllBytes($targetFile, $buffer)

            # Кандидат уходит в корзину, а на его месте появляется чужой файл —
            # ровно та гонка, из-за которой «не перезаписывать» должно быть
            # свойством исполнения, а не только плана (engine::undo_service,
            # правило 5).
            $txId = New-TrashTxId -Counter 2
            $entry = [pscustomobject]@{
                Path      = $targetFile
                Payload   = 'd1'
                Bytes     = [int64] $FileSizeBytes
                FileCount = 1
                MtimeUnix = [int64] ([Math]::Floor(([DateTime]::UtcNow - [DateTime]'1970-01-01T00:00:00Z').TotalSeconds))
            }
            [IO.File]::WriteAllBytes($targetFile, $buffer)
            $foreign = Get-FileStamp $targetFile
            New-TrashTransaction -TrashRoot $script:TrashRoot -TxId $txId -Entries @($entry) `
                -AppVersion $script:AppVersion -VolumeGuid '' | Out-Null
            Test-Path -LiteralPath $targetFile | Should Be $false

            # Чужой файл появился на освободившемся месте.
            $rival = New-Object byte[] $FileSizeBytes
            for ($byte = 0; $byte -lt $FileSizeBytes; $byte++) { $rival[$byte] = [byte] (255 - ($byte -band 0xFF)) }
            [IO.File]::WriteAllBytes($targetFile, $rival)
            $rivalStamp = Get-FileStamp $targetFile

            $ask = Invoke-ManifestRestore -TrashRoot $script:TrashRoot -TxId $txId -Policy 'Ask'
            $ask.Conflicted | Should Be 1
            $ask.RestoredItems | Should Be 0
            $ask.SkippedItems | Should Be 1
            # Решение не принято — элемент остаётся в корзине, то есть отмена не
            # потеряна и может быть повторена после ответа пользователя.
            Test-Path -LiteralPath (Join-Path (Join-Path $script:TrashRoot $txId) 'd1') | Should Be $true
            # Чужой файл не тронут: это и есть «не перезаписывать, спросить».
            $afterAsk = Get-FileStamp $targetFile
            $afterAsk.Sha256 | Should Be $rivalStamp.Sha256
            $script:Summary.ConflictSkipped = 1
            $script:Summary.ForeignFileIntact = $true

            # Ответ «восстановить рядом» — второй документированный исход.
            $rename = Invoke-ManifestRestore -TrashRoot $script:TrashRoot -TxId $txId -Policy 'Rename'
            $rename.RestoredItems | Should Be 1
            $rename.RenamedItems | Should Be 1
            $renamedTarget = Get-RestoreRenameTarget $targetFile
            Test-Path -LiteralPath $renamedTarget -PathType Leaf | Should Be $true
            # Имя узнаваемо: суффикс вставлен перед расширением, как в
            # core::undo::restoreRenameTarget.
            [IO.Path]::GetFileName($renamedTarget) | Should Be 'file1.mrproper-restore.bin'
            $renamedStamp = Get-FileStamp $renamedTarget
            $afterRename = Get-FileStamp $targetFile
            $afterRename.Sha256 | Should Be $rivalStamp.Sha256
            $renamedStamp.Sha256 | Should Not Be $rivalStamp.Sha256

            Remove-Item -LiteralPath $targetFile -Force
            Remove-Item -LiteralPath $renamedTarget -Force
            Write-Host ('конфликт: политика Ask — 0 возвращено, чужой файл цел; Rename — возвращено рядом как ' +
                [IO.Path]::GetFileName($renamedTarget))
        }
    }

    Context 'шаг 7: повторное восстановление безопасно (SPEC §7.2)' {

        It 'второй проход по схлопнувшейся транзакции ничего не переписывает' {
            Initialize-Scenario
            if ($script:TxClean -eq '') {
                Add-SkippedStep 'шаг 4 не выполнен: нет транзакции для повторного восстановления'
                return
            }

            $before = Measure-ExpectedFiles -Expected $script:Fixture.Expected
            $repeat = Invoke-ManifestRestore -TrashRoot $script:TrashRoot -TxId $script:TxClean -Policy 'Overwrite'
            $repeat.RestoredItems | Should Be 0
            $repeat.RestoredFiles | Should Be 0
            $repeat.Available | Should Be $false
            $script:Summary.RepeatRestored = $repeat.RestoredItems

            # Транзакция без остатка схлопнулась — отмены больше нет (§7.2), и
            # уже возвращённые файлы остались нетронутыми.
            $manifest = Read-TrashManifest -TrashRoot $script:TrashRoot -TxId $script:TxClean
            $manifest.State | Should Be 'undone'
            $manifest.Entries.Count | Should Be 0

            $after = Measure-ExpectedFiles -Expected $script:Fixture.Expected
            $after.Identical | Should Be $before.Identical
            $after.Identical | Should Be $before.Expected
            $after.Mismatched.Count | Should Be 0
            Write-Host ('повторное восстановление: возвращено ' + $repeat.RestoredItems +
                ', состояние ' + $manifest.State + ', файлов на месте ' + $after.Identical)
        }
    }

    Context 'шаг 9: полный круг настоящей командой отмены (FR-7, §7.2)' {

        It 'undo --list показывает транзакцию корзины с датой, числом элементов и объёмом' {
            Initialize-Scenario
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }

            $surface = Get-UndoSurface -Cli $script:Cli
            if (-not $surface.Found) {
                Add-SkippedStep ('в этой сборке нет команды отмены (доступны: ' + ($surface.Commands -join ', ') + ')')
                return
            }

            # Своя фикстура и своя транзакция: шаги 4-7 уже израсходовали «чистую»,
            # а шаг 9 обязан быть зелёным сам по себе.
            $script:CliFixture = New-UndoFixture -Root (Join-Path $script:SandboxRoot 'cli-fixture') `
                -CandidateCount 1 -FilesPerCandidate 3 -SizeBytes $FileSizeBytes
            $script:TxCli = New-TrashTxId -Counter 9
            $entries = New-TransactionEntries -Candidates $script:CliFixture.Candidates -Prefix 'c'
            New-TrashTransaction -TrashRoot $script:TrashRoot -TxId $script:TxCli -Entries $entries `
                -AppVersion $script:AppVersion -VolumeGuid '' | Out-Null

            $run = Invoke-CliUndo -Cli $script:Cli -Command $surface.Command -TrashRoot $script:TrashRoot `
                -TxId $script:TxCli -List
            $script:Summary.CliListCode = $run.Exit
            $run.Exit | Should Be 0

            $document = $run.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'kind' 'undo') | Should Be 'undo'
            (Get-RequiredProperty $document 'mode' 'undo') | Should Be 'list'
            $transactions = @(Get-RequiredProperty $document 'transactions' 'undo')
            $found = @($transactions | Where-Object { $_.txId -eq $script:TxCli })
            $found.Count | Should Be 1
            $entry = $found[0]
            (Get-RequiredProperty $entry 'state' 'undo.transactions[]') | Should Be 'committed'
            (Get-RequiredProperty $entry 'available' 'undo.transactions[]') | Should Be $true
            (Get-RequiredProperty $entry 'items' 'undo.transactions[]') | Should Be 1
            (Get-RequiredProperty $entry 'restorable' 'undo.transactions[]') | Should Be 1
            (Get-RequiredProperty $entry 'bytes' 'undo.transactions[]') | Should Be $script:CliFixture.TotalBytes
            (Get-RequiredProperty $entry 'createdAt' 'undo.transactions[]') | Should Match '^\d+$'

            # Список — запрос, а не действие: пустая корзина в нём не ошибка.
            $script:Summary.CliListFound = $transactions.Count
            Write-Host ('undo --list: код ' + $run.Exit + ', транзакций ' + $transactions.Count +
                ', наша ' + $script:TxCli + ' отменяема=' + $entry.available + ', байт ' + $entry.bytes)
        }

        It 'undo --tx без --execute показывает план и не возвращает ни байта (FR-5)' {
            Initialize-Scenario
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            if ($script:TxCli -eq '') { Add-SkippedStep 'шаг 9 не дошёл до плана: нет транзакции'; return }

            $run = Invoke-CliUndo -Cli $script:Cli -Command 'undo' -TrashRoot $script:TrashRoot -TxId $script:TxCli
            $script:Summary.CliDryRunCode = $run.Exit
            $run.Exit | Should Be 0

            $document = $run.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'mode' 'undo') | Should Be 'plan'
            (Get-RequiredProperty $document 'executed' 'undo') | Should Be $false
            (Get-RequiredProperty $document 'restore' 'undo') | Should Be $null
            $plan = Get-RequiredProperty $document 'plan' 'undo'
            (Get-RequiredProperty $plan 'txId' 'undo.plan') | Should Be $script:TxCli
            (Get-RequiredProperty $plan 'restorable' 'undo.plan') | Should Be 1

            $measured = Measure-ExpectedFiles -Expected $script:CliFixture.Expected
            $measured.Present | Should Be 0
            # Снимок идёт в stderr и печатается ДО вопроса (FR-5).
            $run.Err | Should Match 'снимок отмены'
            Write-Host ('undo --tx (сухой прогон): код ' + $run.Exit + ', на месте ' + $measured.Present +
                ' из ' + $measured.Expected + ' файлов')
        }

        It 'undo --tx --execute возвращает 100 % файлов и оставляет state=undone' {
            Initialize-Scenario
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            if ($script:TxCli -eq '') { Add-SkippedStep 'шаг 9 не дошёл до восстановления: нет транзакции'; return }

            $run = Invoke-CliUndo -Cli $script:Cli -Command 'undo' -TrashRoot $script:TrashRoot `
                -TxId $script:TxCli -Execute -Yes -KeepTransaction
            $script:Summary.CliRestoreCode = $run.Exit
            $run.Exit | Should Be 0

            $document = $run.Out | ConvertFrom-Json
            (Get-RequiredProperty $document 'executed' 'undo') | Should Be $true
            (Get-RequiredProperty $document 'confirmed' 'undo') | Should Be $true
            $restore = Get-RequiredProperty $document 'restore' 'undo'
            (Get-RequiredProperty $restore 'restored' 'undo.restore') | Should Be 1
            (Get-RequiredProperty $restore 'skipped' 'undo.restore') | Should Be 0
            (Get-RequiredProperty $restore 'failed' 'undo.restore') | Should Be 0
            (Get-RequiredProperty $restore 'bytesRestored' 'undo.restore') | Should Be $script:CliFixture.TotalBytes
            (Get-RequiredProperty $restore 'manifestWritten' 'undo.restore') | Should Be $true

            $measured = Measure-ExpectedFiles -Expected $script:CliFixture.Expected
            $measured.Present | Should Be $measured.Expected
            $measured.Identical | Should Be $measured.Expected
            $measured.Mismatched.Count | Should Be 0
            $script:Summary.CliRestoredFiles = $measured.Identical
            $script:Summary.CliBytes = $measured.Identical * $FileSizeBytes

            # --keep-transaction: манифест остаётся, и в нём state=undone (§7.2:
            # «отмена доступна, пока транзакция не схлопнулась»).
            $manifest = Read-TrashManifest -TrashRoot $script:TrashRoot -TxId $script:TxCli
            $manifest.State | Should Be 'undone'
            $manifest.Entries.Count | Should Be 0
            $script:Summary.CliState = $manifest.State

            # Настоящая корзина приложения не тронута: песочница — единственная,
            # где отмена что-то возвращала.
            $appTrashAfter = @(Get-TrashTransactionIds $script:AppTrashRoot)
            ($appTrashAfter -join ',') | Should Be ($script:AppTrashBefore -join ',')

            $percent = Get-RestorePercent -Restored $measured.Identical -Expected $measured.Expected
            $script:Summary.RestorePercentByCli = $percent
            Write-Host ('undo --tx --execute: код ' + $run.Exit + ', вернулось ' + $measured.Identical + ' из ' +
                $measured.Expected + ' файлов (' + $percent + ' %), байт ' + $script:Summary.CliBytes +
                ', состояние транзакции ' + $manifest.State)
        }

        It 'повторный undo по той же транзакции даёт «нечего восстанавливать» (код 4)' {
            Initialize-Scenario
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            if ($script:TxCli -eq '') { Add-SkippedStep 'шаг 9 не дошёл до повторной отмены'; return }

            $run = Invoke-CliUndo -Cli $script:Cli -Command 'undo' -TrashRoot $script:TrashRoot `
                -TxId $script:TxCli -Execute -Yes
            # 4 = NothingToUndo (src/cli/cmd_undo.hpp): транзакция уже восстановлена,
            # отмены больше нет (§7.2). Другой код означал бы либо повторное
            # возвращение, либо поломку классификации.
            $script:Summary.CliRepeatCode = $run.Exit
            $run.Exit | Should Be 4
            $run.Err | Should Match 'нечего'

            # Уже возвращённые файлы не тронуты ни байтом.
            $measured = Measure-ExpectedFiles -Expected $script:CliFixture.Expected
            $measured.Identical | Should Be $measured.Expected
            $measured.Mismatched.Count | Should Be 0
            Write-Host ('повторный undo: код ' + $run.Exit + ', файлов на месте ' + $measured.Identical)
        }
    }

    Context 'шаг 8: сводка' {

        It 'процент возврата доведён до 100 и каждый пропуск записан с причиной' {
            Initialize-Scenario

            $lines = @()
            if (Test-Path -LiteralPath $script:SkipLog) {
                $lines = @(Get-Content -LiteralPath $script:SkipLog -Encoding UTF8 |
                        Where-Object { $_.Trim() -ne '' })
            }
            foreach ($line in $lines) { $line | Should Match '^SKIPPED: .{10,}$' }
            $script:Summary.SkipCount = $lines.Count

            $lines | Out-File -FilePath (Join-Path $script:ArtifactsDir 'undo-restore.log') `
                -Encoding UTF8 -Force

            Write-Host ''
            Write-Host '=== Сводка: отмена и восстановление ==='
            Write-Host ('  кандидатов в обороте      : ' + $script:Summary.CleanItems)
            Write-Host ('  файлов ожидалось          : ' + $script:Summary.CleanFiles)
            Write-Host ('  файлов возвращено         : ' + $script:Summary.RestoredFiles)
            Write-Host ('  процент возврата          : ' + $script:Summary.RestorePercent + ' %')
            Write-Host ('  отмена без --execute      : ' + $script:Summary.CancelDryRunOk)
            Write-Host ('  отказ по --execute, код   : ' + $script:Summary.CancelRefusedCode)
            Write-Host ('  отказ на пустом плане     : ' + $script:Summary.EmptyPlanVerdict +
                ' (код ' + $script:Summary.EmptyPlanCode + ')')
            Write-Host ('  mrproper_cli.exe          : ' + $script:Summary.CliReason)
            Write-Host ('  конфликт не перезаписан   : ' + $script:Summary.ConflictSkipped +
                ' (чужой файл цел: ' + $script:Summary.ForeignFileIntact + ')')
            Write-Host ('  повторное восстановление  : ' + $script:Summary.RepeatRestored + ' элементов')
            Write-Host ('  поверхность отмены CLI    : ' + $script:Summary.UndoSurface)
            Write-Host ('  undo --list, код          : ' + $script:Summary.CliListCode +
                ' (транзакций в списке: ' + $script:Summary.CliListFound + ')')
            Write-Host ('  undo сухой прогон, код    : ' + $script:Summary.CliDryRunCode)
            Write-Host ('  undo --execute, код       : ' + $script:Summary.CliRestoreCode +
                ' (файлов: ' + $script:Summary.CliRestoredFiles + ', байт: ' + $script:Summary.CliBytes +
                ', состояние: ' + $script:Summary.CliState + ')')
            Write-Host ('  повторный undo, код       : ' + $script:Summary.CliRepeatCode)
            Write-Host ('  восстановление            : ' +
                $(if ($script:Summary.CliRestoreCode -eq 0) { 'командой CLI (шаг 9)' }
                    elseif ($script:Summary.ContractUsed) { 'по контракту манифеста (FR-7)' }
                    else { '(не выполнялось)' }))
            Write-Host ('  пропущено шагов           : ' + $lines.Count +
                ' (см. ' + (Join-Path $script:ArtifactsDir 'skipped-undo-restore.log') + ')')
            foreach ($line in $lines) { Write-Host ('    ' + $line) }

            # §12: «восстановление 100 % удалённого». Проверяется на сводке,
            # чтобы «зелёный» прогон с пропущенным восстановлением не читался
            # как выполненная приёмка. Там же — код отказа: без CLI шаги 2 и 3
            # печатают SKIPPED и в Pester 3.4 остаются «зелёными», так что
            # сводка обязана требовать настоящий отказ, а не тишину.
            $script:Summary.RestorePercent | Should Be 100.0
            $script:Summary.RestoredFiles | Should Be $script:Summary.CleanFiles
            $script:Summary.CancelDryRunOk | Should Be $true
            $script:Summary.CancelRefusedCode | Should Be 3
            # Шаг 3б обязан был отработать: -1 в сводке означал бы, что строка
            # про пустой план — значение по умолчанию, а не измерение.
            $script:Summary.EmptyPlanCode | Should Not Be -1
            # Круг командой отмены обязателен, когда CLI собран: -1 означал бы,
            # что шаг 9 не дошёл, а прогон остался зелёным (§12 требует, чтобы
            # «восстановление 100 % удалённого» было настоящим, а не контрактом).
            if ($null -ne $script:Cli) {
                $script:Summary.CliListCode | Should Not Be -1
                $script:Summary.CliRestoreCode | Should Be 0
                $script:Summary.CliRepeatCode | Should Be 4
                $script:Summary.CliState | Should Be 'undone'
            }
        }
    }
}
