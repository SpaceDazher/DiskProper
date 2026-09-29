#Requires -Version 5.1
<#
.SYNOPSIS
    MrProper: E2E битых правил — после отказа приложение остаётся на предыдущем
    рабочем наборе (SPEC §9.2 п.4, ADR-008).

.DESCRIPTION
    Правила решают, что приложение удаляет, поэтому канал обновления — враждебный
    вход по построению, и §9.2 п.4 формулирует отказ словами, которые этот
    сценарий и проверяет: «Любая ошибка на шагах 1–3 → тихий отказ с записью в
    лог, приложение продолжает работу на предыдущем рабочем наборе. Приложение
    никогда не остаётся без набора правил». ADR-008 добавляет к этому запрет
    применять набор без проверки, а §10 (риск «подмена набора правил из сети»)
    называет этот же сценарий обязательным: «e2e-тест на битом наборе (ADR-008)».

    Проверяется настоящим вызовом mrproper_cli.exe и настоящими кодами возврата
    (src/cli/cmd_rules.cpp: 0 — успех, 65 — данные не годятся, 69 — проверка
    невозможна). Моков и заглушек нет: если шаг нельзя выполнить, он пропускается
    с причиной в build\e2e-artifacts\badrules-skipped.log.

    Раскладка песочницы повторяет документированный диск приложения
    (src/platform/rulesync_client.hpp, «%LOCALAPPDATA%\MrProper\rules»):

        current/    рабочий набор — его читает движок
        previous/   предыдущий рабочий набор — цель отката
        staging/    кандидат: сюда кладут обновление и здесь его проверяют

    Шаги сценария:

      0. КОНТРОЛЬ. Настоящий набор из rules\ копируется в current/ и
         previous/ и проходит шаги 1–5 §9.2: код 0, ok=true, все файлы совпали
         с манифестом, правила разобраны. Контроль обязателен: если бы «отказ»
         получался на любом наборе, проверки отказа ниже ничего бы не значили.
         scan по рабочему набору — настоящая попытка прочитать его движком
         (код 0 — отчёт; 3 или 4 — документированный отказ, шаг пропускается с
         текстом stderr, как это делает Test-FullCycle.ps1).
      1. ОТКАЗ НА КАЖДОМ ШАГЕ ЦЕПОЧКИ. Двенадцать кандидатов, каждый сломан
         ровно одним способом: битый JSON манифеста, неизвестное поле
         манифеста, неподдерживаемая схема, minAppVersion выше версии
         приложения, несовпадение размера, подмена байта при том же размере,
         отсутствующий файл, файл не перечислен в манифесте, битый JSON
         правила при верном хеше (ломается шаг 5), неизвестное поле в правиле
         при верном хеше, набор без подписи, подпись и ключ на месте, но
         верификатора нет. По §9.2 ни один из них не применяется, и после
         каждой попытки рабочий набор обязан остаться байт в байт прежним.
      2. СЕРИЯ ОТКАЗОВ ПОДРЯД. Все кандидаты один за другим на одной
         песочнице, с проверкой неизменности рабочего набора после каждого.
         Это и есть утверждение сценария: приложение не осталось без набора
         правил и не поехало на половине обновления. В конце проверяются оба
         набора — рабочий и предыдущий.

    Чего сценарий НЕ делает и почему (правда важнее зелёного отчёта):

      * не запускает графическое приложение mrproper.exe. Оно WIN32-GUI
        (src/ui/CMakeLists.txt), без headless-режима, и проверить его старт
        можно только на интерактивной сессии; на агентской машине такой сценарий
        либо зависнет, либо проверит ничего;
      * не проверяет сам выбор стартового набора (core::chooseStartupRuleSet) и
        разбор state.json — это политика ядра, она закрыта юнит-тестами
        tests\unit\rulesync_tests.cpp. Здесь проверяется наблюдаемый контракт
        настоящим бинарником: после отказа рабочий набор не изменился, проходит
        проверку целостности и принимается движком;
      * не требует прав администратора: наборы лежат в %TEMP%, ничего вне
        $TestDrive не пишется и не удаляется (SPEC §12 — приложение работает без
        повышения прав).

    Кодировка файла — UTF-8 С BOM, как у Test-FullCycle.ps1 и
    tools\sign-rules.ps1: Windows PowerShell 5.1 читает .ps1 без BOM в кодировке
    ANSI, и русский текст после этого ломает разбор самого файла вплоть до
    «неожиданный токен».

.PARAMETER CliPath
    Путь к mrproper_cli.exe. Пусто — искать в build\ (<слот>\<конфигурация>).

.PARAMETER Configuration
    Имя конфигурации для поиска сборки: Debug или Release (по умолчанию Debug).

.PARAMETER Slot
    Слот сборки build\<слот>\<конфигурация>. Пусто — искать во всех слотах и взять
    самую свежую сборку.

.PARAMETER RulesPath
    Каталог эталонного набора правил (по умолчанию rules\ в корне репозитория).

.PARAMETER KeepArtifacts
    Не снимать песочницу после прогона (по умолчанию она удаляется).

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-BadRules.ps1

.EXAMPLE
    Invoke-Pester -Script tests\e2e\Test-BadRules.ps1 -Slot a89
    # Pester 3.4 (тот, что стоит в Windows PowerShell 5.1) не знает -Parameters:
    # Invoke-Pester -Script @{ Path = 'tests\e2e\Test-BadRules.ps1';
    #                       Parameters = @{ Slot = 'a89' } }
#>
[CmdletBinding()]
param(
    [string] $CliPath = '',
    [string] $Configuration = 'Debug',
    [string] $Slot = '',
    [string] $RulesPath = '',
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
$script:RulesPath = if ($RulesPath -ne '') { $RulesPath } else { Join-Path $script:RepoRoot 'rules' }

# Артефакты сценария — в build\ (каталог в .gitignore), а не рядом с тестом:
# отчёт о пропусках должен пережить прогон и быть виден без запуска Pester.
$script:ArtifactsDir = Join-Path (Join-Path $script:RepoRoot 'build') 'e2e-artifacts'
try {
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
} catch {
    $script:ArtifactsDir = Join-Path ([IO.Path]::GetTempPath()) 'mrproper-e2e-artifacts'
    New-Item -ItemType Directory -Path $script:ArtifactsDir -Force | Out-Null
}
# Свой файл пропусков: сценарий полного цикла пишет в skipped.log, и два
# одновременных прогона не должны затирать записи друг друга.
$script:SkipLog = Join-Path $script:ArtifactsDir 'badrules-skipped.log'
if (Test-Path -LiteralPath $script:SkipLog) { Remove-Item -LiteralPath $script:SkipLog -Force }

# Состояние прогона (см. Test-FullCycle.ps1: переменные уровня $script:, а не
# локальные — Pester 3.4 и 5 по-разному исполняют замыкания).
$script:LogCounter = 0
$script:Cli = $null
$script:CliMissingReason = ''
$script:LayRoot = ''          # %LOCALAPPDATA%\MrProper\rules целиком
$script:CurrentPath = ''      # рабочий набор
$script:PreviousPath = ''     # предыдущий рабочий набор
$script:StagingPath = ''      # кандидат на проверку
$script:ReferenceOk = $false  # эталонный набор прошёл проверку целостности
$script:ReferenceProblem = ''  # почему нет (иначе отказы ниже ничего не значат)
$script:ReferenceFingerprint = ''

if ($null -eq (Get-Command -Name 'Describe' -ErrorAction SilentlyContinue)) {
    throw ('Pester не загружен: запускайте файл через Invoke-Pester, например ' +
        '`Invoke-Pester -Script tests\e2e\Test-BadRules.ps1`. Прямой запуск через ' +
        'powershell.exe ничего не проверяет: Describe в этом случае не команда PowerShell.')
}

# ---------------------------------------------------------------------------
# Сценарии «битого кандидата»
# ---------------------------------------------------------------------------
#
# Kind — что сломано, VerifyMode — как именно запускается проверка этого
# случая ('no-signature' — шаг подписи сознательно снят, 'signature-required' —
# подпись обязательна, 'signature-key' — подпись обязательна и ключ подан),
# Expect — код возврата, который обязан получиться (cmd_rules.cpp: 65 — данные не
# годятся, 69 — проверка невозможна, то есть подпись проверять нечем), File — имя
# файла, которое должно быть названо в отказе (пусто — файл не обязателен в
# тексте), Why — почему именно так (§9.2, шаги с 1 по 5).
#
# Список идёт по порядку цепочки §9.2 п.2-3: сначала манифест, потом версии,
# потом файлы, потом парсер правил. Всё, что не перечислено здесь, не отвергается
# этим сценарием, — утверждение «ни один битый набор не применяется» относится
# именно к этому перечню, и расширять его надо новой строкой, а не новой логикой.
$script:Cases = @(
    [ordered]@{
        Kind       = 'broken-manifest'
        Title      = 'битый JSON манифеста (шаг 1, схема)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'manifest.json'
        Why        = 'манифест не разбирается — применяться нечему'
    },
    [ordered]@{
        Kind       = 'manifest-unknown-field'
        Title      = 'неизвестное поле манифеста (шаг 1, строгий разбор)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'manifest.json'
        Why        = 'неизвестное поле — ошибка, а не предупреждение (§9.2 п.3)'
    },
    [ordered]@{
        Kind       = 'manifest-schema-2'
        Title      = 'схема манифеста 2 (шаг 1, поддерживается только 1)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'manifest.json'
        Why        = 'набор будущей схемы читать нельзя'
    },
    [ordered]@{
        Kind       = 'min-app-version'
        Title      = 'minAppVersion выше версии приложения (шаг 2)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = ''
        Why        = 'набор для другой версии программы'
    },
    [ordered]@{
        Kind       = 'signature-required'
        Title      = 'набор без подписи, шаг подписи обязателен (шаг 3)'
        VerifyMode = 'signature-required'
        Expect     = 65
        File       = 'rules.sig'
        Why        = 'подписи нет — подписывает владелец ключа, а не автор'
    },
    [ordered]@{
        Kind       = 'signature-unverifiable'
        Title      = 'подпись и ключ на месте, верификатора нет (шаг 3)'
        VerifyMode = 'signature-key'
        Expect     = 69
        File       = ''
        Why        = '«проверить подпись нечем» и «подпись верна» — разные утверждения'
    },
    [ordered]@{
        Kind       = 'size-mismatch'
        Title      = 'файл правил длиннее, чем в манифесте (шаг 4, размер)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'temp.user.json'
        Why        = 'содержимое не то, что подписано'
    },
    [ordered]@{
        Kind       = 'hash-mismatch'
        Title      = 'подмена байта при том же размере (шаг 4, SHA-256)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'temp.user.json'
        Why        = 'размер совпал, хеш нет: единственный тест против подмены'
    },
    [ordered]@{
        Kind       = 'missing-file'
        Title      = 'файл из манифеста отсутствует (шаг 4, комплектность)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'temp.user.json'
        Why        = 'набор неполон, половина правил была бы без файла'
    },
    [ordered]@{
        Kind       = 'unlisted-file'
        Title      = 'файл правил не перечислен в манифесте (шаг 4, requireKnownFilesOnly)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'evil.extra.json'
        Why        = 'правила вне манифеста никто не подписывал, а удалять будут по ним'
    },
    [ordered]@{
        Kind       = 'rule-broken-json'
        Title      = 'битый JSON правила при верном хеше (шаг 5, парсер)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'temp.user.json'
        Why        = 'целостность сошлась, читать набор всё равно нечем'
    },
    [ordered]@{
        Kind       = 'rule-unknown-field'
        Title      = 'неизвестное поле правила при верном хеше (шаг 5, парсер)'
        VerifyMode = 'no-signature'
        Expect     = 65
        File       = 'temp.user.json'
        Why        = 'после проверки целостности неизвестное поле — ошибка (§9.2 п.3)'
    }
)

# ---------------------------------------------------------------------------
# Вспомогательные функции
# ---------------------------------------------------------------------------

function Get-MrProperCli {
    <#
    .SYNOPSIS Найти собранный mrproper_cli.exe.
    .DESCRIPTION Порядок поиска: -CliPath, $env:MRPROPER_CLI, точные пути слота и
    конфигурации, затем все слоты поимённо. Логика скопирована из
    Test-FullCycle.ps1: оба сценария ищут одну и ту же сборку, и расхождение в
    порядке поиска означало бы, что они проверяют разные бинарники.
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
    пробелом (путь к каталогу набора) должен прийти в кавычках.
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
    stderr и stdout разведены по файлам намеренно (§6.2: машина — в stdout,
    человек — в stderr), иначе тест проверял бы их смесь.
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
    массив обратно в конвейер, и вызывающий получает скаляр.
    #>
    param($Value)

    if ($null -eq $Value) { return , @() }
    return , @($Value)
}

function Write-TextFileNoBom {
    <#
    .SYNOPSIS Записать текст БЕЗ BOM.
    .DESCRIPTION Разбор JSON в mrproper-cli BOM не снимает, а Set-Content
    -Encoding UTF8 в Windows PowerShell 5.1 BOM пишет. Поэтому байты файла
    собираются вручную.
    #>
    param([string] $Path, [string] $Text)

    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-Sha256Lower {
    <#
    .SYNOPSIS SHA-256 файла строчными шестнадцатеричными, как в манифесте.
    #>
    param([string] $Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-SetFingerprint {
    <#
    .SYNOPSIS Снимок набора правил: «путь=размер=SHA-256» по каждому файлу.
    .DESCRIPTION Сравнение снимков — единственный способ отличить «рабочий набор
    не тронут» от «рабочий набор перезаписан тем же содержимым»: набор — это
    десятки файлов, и молчаливая подмена одного байта в правиле удаления здесь и
    есть тот случай, ради которого набор вообще подписывается (ADR-008).
    Возвращается одна строка, а не массив: Pester 3.4 сравнивает коллекции
    поэлементно, и внятное сообщение об отказе важнее формы сравнения.
    #>
    param([string] $SetPath)

    if (-not (Test-Path -LiteralPath $SetPath -PathType Container)) {
        return '(каталога нет)'
    }
    $prefix = (Resolve-Path -LiteralPath $SetPath).ProviderPath.TrimEnd('\') + '\'
    $lines = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $SetPath -Recurse -File | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        $lines += ($relative + '=' + $file.Length + '=' + (Get-Sha256Lower $file.FullName))
    }
    if ($lines.Count -eq 0) { return '(пусто)' }
    return ($lines -join "`n")
}

function Get-FingerprintDifference {
    <#
    .SYNOPSIS Первая различающаяся строка двух снимков набора (пусто — снимки равны).
    .DESCRIPTION Сообщение об отказе должно называть файл, а не «наборы разные»:
    по имени файла видно, кандидат это был применён или сломанная копия.
    #>
    param([string] $Before, [string] $After)

    $left = @($Before -split "`n")
    $right = @($After -split "`n")
    for ($index = 0; $index -lt [Math]::Max($left.Count, $right.Count); $index++) {
        $one = if ($index -lt $left.Count) { $left[$index] } else { '(нет строки)' }
        $two = if ($index -lt $right.Count) { $right[$index] } else { '(нет строки)' }
        if ($one -ne $two) { return ('строка ' + $index + ': было "' + $one + '", стало "' + $two + '"') }
    }
    return ''
}

function Copy-RuleSet {
    <#
    .SYNOPSIS Скопировать набор правил целиком (файлы верхнего уровня).
    #>
    param([string] $Source, [string] $Destination)

    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -Path (Join-Path $Source '*') -Destination $Destination -Recurse -Force
    return $Destination
}

function Get-ManifestFileNames {
    <#
    .SYNOPSIS Имена файлов правил набора: все *.json, кроме самого манифеста.
    .DESCRIPTION Порядок — по имени, как в cmd_rules.cpp: вывод проверки должен
    быть детерминированным (§11.4, golden-тесты).
    #>
    param([string] $SetPath)

    $names = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $SetPath -Filter '*.json' -File | Sort-Object Name)) {
        if ($file.Name -ne 'manifest.json') { $names += $file.Name }
    }
    return , $names
}

function New-ManifestText {
    <#
    .SYNOPSIS Собрать манифест заново под текущее содержимое набора.
    .DESCRIPTION Нужен кандидатам, которые обязаны дойти до шага 5 (парсера
    правил): если оставить хеш прежним, отказ придётся на шаге 4 и до парсера
    дело не дойдёт — а проверить надо именно его. Формат — §9.2: schemaVersion,
    version, minAppVersion, files[{path, sha256, size}].
    #>
    param(
        [string] $SetPath,
        [string] $Version = '2026.09.2',
        [string] $MinAppVersion = '0.1.0',
        [int] $SchemaVersion = 1,
        [string] $SchemaKey = 'schemaVersion'
    )

    $files = @()
    foreach ($name in (Get-ManifestFileNames $SetPath)) {
        $full = Join-Path $SetPath $name
        $files += , ([ordered]@{
                path   = $name
                sha256 = (Get-Sha256Lower $full)
                size   = (Get-Item -LiteralPath $full).Length
            })
    }
    $document = [ordered]@{}
    $document[$SchemaKey] = $SchemaVersion
    $document['version'] = $Version
    $document['minAppVersion'] = $MinAppVersion
    $document['files'] = $files
    return ($document | ConvertTo-Json -Depth 8)
}

function New-BrokenCandidate {
    <#
    .SYNOPSIS Положить в staging битый кандидат и вернуть, что сломал.
    .DESCRIPTION Каждая поломка точечная: кандидат отличается от рабочего набора
    ровно одним способом, иначе отказ нечего предъявить — при двух поломках
    нельзя утверждать, что отказ вызван нужной. Случай 'ok' готовит честную
    копию рабочего набора: она обязана проходить проверку, иначе «отказы» ниже
    ничего не отличают от «всё подряд отвергается».
    #>
    param([string] $Staging, [string] $Working, [string] $Kind, [string] $ManifestName = 'manifest.json')

    $candidate = Copy-RuleSet -Source $Working -Destination $Staging
    $manifestPath = Join-Path $candidate $ManifestName
    $note = 'кандидат не сломан (контрольный случай)'

    switch ($Kind) {
        'ok' {
            $note = 'честная копия рабочего набора'
        }
        'broken-manifest' {
            # Обрезанный JSON: манифест не доходит до конца. Кавычка внутри строки
            # в here-string не экранируется — это ровно тот текст, который
            # получает парсер манифеста.
            $truncated = @'
{"schemaVersion": 1, "version": "2026.09.2", "files": [
'@
            Write-TextFileNoBom -Path $manifestPath -Text $truncated
            $note = 'манифест обрезан на середине'
        }
        'manifest-unknown-field' {
            $text = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
            $text = $text -replace '(\{\s*"schemaVersion")', '{ "surpriseField": true, "schemaVersion"'
            Write-TextFileNoBom -Path $manifestPath -Text $text
            $note = 'в манифест добавлено неизвестное поле surpriseField'
        }
        'manifest-schema-2' {
            $text = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
            $text = $text -replace '"schemaVersion"\s*:\s*1', '"schemaVersion": 2'
            Write-TextFileNoBom -Path $manifestPath -Text $text
            $note = 'схема манифеста поднята до 2'
        }
        'min-app-version' {
            Write-TextFileNoBom -Path $manifestPath -Text (New-ManifestText -SetPath $candidate -MinAppVersion '99.0.0')
            $note = 'minAppVersion 99.0.0 при версии приложения 0.1.0'
        }
        'signature-required' {
            $signature = Join-Path $candidate 'rules.sig'
            if (Test-Path -LiteralPath $signature) { Remove-Item -LiteralPath $signature -Force }
            $note = 'подпись удалена, шаг подписи обязателен'
        }
        'signature-unverifiable' {
            # Подпись (64 байта) и публичный ключ (32 байта) в base64 — валидный
            # вид, но проверять их нечем: Ed25519-верификатор в сборку не внедрён.
            # Ожидаемый код — 69 (проверка невозможна), а не 0.
            $signature = Join-Path $candidate 'rules.sig'
            $key = Join-Path $candidate 'rules.pub'
            Write-TextFileNoBom -Path $signature -Text ([Convert]::ToBase64String((New-Object byte[] 64)))
            Write-TextFileNoBom -Path $key -Text ([Convert]::ToBase64String([byte[]] @(1..32 | ForEach-Object { [byte] 7 })))
            $note = 'подпись и ключ на месте, Ed25519-верификатор не внедрён'
        }
        'size-mismatch' {
            $victim = Join-Path $candidate 'temp.user.json'
            [IO.File]::AppendAllText($victim, "`n", (New-Object System.Text.UTF8Encoding($false)))
            $note = 'файл правил длиннее, чем в манифесте'
        }
        'hash-mismatch' {
            # Тот же размер, другие байты: единственный способ проверить, что
            # сверяется хеш, а не только длина.
            $victim = Join-Path $candidate 'temp.user.json'
            $bytes = [IO.File]::ReadAllBytes($victim)
            $bytes[0] = [byte] (($bytes[0] + 1) -band 0xFF)
            [IO.File]::WriteAllBytes($victim, $bytes)
            $note = 'первый байт файла правил подменён, размер сохранён'
        }
        'missing-file' {
            Remove-Item -LiteralPath (Join-Path $candidate 'temp.user.json') -Force
            $note = 'файл правил из манифеста удалён'
        }
        'unlisted-file' {
            # Файл правил, которого нет в манифесте: его никто не подписывал, а
            # удалять по нему будут (ADR-008, requireKnownFilesOnly).
            $extra = @'
{
  "schemaVersion": 1,
  "version": "2026.09.2",
  "rules": [
    {
      "id": "evil.extra",
      "category": "temp.user",
      "safety": "safe",
      "locator": "%USERPROFILE%\\Documents\\**",
      "minAgeDays": 1
    }
  ]
}
'@
            Write-TextFileNoBom -Path (Join-Path $candidate 'evil.extra.json') -Text $extra
            $note = 'добавлено правило evil.extra.json, не перечисленное в манифесте'
        }
        'rule-broken-json' {
            $victim = Join-Path $candidate 'temp.user.json'
            $truncated = @'
{
  "schemaVersion": 1,
  "rules": [
'@
            Write-TextFileNoBom -Path $victim -Text $truncated
            Write-TextFileNoBom -Path $manifestPath -Text (New-ManifestText -SetPath $candidate)
            $note = 'JSON правила обрезан, хеш в манифесте пересчитан (падает шаг 5)'
        }
        'rule-unknown-field' {
            $victim = Join-Path $candidate 'temp.user.json'
            $text = Get-Content -LiteralPath $victim -Raw -Encoding UTF8
            $text = $text -replace '("schemaVersion"\s*:\s*1)', '$1, "surpriseField": true'
            Write-TextFileNoBom -Path $victim -Text $text
            Write-TextFileNoBom -Path $manifestPath -Text (New-ManifestText -SetPath $candidate)
            $note = 'в правило добавлено неизвестное поле, хеш пересчитан (падает шаг 5)'
        }
        default {
            throw ('неизвестный вид поломки: ' + $Kind)
        }
    }

    return [pscustomobject]@{ Path = $candidate; Note = $note }
}

function Invoke-RulesCheck {
    <#
    .SYNOPSIS Проверка набора настоящим бинарником: rules validate или verify.
    .DESCRIPTION Подпись по умолчанию снимается флагом --no-signature, иначе
    проверка подписи обязательна и без внедрённого Ed25519-верификатора вся
    цепочка обрывается кодом 69 ещё до сверки файлов. Отдельные случаи про
    подпись флаг не получают — им он нужен, чтобы дойти до шага 3.
    #>
    param(
        [string] $Cli,
        [string] $SetPath,
        [ValidateSet('validate', 'verify')]
        [string] $Command = 'verify',
        [switch] $NoSignature,
        [string[]] $Extra = @(),
        [string] $LogDirectory
    )

    $arguments = @('rules', $Command, '--set', $SetPath, '--json', '--quiet')
    if ($NoSignature) { $arguments += '--no-signature' }
    if ($null -ne $Extra -and $Extra.Count -gt 0) { $arguments += $Extra }
    return Invoke-MrProperCli -Cli $Cli -Arguments $arguments -LogDirectory $LogDirectory
}

function Get-JsonDocument {
    <#
    .SYNOPSIS Разобрать stdout команды. Пустой stdout — ошибка с текстом stderr.
    #>
    param($Result, [string] $Where)

    if ($Result.Out -eq '') {
        throw ($Where + ': stdout пуст, команда ничего не напечатала; stderr: ' +
            (Get-FirstMeaningfulLine $Result.Err))
    }
    try {
        return ($Result.Out | ConvertFrom-Json)
    } catch {
        throw ($Where + ': stdout не JSON (' + $_.Exception.Message + '): ' + $Result.Out)
    }
}

function Get-Problems {
    <#
    .SYNOPSIS Массив причин отказа из поля problems отчёта проверки.
    .DESCRIPTION Пустой массив в JSON — это $null, а не @(): без этой функции
    под Set-StrictMode обращение к .Count отсутствующего поля упало бы с
    PropertyNotFoundException вместо внятного «причин нет».
    #>
    param($Document, [string] $Where)

    $value = Get-RequiredProperty $Document 'problems' $Where
    if ($null -eq $value) { return , @() }
    return , @($value)
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

function Invoke-CaseCheck {
    <#
    .SYNOPSIS Проверить кандидата так, как это делает приложение для этого случая.
    .DESCRIPTION Шаг подписи обязателен (SPEC §9.2 п.2), и снимать его флагом
    --no-signature можно не всегда: два случая существуют именно ради шага 3, и
    для них флаг не передаётся. Публичный ключ лежит рядом с кандидатом, его
    путь сюда же добавлен — иначе проверка остановилась бы на «ключ не задан».
    Флаг --no-signature и ключ не должны проставляться по случаю в двух местах
    сценария: здесь, один раз.
    #>
    param([string] $Cli, $Case, [string] $SetPath, [string] $LogDirectory)

    $noSignature = $true
    $extra = @()
    switch ([string] $Case.VerifyMode) {
        'no-signature' {
            # Шаг подписи сознательно снят: этот случай ломает не подпись.
            break
        }
        'signature-required' {
            $noSignature = $false
        }
        'signature-key' {
            $noSignature = $false
            $extra = @('--public-key', (Join-Path $SetPath 'rules.pub'))
        }
        default {
            throw ('неизвестный режим проверки подписи: ' + [string] $Case.VerifyMode)
        }
    }
    return Invoke-RulesCheck -Cli $Cli -SetPath $SetPath -Command 'verify' `
        -NoSignature:$noSignature -Extra $extra -LogDirectory $LogDirectory
}

function Assert-VerificationAccepted {
    <#
    .SYNOPSIS Набор принят: код 0, ok=true, все файлы сошлись, правила разобраны.
    .DESCRIPTION Ничего не возвращает и разобранный отчёт принимает параметром.
    Так сделано намеренно: Should в Pester 3.4 кладёт в поток вывода функции
    свои значения, и функция, которая возвращает отчёт после проверок
    Should, отдала бы вызывающему массив из отчёта и мусора (проверено:
    отчёт терялся, а тест падал с «нет поля version»).
    #>
    param($Result, $Document, [string] $Where, [int] $ExpectedFiles = 0, [int] $ExpectedRules = 0)

    $Result.Exit | Should Be 0
    (Get-RequiredProperty $Document 'ok' $Where) | Should Be $true
    (Get-RequiredProperty $Document 'exitCode' $Where) | Should Be 0
    (Get-RequiredProperty $Document 'filesOk' $Where) | Should Be (Get-RequiredProperty $Document 'fileCount' $Where)
    (Get-RequiredProperty $Document 'problems' $Where) | Should BeNullOrEmpty
    $rules = [int] (Get-RequiredProperty $Document 'ruleCount' $Where)
    $rules | Should BeGreaterThan $ExpectedRules
    if ($ExpectedFiles -gt 0) {
        (Get-RequiredProperty $Document 'fileCount' $Where) | Should Be $ExpectedFiles
    }
}

function Assert-VerificationRefused {
    <#
    .SYNOPSIS Набор отвергнут: ожидаемый код, ok=false, есть причина в problems,
    .DESCRIPTION Проверяются четыре вещи, и каждая отвечает на свой вопрос.
      * код возврата — тот, который задокументирован в cmd_rules.cpp для этого
        вида поломки (65 — данные не годятся, 69 — проверка невозможна), и
        машинный exitCode в JSON совпадает с кодом процесса: «тихий отказ»
        §9.2 п.4 не должен выглядеть как успех ни в одном из потоков;
      * ok=false в корне машинного вывода. Подстроку в stdout не ищем: в отчёте
        есть вложенный объект signature со своим полем ok, и при отказе по данным
        он остаётся истинным — искать «"ok": true» в тексте значило бы ловить
        чужое поле;
      * problems непуст — отказ обязан объяснить себя, иначе в логе нечего искать;
      * имя файла названо в отказе — иначе «набор плохой» без указания, какой
        файл плохой, и разбираться придётся вручную.
    #>
    param($Result, $Document, $Case, [string] $Where)

    $Result.Exit | Should Be ([int] $Case.Expect)
    (Get-RequiredProperty $Document 'ok' $Where) | Should Be $false
    (Get-RequiredProperty $Document 'exitCode' $Where) | Should Be ([int] $Case.Expect)

    $problems = Get-Problems -Document $Document -Where $Where
    ($problems.Count -gt 0) | Should Be $true
    ($Result.Err -match 'ОШИБКА') | Should Be $true

    if ([string] $Case.File -ne '') {
        $joined = ($problems -join ' | ')
        $joined.Contains([string] $Case.File) | Should Be $true
    }
}

function Assert-WorkingSetUntouched {
    <#
    .SYNOPSIS Рабочий набор не изменился: тот же снимок, то же число файлов.
    #>
    param([string] $Before, [string] $After, [string] $Where)

    $difference = Get-FingerprintDifference -Before $Before -After $After
    if ($difference -ne '') {
        throw ($Where + ': рабочий набор изменился после отказа кандидата — ' + $difference)
    }
    ($After -split "`n").Count | Should Be ($Before -split "`n").Count
}

function Add-SkippedStep {
    <#
    .SYNOPSIS Отметить шаг пропущенным с точной причиной.
    .DESCRIPTION Причина обязательна: «пропущено» без причины — замаскированный
    дефект. Pester 5 умеет Set-ItResult -Skipped; в Pester 3.4 такой команды
    нет, и тогда остаются строка SKIPPED в выводе и запись в skipped.log —
    сводка её покажет.
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

function Test-CasePrerequisites {
    <#
    .SYNOPSIS Готов ли сценарий к проверке отказа; иначе — причина пропуска.
    .DESCRIPTION Причин ровно три, и каждая названа своим текстом: нет бинарника,
    нет набора правил в репозитории, эталонный набор сам не проходит проверку.
    Третья — самая важная: если эталон битый, «отказы» ниже не отличают ничего от
    «эта сборка всё отвергает», и такой прогон молча ничего не проверяет.
    #>
    if ($null -eq $script:Cli) { return $script:CliMissingReason }
    if (-not (Test-Path -LiteralPath $script:RulesPath -PathType Container)) {
        return ('набор правил не найден: ' + $script:RulesPath)
    }
    if (-not $script:ReferenceOk) { return $script:ReferenceProblem }
    return ''
}

# ---------------------------------------------------------------------------
# Сценарий
# ---------------------------------------------------------------------------

Describe 'MrProper: битые правила — старт на предыдущем рабочем наборе (SPEC §9.2, ADR-008)' -Tag 'e2e' {

    BeforeEach {
        $script:Cli = Get-MrProperCli -Requested $CliPath -Configuration $Configuration -Slot $Slot
        if ($null -eq $script:Cli) {
            $script:CliMissingReason = ('не найден mrproper_cli.exe (соберите tools\build.bat ' +
                $Configuration + ' <слот> или укажите -CliPath)')
        } else {
            $script:CliMissingReason = ''
        }

        # Раскладка ровно как в приложении: current/previous/staging.
        $script:LayRoot = Join-Path (Get-SandboxRoot) ('badrules-' + $PID)
        if (Test-Path -LiteralPath $script:LayRoot) {
            Remove-Item -LiteralPath $script:LayRoot -Recurse -Force
        }
        New-Item -ItemType Directory -Path $script:LayRoot -Force | Out-Null
        $script:CurrentPath = Join-Path $script:LayRoot 'current'
        $script:PreviousPath = Join-Path $script:LayRoot 'previous'
        $script:StagingPath = Join-Path $script:LayRoot 'staging'

        $script:ReferenceOk = $false
        $script:ReferenceProblem = ''
        $script:ReferenceFingerprint = ''

        if ($null -eq $script:Cli -or -not (Test-Path -LiteralPath $script:RulesPath -PathType Container)) { return }

        # Настоящий набор из репозитория — то, на чём приложение работает.
        # Копия нужна и потому, что правила в репозитории правят параллельно:
        # сценарий обязан судить о битых кандидатах, а не о чужой незавершённой
        # правке манифеста.
        $null = Copy-RuleSet -Source $script:RulesPath -Destination $script:CurrentPath
        $null = Copy-RuleSet -Source $script:RulesPath -Destination $script:PreviousPath

        $reference = Invoke-RulesCheck -Cli $script:Cli -SetPath $script:CurrentPath `
            -Command 'verify' -NoSignature -LogDirectory $script:LayRoot
        if ($reference.Exit -ne 0) {
            $document = $null
            try { $document = Get-JsonDocument -Result $reference -Where 'эталон' } catch { $document = $null }
            $detail = (Get-FirstMeaningfulLine $reference.Err)
            if ($null -ne $document) {
                $problems = Get-Problems -Document $document -Where 'эталон'
                if ($problems.Count -gt 0) { $detail = $problems -join ' | ' }
            }
            $script:ReferenceProblem = ('эталонный набор ' + $script:CurrentPath +
                ' сам не проходит проверку целостности (код ' + $reference.Exit + '): ' + $detail +
                ' — кандидаты не проверяются, иначе «отказ» ничего не значил бы')
            return
        }
        $script:ReferenceOk = $true
        $script:ReferenceFingerprint = Get-SetFingerprint $script:CurrentPath
    }

    AfterEach {
        if ($KeepArtifacts) {
            Write-Host ('песочница оставлена: ' + $script:LayRoot)
        } elseif (Test-Path -LiteralPath $script:LayRoot) {
            Remove-Item -LiteralPath $script:LayRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Context 'шаг 0: эталонный набор' {

        It 'рабочий набор принимается целиком: шаги 1-5 §9.2, все файлы и правила' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            if (-not (Test-Path -LiteralPath $script:RulesPath -PathType Container)) {
                Add-SkippedStep ('набор правил не найден: ' + $script:RulesPath); return
            }
            # Без этого утверждения «отказы» ниже не значат ничего: набор, который
            # не проходит проверку, был бы отвергнут в любом случае.
            $script:ReferenceProblem | Should BeNullOrEmpty

            $verifyCurrent = Invoke-RulesCheck -Cli $script:Cli -SetPath $script:CurrentPath `
                -Command 'verify' -NoSignature -LogDirectory $script:LayRoot
            $document = Get-JsonDocument -Result $verifyCurrent -Where 'verify current'
            Assert-VerificationAccepted -Result $verifyCurrent -Document $document -Where 'verify current'
            ([string] (Get-RequiredProperty $document 'version' 'verify current')) | Should Not BeNullOrEmpty
            # Подпись не проверялась, и об этом сказано в warnings: без флага
            # --no-signature результат пригоден для приёмки набора в CI.
            $warnings = ConvertTo-CountArray (Get-RequiredProperty $document 'warnings' 'verify current')
            ($warnings -join ' ').Contains('--no-signature') | Should Be $true
            (Get-RequiredProperty $document 'signature.checked' 'verify current') | Should Be $false

            # validate — независимая точка входа (автор набора вместо CI): тот же
            # набор обязан приниматься и ею, иначе «отказы» ниже могут быть
            # свойством одной команды.
            $validateResult = Invoke-RulesCheck -Cli $script:Cli -SetPath $script:CurrentPath `
                -Command 'validate' -NoSignature -LogDirectory $script:LayRoot
            $validate = Get-JsonDocument -Result $validateResult -Where 'validate current'
            Assert-VerificationAccepted -Result $validateResult -Document $validate -Where 'validate current'
            (Get-RequiredProperty $validate 'command' 'validate current') | Should Be 'validate'

            # Предыдущий рабочий набор — цель отката (§9.2), он тоже должен быть
            # целым: откатываться на битый набор бессмысленно.
            $verifyPrevious = Invoke-RulesCheck -Cli $script:Cli -SetPath $script:PreviousPath `
                -Command 'verify' -NoSignature -LogDirectory $script:LayRoot
            $previousDocument = Get-JsonDocument -Result $verifyPrevious -Where 'verify previous'
            Assert-VerificationAccepted -Result $verifyPrevious -Document $previousDocument -Where 'verify previous'

            # Снимок рабочего набора: дальше он сравнивается после каждой попытки
            # обновления. Подпись копируется сюда один раз, чтобы пустой каталог
            # не прошёл проверку «успехом».
            $script:ReferenceFingerprint | Should Not BeNullOrEmpty
            $script:ReferenceFingerprint.Contains('temp.user.json=') | Should Be $true
        }

        It 'scan читает рабочий набор: приложение стартует не без правил' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $blocker = Test-CasePrerequisites
            if ($blocker -ne '') { Add-SkippedStep $blocker; return }

            $scan = Invoke-MrProperCli -Cli $script:Cli -LogDirectory $script:LayRoot `
                -Arguments @('scan', '--json', '--rules', $script:CurrentPath, '--quiet')

            # Коды scan (src/cli/cmd_scan.cpp): 0 — отчёт, 3 — набор правил
            # недоступен, 4 — прогон не дал результата. Ничего другого команда
            # возвращать не обещает, и такой код — уже дефект, а не пропуск.
            if ($scan.Exit -ne 0 -and $scan.Exit -ne 3 -and $scan.Exit -ne 4) {
                throw ('scan вернул недокументированный код ' + $scan.Exit + ': ' +
                    (Get-FirstMeaningfulLine $scan.Err))
            }
            if ($scan.Exit -ne 0) {
                Add-SkippedStep ('scan: код ' + $scan.Exit + ' — ' + (Get-FirstMeaningfulLine $scan.Err))
                return
            }
            $report = Get-JsonDocument -Result $scan -Where 'scan'
            (Get-RequiredProperty $report 'kind' 'scan') | Should Be 'scan'
            (Get-RequiredProperty $report 'schema' 'scan') | Should Be 1
        }
    }

    Context 'шаг 1: отказ на каждом шаге цепочки §9.2' {

        # Один It на вид поломки, а не один It на весь список: имя теста в отчёте
        # Pester — это первое, куда смотрят, когда прогон красный, и «битый набор
        # отвергнут» без указания шага бесполезно.
        foreach ($case in $script:Cases) {
            It ($case.Title + ' — отказ, рабочий набор не тронут') {
                if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
                $blocker = Test-CasePrerequisites
                if ($blocker -ne '') { Add-SkippedStep $blocker; return }

                $before = $script:ReferenceFingerprint
                $candidate = New-BrokenCandidate -Staging $script:StagingPath `
                    -Working $script:CurrentPath -Kind $case.Kind
                # Причина поломки попадает в имя проверки: сообщение об отказе
                # должно объяснять, что именно проверялось, а не только где.
                $where = 'verify кандидата (' + $case.Kind + '): ' + $case.Why

                # Кандидат проверяется там же, где проверялся бы настоящий:
                # в staging, а не в current. Ни одна из этих команд не пишет
                # в рабочий набор — и следующая строка это проверяет.
                $verify = Invoke-CaseCheck -Cli $script:Cli -Case $case -SetPath $candidate.Path `
                    -LogDirectory $script:LayRoot
                $candidateDocument = Get-JsonDocument -Result $verify -Where $where
                Assert-VerificationRefused -Result $verify -Document $candidateDocument -Case $case -Where $where

                # Главное утверждение сценария: отказ не тронул рабочий набор.
                Assert-WorkingSetUntouched -Before $before `
                    -After (Get-SetFingerprint $script:CurrentPath) -Where $where

                # Кандидат остался в staging отдельным каталогом: половина
                # обновления в current не попала.
                (Get-SetFingerprint $script:CurrentPath).Contains('evil.extra.json=') | Should Be $false
            }
        }
    }

    Context 'шаг 2: серия отказов подряд — приложение не осталось без набора правил' {

        It 'двенадцать битых кандидатов один за другим: current байт в байт прежний, оба набора целы' {
            if ($null -eq $script:Cli) { Add-SkippedStep $script:CliMissingReason; return }
            $blocker = Test-CasePrerequisites
            if ($blocker -ne '') { Add-SkippedStep $blocker; return }

            $before = $script:ReferenceFingerprint
            $refused = @()
            foreach ($case in $script:Cases) {
                $candidate = New-BrokenCandidate -Staging $script:StagingPath `
                    -Working $script:CurrentPath -Kind $case.Kind
                $where = 'серия отказов, кандидат ' + $case.Kind + ': ' + $case.Why
                $verify = Invoke-CaseCheck -Cli $script:Cli -Case $case -SetPath $candidate.Path `
                    -LogDirectory $script:LayRoot
                $candidateDocument = Get-JsonDocument -Result $verify -Where $where
                Assert-VerificationRefused -Result $verify -Document $candidateDocument -Case $case -Where $where
                Assert-WorkingSetUntouched -Before $before `
                    -After (Get-SetFingerprint $script:CurrentPath) -Where $where
                $refused += $case.Kind
            }
            $refused.Count | Should Be $script:Cases.Count

            # Вторая точка входа (rules validate — автор набора вместо CI) обязана
            # вести себя так же: набор, отвергнутый путём обновления, не должен
            # вдруг пройти через validate. Проверяется на последнем кандидате
            # серии, у которого биты и JSON правила, и его содержимое.
            $last = Invoke-RulesCheck -Cli $script:Cli -SetPath $candidate.Path -Command 'validate' `
                -NoSignature -LogDirectory $script:LayRoot
            $lastDocument = Get-JsonDocument -Result $last -Where 'validate последнего кандидата'
            Assert-VerificationRefused -Result $last -Document $lastDocument -Case $case `
                -Where ('validate последнего кандидата (' + $case.Kind + ')')

            # А теперь честная копия рабочего набора в том же staging: она
            # обязана пройти. Иначе «отказы» выше можно было бы получить и на
            # заведомо годном наборе — то есть проверялось бы не то.
            $good = New-BrokenCandidate -Staging $script:StagingPath -Working $script:CurrentPath -Kind 'ok'
            $accepted = Invoke-RulesCheck -Cli $script:Cli -SetPath $good.Path -Command 'verify' `
                -NoSignature -LogDirectory $script:LayRoot
            $goodDocument = Get-JsonDocument -Result $accepted -Where 'честный кандидат'
            Assert-VerificationAccepted -Result $accepted -Document $goodDocument -Where 'честный кандидат'

            # Финал: и рабочий, и предыдущий наборы живы, правила в них есть и
            # совпадают — откатываться было бы на тот же самый набор.
            $currentResult = Invoke-RulesCheck -Cli $script:Cli -SetPath $script:CurrentPath -Command 'verify' `
                -NoSignature -LogDirectory $script:LayRoot
            $current = Get-JsonDocument -Result $currentResult -Where 'после серии отказов: current'
            Assert-VerificationAccepted -Result $currentResult -Document $current -Where 'после серии отказов: current'
            $previousResult = Invoke-RulesCheck -Cli $script:Cli -SetPath $script:PreviousPath -Command 'verify' `
                -NoSignature -LogDirectory $script:LayRoot
            $previous = Get-JsonDocument -Result $previousResult -Where 'после серии отказов: previous'
            Assert-VerificationAccepted -Result $previousResult -Document $previous -Where 'после серии отказов: previous'
            (Get-RequiredProperty $current 'version' 'после серии отказов: current') |
                Should Be (Get-RequiredProperty $previous 'version' 'после серии отказов: previous')
            (Get-RequiredProperty $current 'ruleCount' 'после серии отказов: current') |
                Should Be (Get-RequiredProperty $previous 'ruleCount' 'после серии отказов: previous')

            Assert-WorkingSetUntouched -Before $before `
                -After (Get-SetFingerprint $script:CurrentPath) -Where 'после серии отказов'
        }
    }

    Context 'шаг 3: сводка' {

        It 'каждый пропуск записан с причиной' {
            # Сводка последним шагом, как в Test-FullCycle.ps1: «зелёный» Pester
            # не значит «сценарий пройден», если часть шагов пропущена. Пропусков
            # может не быть вовсе — тогда это тоже результат, и печатается он
            # прямо, а не молча.
            $entries = @()
            if (Test-Path -LiteralPath $script:SkipLog -PathType Leaf) {
                $entries = @(Get-Content -LiteralPath $script:SkipLog -Encoding UTF8 |
                        Where-Object { $_.Trim() -ne '' })
            }
            foreach ($entry in $entries) { $entry | Should Match '^SKIPPED: .{10,}$' }
            if ($entries.Count -eq 0) {
                Write-Host 'пропусков нет: весь сценарий выполнен на этой сборке'
            } else {
                Write-Host ('пропущено шагов: ' + $entries.Count + ' (см. ' + $script:SkipLog + ')')
                foreach ($entry in $entries) { Write-Host ('  ' + $entry) }
            }
        }
    }
}
