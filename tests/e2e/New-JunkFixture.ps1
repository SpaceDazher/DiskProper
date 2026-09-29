#Requires -Version 5.1
<#
.SYNOPSIS
    MrProper: генератор синтетического мусора для e2e-сценариев (SPEC §11.3).

.DESCRIPTION
    E2E-слой — это PowerShell-сценарии на VM-образах (SPEC §11.3, п. 3), и
    половина из двадцати сценариев упирается в один и тот же вопрос: откуда на
    машине взять «мусор» нужного объёма и нужных категорий. Готовить его руками
    нельзя: Этап 2 требует, чтобы `mrproper-cli scan --json` вернул все шесть
    категорий, а сумма allocatedBytes отличалась от эталона не более чем на
    0.5 % (SPEC §8) — ручная подготовка этой цифры не даёт, и на VM с
    переустановленными образами она каждый раз разная.

    Скрипт делает четыре вещи:
      * New      — создаёт синтетический мусор: файлы заданного объёма с
                   правдоподобными именами и расширениями, разложенные по тем же
                   относительным путям, что и настоящие локаторы правил
                   (SPEC §4 FR-3), и — что важнее — с возрастом, разложенным по
                   порогам minAgeDays каждого правила: часть файлов «старая»
                   (обязана попасть в кандидаты), часть «свежая» (не должна).
                   Рядом пишется fixture-manifest.json: сколько файлов и байт
                   положено, сколько из них переживёт возрастной фильтр, какой
                     набор правил и версия были эталоном. Сценарий сверяет с
                   манифестом вывод скана, и расхождение видно сразу.
      * List     — печатает каталог категорий: id правила, уровень риска, порог
                   возраста, настоящий локатор и то, что фикстура кладёт вместо
                   него. Это источник для -Category и шпаргалка авторам сценариев.
      * Remove   — сносит созданное дерево, но только то, которое этот скрипт
                   и создал (проверяется по манифесту) — иначе -Root с опечаткой
                   съел бы чужой каталог.
      * SelfTest — проверяет сам скрипт: разбирает его парсером PowerShell
                   (ошибок быть не должно — это критерий приёмки задачи),
                   затем создаёт маленькую фикстуру в %TEMP%, сверяет её с
                   манифестом по байтам и возрасту и убирает за собой.

    Две раскладки, и разница между ними принципиальная:
      * Mirror (по умолчанию) — всё дерево под -Root, с зеркалом системных
        путей: user\LocalAppData, user\Profile, system\Windows, system\
        ProgramData. Система не затрагивается, фикстуру можно гонять на
        рабочей машине. Сканер такому дереву не видно: локаторы правил заданы
        литералами, и без подмены набора он ищет настоящие каталоги.
      * Real — файлы кладутся по настоящим путям (%LOCALAPPDATA%\Temp,
        %SystemRoot%\Temp, C:\ProgramData\…), поэтому `scan --json` их видит
        и Этап 2 становится проверяемым. Такая раскладка требует -AllowRealPaths,
        системные категории — повышения прав, и годится только для одноразовой
        VM: скрипт пишет в %LOCALAPPDATA% и C:\Windows настоящие файлы.

    Чего скрипт не делает и почему — чтобы не выдавать невозможное за сделанное:
      * recycle.bin файлами не синтезируется: объём корзины живёт в
        $Recycle.Bin и читается через SHQueryRecycleBin, а каталог с файлами
        внутри него сканеру кандидатом не покажется. Сценарий про корзину
        готовит мусор штатным перемещением файлов в корзину, и к этому
        скрипту он отношения не имеет.
      * winsxs.report не синтезируется вовсе: это оценка DISM
        /AnalyzeComponentStore, а не обход каталога. Правдоподобно наполнить
        WinSxS нельзя, да и измерить снаружи нельзя. Сценарий проверяет только
        «оценка ничего не удалила».
      * набор правил скрипт не подменяет: правила — данные, и движок принимает
        набор только после проверки подписи (SPEC §9.2, ADR-008,
        src/platform/rulesync_client.hpp п. 2). Свой набор под фикстуру значит
        ещё и пересборку с ключом этого набора. Поэтому фикстура готовит
        данные под настоящие локаторы, а не набор под фикстуру.
      * allocatedBytes в манифесте не пишется: аллоцированный размер отдаёт
        файловая система, и на NTFS файл меньше кластера округляется вверх
        (обычно 4 КиБ). Манифест даёт логический размер и оговорку, а сценарий
        сверяет сумму с допуском 0.5 % — как и требует Этап 2.

.PARAMETER Action
    New | List | Remove | SelfTest. По умолчанию New.

.PARAMETER Root
    Корень фикстуры в раскладке Mirror. По умолчанию %TEMP%\mrproper-junk-fixture.
    Скрипт создаёт каталог, если его нет, и не трогает ничего за его пределами.

.PARAMETER Layout
    Mirror | Real. По умолчанию Mirror. Real требует -AllowRealPaths.

.PARAMETER AllowRealPaths
    Подтверждение, что машина одноразовая. Без него -Layout Real — отказ.

.PARAMETER Category
    Что готовить, через запятую: -Category temp.user=64MB,logs.system=200.
    Формат элемента: id[=величина]. Величина — либо объём (64MB, 1.5GiB, 512KiB,
    либо 1048576b с суффиксом «b»), либо число файлов голым числом (200).
    Список профилей — после «+»: browser.cache.chrome=128MB+Default+Profile 2.
    Служебные имена: default — шесть категорий проверки Этапа 2, all — все
    поддерживаемые, кроме тяжёлых и Risky. Пустой список с -TotalBytes — все
    поддерживаемые.
    Именно запятая, а не повторение флага: `powershell.exe -File script.ps1`
    не разбирает командную строку как PowerShell, и два флага -Category
    binder не примет (ParameterAlreadyBound).

.PARAMETER TotalBytes
    Общий объём на категории, которым объём не задан: 1GB делится поровну.
    Категории с явным объёмом или числом файлов не участвуют в делении.

.PARAMETER FreshRatio
    Доля «свежих» файлов (0..1): их возраст меньше minAgeDays, поэтому сканер
    обязан их пропустить. По умолчанию 0 — весь мусор старше порога, то есть
    весь должен найтись. Для категорий с minAgeDays = 0 доля игнорируется:
        возрастной фильтр там отсутствует, «свежих» файлов не бывает.

.PARAMETER AgeJitterDays
    Насколько глубоко в прошлое уходят «старые» файлы: от minAgeDays + 1 до
    minAgeDays + 1 + AgeJitterDays дней. Разброс делает возраст неприлично
    одинаковым, а крайние случаи (файл ровно на пороге) проверяются отдельно,
    через -AgeJitterDays 0.

.PARAMETER MaxFilesPerCategory
    Потолок числа файлов в одной категории (по умолчанию 20000). Нужен, чтобы
    опечатка в объёме (12TB) не забила диск; при срабатывании скрипт падает с
    понятным текстом, а не урезает молча.

.PARAMETER Profile
    Профили браузера для каталогов с плейсхолдером {profile}: Default, Profile 1.
    На каждый профиль создаётся свой каталог — так проверяется группировка
    кандидатов по профилю (groupByProfile в правилах).

.PARAMETER Seed
    Начальное значение генератора. Одинаковый Seed и одинаковый запрос дают
    одинаковые имена и одинаковые ВОЗРАСТЫ файлов: возраст считается от
    момента запуска, поэтому абсолютные отметки времени двух прогонов будут
    отличаться на разницу во времени старта. Повторяется то, что важно для
    сканера, — набор файлов, их размеры и «насколько старые». Упавший сценарий
    можно прогнать заново и сравнить возрасты, а не отметки времени.

.PARAMETER LockFileCount
    Сколько свежих файлов (не более одного на категорию) держать открытыми без
    общего доступа. Сценарий «приложение держит файл» (SPEC §11.3) ждёт от
    движка пропуска занятого файла, а занятого файла на VM иначе не бывает.

.PARAMETER HoldLockedSeconds
    Сколько секунд держать эти файлы занятыми. Ноль — не блокировать ничего.
    Пока скрипт держит файлы, сценарий должен успеть просканировать: удобно
    запускать его параллельно (Start-Process / Start-Job) и ждать маркер в выводе.

.PARAMETER Json
    Машиночитаемый результат (манифест фикстуры или каталог категорий) в stdout;
    человеческий текст при этом уходит в stderr, как в tools\sign-rules.ps1.

.PARAMETER Quiet
    Только ошибки.

.EXAMPLE
    powershell -NoProfile -File tests\e2e\New-JunkFixture.ps1 -Action List

    Печатает каталог категорий: что можно попросить и что фикстура подставит.

.EXAMPLE
    powershell -NoProfile -File tests\e2e\New-JunkFixture.ps1 -Action New `
        -Root C:\vm\junk -TotalBytes 1GB -Category default

    Готовит шесть категорий проверки Этапа 2 общим объёмом 1 ГБ зеркалом под
    C:\vm\junk. Сканер это дерево не увидит — для проверки скана нужен
    -Layout Real -AllowRealPaths на одноразовой VM.

.EXAMPLE
    powershell -NoProfile -File tests\e2e\New-JunkFixture.ps1 -Action New `
        -Root C:\vm\junk -Category temp.user=64MB,logs.system=200,browser.cache.chrome=128MB+Default+Profile 1 -FreshRatio 0.1

    Точечная фикстура: 64 МиБ временных файлов, 200 файлов журналов, кэш Chrome
    в двух профилях, десятая часть файлов свежее порога возраста.

.EXAMPLE
    powershell -NoProfile -File tests\e2e\New-JunkFixture.ps1 -Action New `
        -Layout Real -AllowRealPaths -Category temp.user=256MB,temp.system=256MB

    Единственный режим, в котором `mrproper-cli scan --json` увидит фикстуру.
    Требует одноразовой VM: файлы пишутся в %LOCALAPPDATA%\Temp и
    %SystemRoot%\Temp по-настоящему.

.EXAMPLE
    powershell -NoProfile -File tests\e2e\New-JunkFixture.ps1 -Action SelfTest

    Разбор скрипта парсером плюс маленькая фикстура в %TEMP% с проверкой
    байтов, возраста и границы файлов. Кода возврата 0 — можно в CI.

.NOTES
    Файл держится в UTF-8 с BOM и CRLF намеренно: PowerShell 5.1 читает .ps1
    без BOM в однобайтовой кодировке ANSI, и русский текст в комментариях
    превращается в ломбард (та же причина, по которой tools\build_lock.ps1
    ограничен ASCII). Стиль — как у tools\sign-rules.ps1.

    Производительность. Внутренние функции не имеют атрибутов
    [Parameter(Mandatory)]: на этом хосте такой атрибут стоит десятки
    миллисекунд на вызов, и на горячем пути (файл на файл) фикстура на 1 ГБ
    раскладывалась 5 минут вместо 5 секунд. Подробности и замер — в
    New-FixtureFile; там же про то, почему обязательность проверяется вручную.

    Коды возврата: 0 — успех, 1 — сбой, 2 — ошибка использования.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('New', 'List', 'Remove', 'SelfTest')]
    [string]$Action = 'New',

    [string]$Root,
    [string]$RepoRoot,
    [ValidateSet('Mirror', 'Real')]
    [string]$Layout = 'Mirror',
    [switch]$AllowRealPaths,

    [string[]]$Category = @(),
    [string]$TotalBytes,

    [ValidateRange(0.0, 1.0)]
    [double]$FreshRatio = 0.0,
    [ValidateRange(0, 3650)]
    [int]$AgeJitterDays = 365,
    [ValidateRange(1, 1000000)]
    [int]$MaxFilesPerCategory = 20000,
    [string[]]$Profile = @('Default', 'Profile 1'),
    [int]$Seed = 20260928,

    [ValidateRange(0, 1000)]
    [int]$LockFileCount = 0,
    [ValidateRange(0, 3600)]
    [int]$HoldLockedSeconds = 0,

    [switch]$Json,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Русский текст в отчёте должен доезжать до консоли читаемым, а не ломбардом:
# без этого cmd в кодировке 866/1251 превращает сообщения в мусор. У redirects
# и неконсольных хостов смена кодировки может быть запрещена — тогда оставляем
# как есть, на разбор скрипта это не влияет.
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding = [System.Text.Encoding]::UTF8
} catch {
    # хост без смены кодировки вывода — не повод прерывать генерацию
}

# Код возврата, который поставит нижний обработчик. Ошибка использования (2) и
# сбой (1) — разные ситуации для CI: «забыл -Category» и «диск кончился» лечат
# по-разному.
$script:ExitCode = 0

# Объём одной категории, если он не задан ни явно, ни через -TotalBytes.
# Восемь мегабайт — достаточно, чтобы увидеть кандидата в отчёте, и мало,
# чтобы фикстура случайно не заняла диск.
$script:DefaultCategoryBytes = 8MB

# Буфер записи. Один мегабайт: файл целиком в память не держим, а на каждый
# мелкий файл не открываем новый буфер.
$script:ChunkBytes = 1MB

# Состояние генератора псевдослучайных чисел (тот же LCG, что в
# core/random_pool.cpp). Нужен, чтобы разброс имён и возрастов был
# воспроизводимым при одном и том же -Seed.
$script:Rng = [uint32]0

# ------------------------------------------------------------------ каталог --

# Записи о правилах, под которые умеет готовить мусор. Поля взяты из rules\*.json
# без домысливания: RuleLocator — дословный locator, MinAgeDays — его порог,
# Safety — его уровень риска. Правило и фикстура разойдутся, и сценарий будет
# проверять не то, что чистит приложение.
$script:Categories = @(
    @{
        Id            = 'temp.user'
        Category      = 'temp.user'
        Safety        = 'safe'
        MinAgeDays    = 2
        RuleLocator   = '%LOCALAPPDATA%\Temp\**'
        Prefix        = 'localappdata'
        SubPath       = 'Temp'
        Template      = 'tmp-######.tmp'
        TypicalBytes  = 1MB
        Admin         = $false
        InDefault     = $true
        InAll         = $true
        Note          = 'Распакованные установщики и променуточные данные (SPEC FR-3). Порог 2 дня. Отдельное правило temp.user.env (%TEMP%) не готовится: на Windows %TEMP% лежит внутри %LOCALAPPDATA%\Temp и покрыто тем же деревом.'
    }
    @{
        Id            = 'temp.system'
        Category      = 'temp.system'
        Safety        = 'safe'
        MinAgeDays    = 7
        RuleLocator   = 'C:\Windows\Temp\**'
        Prefix        = 'windows'
        SubPath       = 'Temp'
        Template      = 'sys-######.tmp'
        TypicalBytes  = 1MB
        Admin         = $true
        InDefault     = $true
        InAll         = $true
        Note          = 'Системная папка Temp, порог 7 дней. Настоящий путь, поэтому в -Layout Real нужны права администратора.'
    }
    @{
        Id            = 'logs.system'
        Category      = 'logs'
        Safety        = 'safe'
        MinAgeDays    = 14
        RuleLocator   = 'C:\Windows\Logs\**'
        Prefix        = 'windows'
        SubPath       = 'Logs\CBS'
        Template      = 'CbG-######.log'
        TypicalBytes  = 64KB
        Admin         = $true
        InDefault     = $true
        InAll         = $true
        Note          = 'Журналы CBS (сборки компонентов). Порог 14 дней — самый долгий из safe, ради него стоит задавать -AgeJitterDays.'
    }
    @{
        Id            = 'logs.local'
        Category      = 'logs'
        Safety        = 'safe'
        MinAgeDays    = 14
        RuleLocator   = '%LOCALAPPDATA%\**\Logs\**\*.log'
        Prefix        = 'localappdata'
        SubPath       = 'MrProperFixture\Logs'
        Template      = 'app-######.log'
        TypicalBytes  = 64KB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'Журналы приложений. Локатор с одиночным «**» в середине, поэтому под него подходит любой подкаталог Logs в профиле.'
    }
    @{
        Id            = 'wer'
        Category      = 'wer'
        Safety        = 'safe'
        MinAgeDays    = 7
        RuleLocator   = 'C:\ProgramData\Microsoft\Windows\WER\**'
        Prefix        = 'programdata'
        SubPath       = 'Microsoft\Windows\WER\ReportQueue'
        Template      = 'Report-######.wer'
        TypicalBytes  = 32KB
        Admin         = $true
        InDefault     = $false
        InAll         = $true
        Note          = 'Накопленные отчёты об ошибках, порог 7 дней. Дампы сюда не кладутся: .dmp принадлежат crash.dumps и memory.dumps.'
    }
    @{
        Id            = 'browser.cache.chrome'
        Category      = 'browser.cache'
        Safety        = 'safe'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\Google\Chrome\User Data\*\Cache\**'
        Prefix        = 'localappdata'
        SubPath       = 'Google\Chrome\User Data\{profile}\Cache\Cache_Data'
        Template      = 'f_######'
        TypicalBytes  = 256KB
        Admin         = $false
        InDefault     = $true
        InAll         = $true
        Note          = 'HTTP-кэш Chromium в современном формате: подкаталог Cache_Data с файлами f_######. Порога возраста нет — весь мусор обязан попасть в кандидаты, FreshRatio здесь не работает.'
    }
    @{
        Id            = 'browser.cache.edge'
        Category      = 'browser.cache'
        Safety        = 'safe'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\Microsoft\Edge\User Data\*\Cache\**'
        Prefix        = 'localappdata'
        SubPath       = 'Microsoft\Edge\User Data\{profile}\Cache\Cache_Data'
        Template      = 'f_######'
        TypicalBytes  = 256KB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'Тот же кэш у Edge. Отдельной категорией от Chrome он не идёт, но отдельной строкой в плане да: правила различаются шаблоном пути.'
    }
    @{
        Id            = 'firefox.cache.cache2'
        Category      = 'firefox.cache'
        Safety        = 'safe'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\Mozilla\Firefox\Profiles\*\cache2\**'
        Prefix        = 'localappdata'
        SubPath       = 'Mozilla\Firefox\Profiles\{profile}\cache2\entries'
        Template      = 'A1B2C3D4-######'
        TypicalBytes  = 128KB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'Firefox: подкаталог cache2 с записями, имена — шестнадцатеричные. Проверяет, что отдельная категория firefox.cache не смешана с browser.cache.'
    }
    @{
        Id            = 'browser.history.chrome'
        Category      = 'browser.history'
        Safety        = 'review'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\Google\Chrome\User Data\*\History*'
        Prefix        = 'localappdata'
        SubPath       = 'Google\Chrome\User Data\{profile}'
        Names         = @('History', 'History-journal', 'History-wal', 'Cookies', 'Login Data', 'Web Data', 'Favicons')
        TypicalBytes  = 64KB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'История, cookies и пароли: review, по умолчанию выключено (SPEC FR-3). Имена здесь не генерируются, а фиксированы — как в настоящем профиле, поэтому число файлов ограничено семью, и запрос «больше» обрежется с предупреждением.'
    }
    @{
        Id            = 'shadercache.directx'
        Category      = 'shadercache'
        Safety        = 'safe'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\D3DSCache\**'
        Prefix        = 'localappdata'
        SubPath       = 'D3DSCache'
        Template      = 'shadercache-######.bin'
        TypicalBytes  = 512KB
        Admin         = $false
        InDefault     = $true
        InAll         = $true
        Note          = 'Кэш шейдеров DirectX. Порога нет: пересобирается при следующем запуске любой игры.'
    }
    @{
        Id            = 'shadercache.nvidia.dx'
        Category      = 'shadercache'
        Safety        = 'safe'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\NVIDIA\DXCache\**'
        Prefix        = 'localappdata'
        SubPath       = 'NVIDIA\DXCache'
        Template      = 'cache-######.bin'
        TypicalBytes  = 512KB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'Кэш шейдеров NVIDIA. На VM без видеокарты каталога не будет — правило просто не даст кандидатов, и это норма.'
    }
    @{
        Id            = 'icon.font.cache.explorer'
        Category      = 'icon.font.cache'
        Safety        = 'safe'
        MinAgeDays    = 2
        RuleLocator   = '%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache*.db'
        Prefix        = 'localappdata'
        SubPath       = 'Microsoft\Windows\Explorer'
        Template      = 'iconcache-#.db'
        TypicalBytes  = 1MB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'Кэш значков проводника (правило icon.font.cache.explorer.thumbnails отличается только маской thumbcache_*.db). Порог 2 дня.'
    }
    @{
        Id            = 'prefetch.files'
        Category      = 'prefetch'
        Safety        = 'review'
        MinAgeDays    = 30
        RuleLocator   = 'C:\Windows\Prefetch\**\*.pf'
        Prefix        = 'windows'
        SubPath       = 'Prefetch'
        Template      = 'MRPROPER-######.pf'
        TypicalBytes  = 128KB
        Admin         = $true
        InDefault     = $false
        InAll         = $true
        Note          = 'Prefetch: review и выключен по умолчанию (SPEC FR-3 — сборочный мусор не даёт, ухудшает отладку). Порог 30 дней.'
    }
    @{
        Id            = 'ms.update.download'
        Category      = 'ms.update'
        Safety        = 'safe'
        MinAgeDays    = 7
        RuleLocator   = 'C:\Windows\SoftwareDistribution\Download\**'
        Prefix        = 'windows'
        SubPath       = 'SoftwareDistribution\Download'
        Template      = 'update-######.cab'
        TypicalBytes  = 8MB
        Admin         = $true
        InDefault     = $false
        InAll         = $true
        Note          = 'Скачанные обновления Windows, порог 7 дней. Службы wuauserv/bits должны быть остановлены — на VM сценарий «занято» их глушит.'
    }
    @{
        Id            = 'delivery.opt.cache'
        Category      = 'delivery.opt'
        Safety        = 'review'
        MinAgeDays    = 7
        RuleLocator   = 'C:\Windows\SoftwareDistribution\DeliveryOptimization\**'
        Prefix        = 'windows'
        SubPath       = 'SoftwareDistribution\DeliveryOptimization\Cache'
        Template      = 'do-######.dat'
        TypicalBytes  = 1MB
        Admin         = $true
        InDefault     = $false
        InAll         = $true
        Note          = 'Кэш доставки оптимизации, review. Лежит рядом с ms.update, но в другой ветке дерева, поэтому пересечения кандидатов нет.'
    }
    @{
        Id            = 'crash.dumps.app'
        Category      = 'crash.dumps'
        Safety        = 'review'
        MinAgeDays    = 30
        RuleLocator   = '%LOCALAPPDATA%\CrashDumps\**\*.dmp'
        Prefix        = 'localappdata'
        SubPath       = 'CrashDumps'
        Template      = 'App-######.dmp'
        TypicalBytes  = 2MB
        Admin         = $false
        InDefault     = $true
        InAll         = $true
        Note          = 'Дампы упавших приложений: review, выключено по умолчанию. Порог 30 дней — свежий дамп почти всегда нужен для разбора.'
    }
    @{
        Id            = 'memory.dumps.livekernel'
        Category      = 'memory.dumps'
        Safety        = 'review'
        MinAgeDays    = 30
        RuleLocator   = 'C:\Windows\LiveKernelReports\**\*.dmp'
        Prefix        = 'windows'
        SubPath       = 'LiveKernelReports\WATCHDOG'
        Template      = 'WATCHDOG-######.dmp'
        TypicalBytes  = 4MB
        Admin         = $true
        InDefault     = $false
        InAll         = $true
        Note          = 'Дампы ядра и видеодрайвера (таймауты GPU, watchdog). Порог 30 дней, review.'
    }
    @{
        Id            = 'npm.pip.cache.npm'
        Category      = 'npm.pip.cache'
        Safety        = 'review'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\npm-cache'
        Prefix        = 'localappdata'
        SubPath       = 'npm-cache\_cacache\content-v2\sha512\00\00'
        Template      = 'aa-######.bin'
        TypicalBytes  = 256KB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'Кэш npm. Правило оценивает каталог целиком и не перечисляет содержимое (см. note в rules\dev.caches.json), поэтому фикстура кладёт файлы вглубь content-v2, а их число на кандидата не влияет.'
    }
    @{
        Id            = 'npm.pip.cache.nuget.packages'
        Category      = 'npm.pip.cache'
        Safety        = 'review'
        MinAgeDays    = 0
        RuleLocator   = '%USERPROFILE%\.nuget\packages'
        Prefix        = 'userprofile'
        SubPath       = '.nuget\packages\mrproper.fixture\1.0.0'
        Template      = 'mrproper.fixture.1.0.0.nupkg'
        TypicalBytes  = 1MB
        Admin         = $false
        InDefault     = $false
        InAll         = $true
        Note          = 'Глобальная папка пакетов NuGet — самый крупный и самый опасный элемент категории. Раскладка как у настоящей: <идентификатор>\<версия>\<файл>.'
    }
    @{
        Id            = 'user.bigfiles'
        Category      = 'user.bigfiles'
        Safety        = 'review'
        MinAgeDays    = 90
        RuleLocator   = '%USERPROFILE%\Downloads\**'
        Prefix        = 'userprofile'
        SubPath       = 'Downloads'
        Template      = 'archive-######.mp4'
        TypicalBytes  = 1200MB
        Admin         = $false
        InDefault     = $false
        InAll         = $false
        Note          = 'Крупные файлы пользователя: правило берёт Downloads и требует больше 1 ГБ и старше 90 дней (SPEC FR-3). Каждый файл здесь больше гигабайта, поэтому в -Category all не входит — готовится только по явному имени.'
    }
    @{
        Id            = 'installer.cache.packages'
        Category      = 'installer.cache'
        Safety        = 'risky'
        MinAgeDays    = 0
        RuleLocator   = 'C:\Windows\Installer\**\*.msi'
        Prefix        = 'windows'
        SubPath       = 'Installer'
        Template      = 'msi-######.msi'
        TypicalBytes  = 4MB
        Admin         = $true
        InDefault     = $false
        InAll         = $false
        Note          = 'Кэш установщиков: Risky, только оценка, удаление ломает uninstall/repair. В -Category all не входит и в -Layout Real не пишется: настоящий C:\Windows\Installer трогать нельзя даже на VM — сценарий проверяет скрытие Risky по умолчанию на настоящем каталоге.'
    }
    @{
        Id            = 'wsl.vhdx.report.distro.store'
        Category      = 'wsl.vhdx.report'
        Safety        = 'risky'
        MinAgeDays    = 0
        RuleLocator   = '%LOCALAPPDATA%\Packages\*\LocalState\ext4.vhdx'
        Prefix        = 'localappdata'
        SubPath       = 'Packages\MrProperFixture\LocalState'
        Template      = 'ext4.vhdx'
        TypicalBytes  = 64MB
        Admin         = $false
        InDefault     = $false
        InAll         = $false
        Note          = 'Образ дистрибутива WSL: Risky, оценка по размеру файла. В .vhdx нельзя положить мусор, который не поймает оценщик, а настоящий ext4.vhdx занимает гигабайты, поэтому в -Category all не входит.'
    }
    # --- то, что файлами не синтезируется -------------------------------------
    @{
        Id          = 'recycle.bin.allvolumes'
        Category    = 'recycle.bin'
        Safety      = 'safe'
        MinAgeDays  = 1
        RuleLocator = '%SystemDrive%\$Recycle.Bin\**'
        Supported   = $false
        Reason      = 'Корзина не каталог файлов: объём отдаёт SHQueryRecycleBin, а каталог с файлами внутри $Recycle.Bin кандидатом не станет. Сценарий готовит мусор обычным перемещением файлов в корзину.'
    }
    @{
        Id          = 'winsxs.report'
        Category    = 'winsxs.report'
        Safety      = 'risky'
        MinAgeDays  = 0
        RuleLocator = 'C:\Windows\WinSxS'
        Supported   = $false
        Reason      = 'Оценка идёт через DISM /AnalyzeComponentStore, а не обходом каталога: правдоподобно наполнить WinSxS и измерить его снаружи нельзя. Сценарий проверяет только то, что оценка ничего не удалила.'
    }
)

# Значения по умолчанию для полей каталога. Заполняются один раз, и таблица
# становится прямоугольной: под Set-StrictMode обращение к отсутствующему ключу
# хеш-таблицы — исключение, а половина записей (неподдерживаемые категории)
# половину полей по делу не имеет. Без этого каждая строка чтения каталога
# нуждалась бы в проверке «а есть ли такой ключ», и одна забытая проверка
# роняла бы -Action List целиком.
$script:CategoryDefaults = [ordered]@{
    'Supported'    = $true
    'Admin'        = $false
    'InDefault'    = $false
    'InAll'        = $true
    'TypicalBytes' = 64KB
    'Prefix'       = ''
    'SubPath'      = ''
    'Template'     = ''
    'Names'        = $null
    'Note'         = ''
    'Reason'       = ''
}

function Initialize-Catalogue {
    # Прямоугольная таблица: каждый ключ есть у каждой записи, отсутствующие
    # добираются значениями по умолчанию. Возвращать нужно копию списка, иначе
    # вызовы с -eq $true переписывают общий массив (PowerShell копирует
    # элемент структурного литерала при обращении к нему как к объекту).
    param([Parameter(Mandatory = $true)]$Catalogue)
    $result = @()
    foreach ($item in $Catalogue) {
        $record = @{}
        foreach ($key in $item.Keys) { $record[$key] = $item[$key] }
        foreach ($key in $script:CategoryDefaults.Keys) {
            if (-not $record.ContainsKey($key)) {
                $record[$key] = $script:CategoryDefaults[$key]
            }
        }
        $result += $record
    }
    return ,$result
}

$script:Categories = Initialize-Catalogue $script:Categories

# Зеркало префиксов в раскладке Mirror. Реальные адреса намеренно не пишутся
# на диск: сканер по ним и не ходит, а фикстура должна быть безопасной на любой
# машине. Порядок в имени ключа — «что подставляем», а не «что на настоящем
# месте», чтобы в дереве фикстуры нельзя было спутать зеркало с системой.
$script:PrefixesMirror = @{
    'localappdata' = 'user\LocalAppData'
    'appdata'      = 'user\AppData\Roaming'
    'userprofile'  = 'user\Profile'
    'windows'      = 'system\Windows'
    'programdata'  = 'system\ProgramData'
}

# Тот же префикс, но настоящим путём. Только для -Layout Real.
$script:PrefixesReal = @{
    'localappdata' = $env:LOCALAPPDATA
    'appdata'      = $env:APPDATA
    'userprofile'  = $env:USERPROFILE
    'windows'      = $env:SystemRoot
    'programdata'  = $env:ProgramData
}

# --------------------------------------------------------------------- вывод --

function New-UsageError {
    # Ошибка использования (код возврата 2) и сбой (1) — разные ситуации для
    # CI: «не названа категория» лечится иначе, чем «диск кончился».
    # Классифицируем по типу исключения, а не по совпадению в тексте: иначе
    # любая правка формулировки тихо превращает usage-ошибку в сбой.
    param([Parameter(Mandatory = $true)][string]$Message)
    return (New-Object System.ArgumentException($Message))
}

function Write-Line {
    # Человекочитаемый отчёт. В режиме -Json он уходит в stderr: на stdout
    # должен остаться ровно один JSON-объект, иначе его не разобрать в CI.
    param([string]$Text = '', [string]$Kind = 'info')

    # -Quiet убирает прогресс, но не ошибку: молчаливый ненулевой код
    # возврата хуже, чем лишняя строка, иначе сценарий упадёт, не показав
    # причину.
    if ($Quiet -and $Kind -ne 'err') { return }

    $prefixes = @{
        'info' = ''
        'step' = '  '
        'ok'   = '[ok] '
        'warn' = '[!]  '
        'err'  = '[x]  '
    }
    $prefix = $prefixes[$Kind]
    $line = $prefix + $Text
    if ($Kind -eq 'err' -or $Kind -eq 'warn') {
        [Console]::Error.WriteLine($line)
    } else {
        [Console]::Out.WriteLine($line)
    }
}

function Write-Fail {
    param([Parameter(Mandatory = $true)][string]$Message, [int]$Code = 1)
    $script:ExitCode = $Code
    Write-Line $Message 'err'
}

function Format-Bytes {
    # Человекочитаемый объём. Windows считает 1 КиБ = 1024 байт, и сценарии
    # сравнивают числа с отчётом, где unit — тоже 1024, поэтому и здесь так.
    param([Parameter(Mandatory = $true)][double]$Bytes)

    $units = @('Б', 'КиБ', 'МиБ', 'ГиБ', 'ТиБ')
    $value = $Bytes
    $index = 0
    while ($value -ge 1024 -and $index -lt ($units.Count - 1)) {
        $value = $value / 1024
        $index = $index + 1
    }
    if ($index -eq 0) {
        return ('{0} {1}' -f [int64]$value, $units[$index])
    }
    return ('{0:0.##} {1}' -f $value, $units[$index])
}

function Write-Json {
    # Машинный вывод: ровно один JSON на stdout, разумной глубины.
    param([Parameter(Mandatory = $true)]$Object, [int]$Depth = 6)
    $document = $Object | ConvertTo-Json -Depth $Depth
    [Console]::Out.WriteLine($document)
}

# ------------------------------------------------------------------ величины --

function ConvertTo-ByteSize {
    # «64MB», «1.5GiB», «1048576», «2 KB» → число байт. Своя разборка, а не
    # [System.Management.Automation.LanguagePrimitives]: тот понимает только
    # голые числа и молча проглатывает мусор, а сценарий с опечаткой в объёме
    # должен падать, а не создавать 20000 файлов по 12 МиБ.
    param([Parameter(Mandatory = $true)][string]$Text)

    $pattern = '^(?<n>\d+(?:[.,]\d+)?)\s*(?<u>[KMGT]?i?B|bytes?)?$'
    $match = [regex]::Match($Text.Trim(), $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) {
        throw (New-UsageError ('не разобрана величина «{0}»: ждём 64MB, 1.5GiB или число байт' -f $Text))
    }

    $number = [double]::Parse(
        $match.Groups['n'].Value.Replace(',', '.'),
        [System.Globalization.CultureInfo]::InvariantCulture)
    if ($number -le 0) {
        throw (New-UsageError ('величина «{0}» должна быть больше нуля' -f $Text))
    }

    $unit = $match.Groups['u'].Value.ToUpperInvariant()
    $scale = 1
    if ($unit -eq 'KB' -or $unit -eq 'KIB') { $scale = 1KB }
    elseif ($unit -eq 'MB' -or $unit -eq 'MIB') { $scale = 1MB }
    elseif ($unit -eq 'GB' -or $unit -eq 'GIB') { $scale = 1GB }
    elseif ($unit -eq 'TB' -or $unit -eq 'TIB') { $scale = 1TB }

    return [int64][Math]::Round($number * $scale)
}

function Get-NextInt {
    # Псевдослучайное число [0, Bound) из LCG. Get-Random с -SetSeed в
    # PowerShell 5.1 не воспроизводим между вызовами, а повторяемость
    # фикстуры нужна, чтобы упавший сценарий можно было прогнать заново и
    # получить то же дерево.
    # Атрибутов [Parameter(Mandatory)] здесь нет намеренно: см. New-FixtureFile.
    param([int]$Bound)

    if ($Bound -le 1) { return 0 }
    # Модуль по 2^32, а не «-band 0xFFFFFFFF»: в PowerShell этот литерал —
    # Int32 -1, и маскирует он ровно ничего, после чего приведение к UInt32
    # падает на переполнении. Проверено на PowerShell 5.1.
    $script:Rng = [uint32](($script:Rng * 1103515245 + 12345) % 4294967296)
    return [int](($script:Rng -shr 8) % [uint64]$Bound)
}

function Format-LeafName {
    # Имя файла по шаблону: «f_######» → «f_000042», «iconcache-#.db» →
    # «iconcache-7.db». Прогон из двух и более «#» добивается нулями до своей
    # длины, одиночный «#» подставляется как есть — так в одном шаблоне
    # уживаются и f_000001 Chromium, и iconcache_16.db проводника.
    # Атрибутов [Parameter(Mandatory)] здесь нет намеренно, хотя вызывается
    # функция на каждый файл: см. комментарий в New-FixtureFile.
    param(
        [string]$Template,
        [int]$Index
    )

    $digit = $Index.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    $text = $Template
    while ($text.Contains('##')) {
        $start = $text.IndexOf('##')
        $run = 0
        while (($start + $run) -lt $text.Length -and $text[$start + $run] -eq '#') {
            $run = $run + 1
        }
        $replacement = $digit.PadLeft($run, '0')
        $text = $text.Substring(0, $start) + $replacement + $text.Substring($start + $run)
    }
    return $text.Replace('#', $digit)
}

# ----------------------------------------------------------------- категории --

function Get-CategoryRecord {
    # Поиск по id. Список известных id печатаем в тексте ошибки: «категория не
    # найдена» без подсказки бесполезна, а -Action List всё равно существует.
    param([Parameter(Mandatory = $true)][string]$Id)

    $name = $Id.Trim().ToLowerInvariant()
    foreach ($record in $script:Categories) {
        if ($record.Id.ToLowerInvariant() -eq $name) { return $record }
    }
    foreach ($record in $script:Categories) {
        if ($record.Id.ToLowerInvariant().StartsWith($name)) { return $record }
    }
    foreach ($record in $script:Categories) {
        if ($record.Category.ToLowerInvariant() -eq $name) { return $record }
    }

    $known = ($script:Categories | ForEach-Object { $_.Id }) -join ', '
    throw (New-UsageError ('категория «{0}» неизвестна. Известные: {1}' -f $Id, $known))
}

function Remove-OuterQuotes {
    # Обёртки запуска (WSL interop, шаги CI) доставляют аргументы с кавычками
    # внутри значения: -Category "a,b" приходит как «"a,b», где закрывающая
    # кавычка уже съедена оболочкой. Поэтому снимаем кавычки с обоих концов
    # поштучно, а не только парой: одинарная висячая кавычка иначе остаётся
    # в имени категории. В самих значениях кавычек быть не может.
    param([Parameter(Mandatory = $true)][string]$Text)

    $value = $Text.Trim().Trim([char[]]@('"', "'"))
    return $value
}

function Resolve-Request {
    # «temp.user» → id без объёма; «logs.system=200» → число файлов;
    # «browser.cache.chrome=128MB+Default+Profile 2» → объём и профили.
    # Разделитель профилей — «+», а не «,»: запятая уже занята под список
    # категорий, и один и тот же символ в двух местах означал бы, что
    # «browser.cache.chrome=128MB:Default,Profile 1» не разбирается однозначно.
    # Порядок разбора: «+» раньше «=», потому что в «128MB+Default» знак
    # равенства один и он же отделяет величину от id.
    param([Parameter(Mandatory = $true)][string]$Spec)

    $text = (Remove-OuterQuotes $Spec)
    if ($text.Length -eq 0) { throw (New-UsageError 'пустой элемент -Category') }

    $profiles = @()
    $plus = $text.IndexOf('+')
    if ($plus -ge 0) {
        $list = $text.Substring($plus + 1)
        $text = $text.Substring(0, $plus)
        $profiles = @($list -split '\+' | ForEach-Object { $_.Trim() } | Where-Object { $_.Length -gt 0 })
        if ($profiles.Count -eq 0) {
            throw (New-UsageError ('в «{0}» после «+» пустой список профилей' -f $Spec))
        }
    }

    $id = $text
    $value = $null
    $equals = $text.IndexOf('=')
    if ($equals -ge 0) {
        $id = $text.Substring(0, $equals)
        $value = $text.Substring($equals + 1)
    }
    if ($id.Trim().Length -eq 0) { throw (New-UsageError ('в «{0}» нет id категории' -f $Spec)) }

    $sizeBytes = $null
    $fileCount = $null
    if ($null -ne $value) {
        # Голое число — это число файлов, а не байт: «=200» в записи про 200
        # файлов-кандидатов (SPEC §8, Этап 3) читается естественнее, чем 200
        # байт мусора. Байты требуют единицы измерения: 64MB, 1.5GiB, 512KiB
        # или суффикс «b» для ровного числа (1048576b).
        if ($value -match '^\d+$') {
            $fileCount = [int]$value
            if ($fileCount -le 0) { throw (New-UsageError ('в «{0}» число файлов должно быть больше нуля' -f $Spec)) }
        } else {
            $sizeBytes = ConvertTo-ByteSize $value
        }
    }

    return [pscustomobject]@{
        Id        = $id.Trim()
        SizeBytes = $sizeBytes
        FileCount = $fileCount
        Profiles  = $profiles
    }
}

function Resolve-Requests {
    # Разворачивает список -Category в запросы, включая служебные имена
    # default (шесть категорий проверки Этапа 2) и all (все поддерживаемые
    # без тяжёлых и Risky), и распределяет -TotalBytes по тем, у кого объём
    # не задан.
    param(
        [string[]]$Spec,
        [string]$TotalSpec
    )

    $requests = New-Object System.Collections.ArrayList
    if ($Spec.Count -eq 0) {
        if ([string]::IsNullOrEmpty($TotalSpec)) {
            throw (New-UsageError 'нечего готовить: назовите -Category (например -Category default) или задайте -TotalBytes')
        }
        $Spec = @('all')
    }

    # Запятая разбирается здесь, а не binder'ом PowerShell: запуск
    # `powershell.exe -File script.ps1 -Category a,b` передаёт «a,b» ОДНИМ
    # аргументом, и [string[]] остаётся одним элементом со встроенной
    # запятой. Повторение флага (-Category a -Category b) binder не примет вовсе
    # (ParameterAlreadyBound), поэтому список — всегда через запятую. Разбор
    # идемпотентен: если запятые уже разошлись на элементы, повторно они
    # ничего не меняют.
    $items = New-Object System.Collections.ArrayList
    foreach ($entry in $Spec) {
        foreach ($piece in ($entry -split ',')) {
            $trimmed = (Remove-OuterQuotes $piece)
            if ($trimmed.Length -gt 0) { [void]$items.Add($trimmed) }
        }
    }

    foreach ($name in $items) {
        $name = $name.Trim()
        if ($name -eq 'all') {
            foreach ($record in $script:Categories) {
                if ($record.Supported -ne $false -and $record.InAll -eq $true) {
                    [void]$requests.Add((Resolve-Request $record.Id))
                }
            }
            continue
        }
        if ($name -eq 'default') {
            foreach ($record in $script:Categories) {
                if ($record.InDefault -eq $true) {
                    [void]$requests.Add((Resolve-Request $record.Id))
                }
            }
            continue
        }
        [void]$requests.Add((Resolve-Request $name))
    }

    # Дубли схлопываем: дважды попросенные temp.user должны бы честно сказать,
    # что это две разные порции, а не тихо сложиться в один каталог.
    $seen = @{}
    foreach ($request in $requests) {
        $key = $request.Id.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            throw (New-UsageError ('категория «{0}» запрошена дважды: разнесите объём по другим категориям или в один запрос' -f $request.Id))
        }
        $seen[$key] = $true
    }

    $unspecified = @($requests | Where-Object { $null -eq $_.SizeBytes -and $null -eq $_.FileCount })
    if ($unspecified.Count -gt 0) {
        if (-not [string]::IsNullOrEmpty($TotalSpec)) {
            $share = [int64][Math]::Floor((ConvertTo-ByteSize $TotalSpec) / $unspecified.Count)
            if ($share -le 0) {
                throw (New-UsageError ('-TotalBytes {0} на {1} категорий не хватает: каждой досталось бы 0 байт' -f $TotalSpec, $unspecified.Count))
            }
            foreach ($request in $unspecified) { $request.SizeBytes = $share }
        } else {
            foreach ($request in $unspecified) { $request.SizeBytes = $script:DefaultCategoryBytes }
        }
    }

    # Возврат через запятую: иначе список запросов из одного элемента доходит до
    # вызывающего развёрнутым в одиночный объект, у которого нет .Count.
    return ,$requests
}

function New-FixtureDirectories {
    # Каталоги под одну категорию. Плейсхолдер {profile} раскрывается в
    # отдельный каталог на каждый профиль: правила браузеров группируют
    # кандидатов по профилю (groupByProfile), и фикстура обязана это повторять,
    # иначе сценарий проверит один каталог вместо двух строк плана.
    param(
        [Parameter(Mandatory = $true)]$Record,
        [string[]]$Profiles,
        [Parameter(Mandatory = $true)][string]$Base
    )

    $sub = $Record.SubPath
    if ($sub.Contains('{profile}')) {
        $names = $Profiles
        if ($names.Count -eq 0) { $names = @('Default') }
        $paths = @()
        foreach ($name in $names) {
            $paths += (Join-Path $Base ($sub.Replace('{profile}', $name)))
        }
        # Запятая перед возвратом обязательна: без неё PowerShell разворачивает
        # массив в конвейер, и вызывающий получает строку вместо списка каталогов
        # (у одиночной категории это ровно один элемент, а .Count у строки нет).
        return ,$paths
    }
    return ,@((Join-Path $Base $sub))
}

# -------------------------------------------------------------------- запись --

function New-FixtureFile {
    # Один файл заданного размера с заданным временем. Пишем потоком общим
    # буфером, содержимое детерминированное и не нулевое: нули на NTFS с
    # включённым сжатием дали бы allocatedBytes заметно меньше размера, и
    # сравнение с эталоном по 0.5 % (SPEC §8, Этап 2) врало бы.
    # Время ставится после закрытия файла: на открытом SetLastWriteTimeUtc
    # может не сработать.
    # ПОЧЕМУ НЕТ [Parameter(Mandatory = $true)].
    #
    # Любой атрибут [Parameter()] делает функцию «расширенной», и её разбор
    # параметров в PowerShell 5.1 стоит десятки миллисекунд на вызов — измерено
    # на этом хосте: 100 вызовов 4468 мс с атрибутами и 45 мс без них, при
    # одинаковом теле. Эта функция зовётся на каждый файл, поэтому фикстура на
    # 1 ГБ (4184 файла, SPEC §8 Этап 2) раскладывалась 5 минут вместо секунд.
    #
    # Побочная польза: Mandatory без ValueFromPipeline при недостающем
    # аргументе заставил бы PowerShell ПРОМПТИТ пользователя, а в
    # неинтерактивном прогоне e2e это молчаливое зависание вместо ошибки.
    # Обязательность проверяем сами: без пути и без положительного размера
    # файла писать нечего, и это ошибка использования (New-UsageError).
    param(
        [string]$Path,
        [int64]$Bytes,
        [datetime]$Utc,
        [byte[]]$Chunk
    )

    if ([string]::IsNullOrEmpty($Path) -or $Bytes -lt 1 -or $null -eq $Chunk -or $Chunk.Length -lt 1) {
        throw (New-UsageError ('New-FixtureFile: некорректный вызов (путь «{0}», размер {1}, буфер {2} байт)' -f `
                $Path, $Bytes, $(if ($null -eq $Chunk) { 0 } else { $Chunk.Length })))
    }

    $directory = [System.IO.Path]::GetDirectoryName($Path)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }

    $stream = [System.IO.File]::Create($Path)
    try {
        $written = [int64]0
        while ($written -lt $Bytes) {
            $take = [int]$Chunk.Length
            $left = $Bytes - $written
            if ($left -lt $take) { $take = [int]$left }
            $stream.Write($Chunk, 0, $take)
            $written = $written + $take
        }
    } finally {
        # Закрываем и на исключении: незакрытый FileStream держит файл занятым,
        # и следующий прогон фикстуры упрётся в него.
        $stream.Dispose()
    }

    $stamp = [System.DateTime]::SpecifyKind($Utc, [System.DateTimeKind]::Utc)
    [System.IO.File]::SetCreationTimeUtc($Path, $stamp)
    [System.IO.File]::SetLastWriteTimeUtc($Path, $stamp)
    [System.IO.File]::SetLastAccessTimeUtc($Path, $stamp)
}

function New-JunkCategory {
    # Создаёт мусор одной категории и возвращает запись манифеста. Считает всё
    # по факту: число файлов и байты — из того, что легло на диск, а не из
    # того, что задумано. Манifest — контракт сценария, и он должен описывать
    # реальность, иначе расхождение нельзя будет отличить от ошибки сканера.
    param(
        [Parameter(Mandatory = $true)]$Request,
        [Parameter(Mandatory = $true)]$Record,
        [Parameter(Mandatory = $true)]$Context,
        # Префикс (user\LocalAppData, system\Windows, …) передаётся отдельно от
        # контекста намеренно: держать его в общем состоянии значило бы
        # полагаться на то, что цикл успел его переписать. С такой ошибкой
        # temp.user и temp.system молча писали в один и тот же каталог, и
        # фикстура «шесть категорий» оказывалась пятью.
        [Parameter(Mandatory = $true)][string]$Base
    )

    # Сколько всего байт и сколько файлов.
    $targetBytes = $null
    $fileCount = $null
    if ($null -ne $Request.SizeBytes) { $targetBytes = [int64]$Request.SizeBytes }
    if ($null -ne $Request.FileCount) { $fileCount = [int]$Request.FileCount }
    if ($null -eq $fileCount) {
        $typical = [int64]$Record.TypicalBytes
        if ($typical -le 0) { $typical = 64KB }
        $fileCount = [int][Math]::Ceiling([double]$targetBytes / [double]$typical)
        if ($fileCount -lt 1) { $fileCount = 1 }
    }
    if ($null -eq $targetBytes) {
        $targetBytes = [int64]$fileCount * [int64]$Record.TypicalBytes
    }

    # У правил с фиксированными именами (Cookies, Login Data) больше файлов,
    # чем сгенерировать нельзя: имя не выдумать. Обрезаем с предупреждением
    # и пересчитываем объём, иначе сумма разъедусь с ожиданием сценария.
    $names = @()
    if ($null -ne $Record.Names) {
        $names = @($Record.Names)
        if ($fileCount -gt $names.Count) {
            Write-Line ('{0}: запрошено файлов {1}, у правила фиксированные имена ({2}) — беру {3}' -f `
                    $Record.Id, $fileCount, $names.Count, $names.Count) 'warn'
            $fileCount = $names.Count
            $targetBytes = [int64]$fileCount * [int64]$Record.TypicalBytes
        }
    }
    if ($fileCount -lt 1) { $fileCount = 1 }
    if ($fileCount -gt $Context.MaxFiles) {
        throw (New-UsageError ('{0}: {1} файлов при типичном размере {2}. Голое число в -Category читается как число файлов: ' +
            'для объёма добавь единицу измерения (например 64MB) или суффикс «b», либо подними -MaxFilesPerCategory.' -f `
            $Record.Id, $fileCount, (Format-Bytes $Record.TypicalBytes)))
    }

    # Сколько файлов «свежие», то есть не должны пережить возрастной фильтр.
    # При minAgeDays = 0 фильтра нет, и «свежесть» не имеет смысла: такие
    # файлы просто не отмечаем, иначе манифест врал бы про кандидатов.
    $freshCount = 0
    if ($Context.FreshRatio -gt 0) {
        if ($Record.MinAgeDays -le 0) {
            Write-Line ('{0}: minAgeDays = 0, возрастного фильтра нет — -FreshRatio здесь не действует' -f $Record.Id) 'warn'
        } else {
            $freshCount = [int][Math]::Round($fileCount * $Context.FreshRatio)
            if ($freshCount -gt $fileCount) { $freshCount = $fileCount }
        }
    }

    $profiles = $Request.Profiles
    if ($profiles.Count -eq 0) { $profiles = $Context.Profiles }
    $directories = New-FixtureDirectories $Record $profiles $Base

    # Файлов может оказаться меньше, чем каталогов (просили 1 файл, а профилей
    # три). Молча оставлять два пустых каталога нельзя — сценарий увидит в
    # плане кандидата, которого нет, поэтому говорим об этом прямо.
    if ($fileCount -lt $directories.Count) {
        Write-Line ('{0}: файлов {1}, а каталогов под профили {2} — часть профилей останется без мусора' -f `
                $Record.Id, $fileCount, $directories.Count) 'warn'
    }

    $stats = @()
    foreach ($directory in $directories) {
        $stats += [pscustomobject]@{
            path            = $directory
            files           = 0
            bytes           = [int64]0
            candidateFiles  = 0
            candidateBytes  = [int64]0
            locked          = [System.Collections.ArrayList]::new()
        }
    }

    # Раскладываем объём поровну, последний файл добирает остаток: сумма
    # совпадает с запрошенной побайтно, а не «примерно».
    $perFile = [int64][Math]::Floor($targetBytes / $fileCount)
    if ($perFile -lt 1) { $perFile = 1 }
    $remaining = $targetBytes

    $oldest = $null
    $newest = $null
    $totalFiles = 0
    $totalBytes = [int64]0
    $candidateFiles = 0
    $candidateBytes = [int64]0
    $oldPaths = New-Object System.Collections.ArrayList

    for ($index = 0; $index -lt $fileCount; $index++) {
        $slot = $stats[$index % $stats.Count]
        $directory = $slot.path

        if ($names.Count -gt 0) {
            $leaf = $names[[Math]::Min($index, $names.Count - 1)]
        } else {
            $leaf = Format-LeafName $Record.Template ($index + 1)
        }
        $path = Join-Path $directory $leaf

        $take = $perFile
        if ($take -gt $remaining) { $take = $remaining }
        if ($take -lt 1) { $take = 1 }
        $remaining = $remaining - $take

        # Возраст. Старый файл — за порогом правила плюс разброс; свежий —
        # строго моложе порога (длящийся меньше суток, чтобы при minAgeDays = 1
        # он гарантированно остался «молодым» независимо от времени суток).
        $isFresh = ($index -lt $freshCount)
        $ageDays = 0
        if ($isFresh) {
            $ageHours = Get-NextInt 24
            $stamp = $Context.NowUtc.AddHours(-$ageHours)
        } else {
            $ageDays = $Record.MinAgeDays + 1
            if ($Context.AgeJitterDays -gt 0) {
                $ageDays = $ageDays + (Get-NextInt ($Context.AgeJitterDays + 1))
            }
            $ageSeconds = Get-NextInt 86400
            $stamp = $Context.NowUtc.AddDays(-$ageDays).AddSeconds(-$ageSeconds)
        }

        New-FixtureFile $path $take $stamp $Context.Chunk

        $slot.files = $slot.files + 1
        $slot.bytes = $slot.bytes + $take
        $totalFiles = $totalFiles + 1
        $totalBytes = $totalBytes + $take
        if ($null -eq $oldest -or $stamp -lt $oldest) { $oldest = $stamp }
        if ($null -eq $newest -or $stamp -gt $newest) { $newest = $stamp }
        if (-not $isFresh) {
            $slot.candidateFiles = $slot.candidateFiles + 1
            $slot.candidateBytes = $slot.candidateBytes + $take
            $candidateFiles = $candidateFiles + 1
            $candidateBytes = $candidateBytes + $take
            [void]$oldPaths.Add($path)
        }
    }

    # Занятые файлы: по одному на каталог, из числа свежих кандидатов. Их
    # держим открытыми без общего доступа, пока сценарий не проверит, что
    # движок занятое пропускает, а не удаляет.
    $lockedPaths = @()
    if ($Context.LockFileCount -gt 0 -and $oldPaths.Count -gt 0) {
        $wanted = [int][Math]::Min($Context.LockFileCount, $oldPaths.Count)
        for ($index = 0; $index -lt $wanted; $index++) {
            $path = $oldPaths[$index]
            try {
                $handle = [System.IO.File]::Open(
                    $path,
                    [System.IO.FileMode]::Open,
                    [System.IO.FileAccess]::Read,
                    [System.IO.FileShare]::None)
                # [void] обязателен: Add возвращает индекс, и без него в вывод
                # функции попадает Int32 — а дальше $entry.files на массиве из
                # числа и словаря падает под StrictMode.
                [void]$script:Locks.Add($handle)
                $lockedPaths += $path
                $slot = $stats[$index % $stats.Count]
                [void]$slot.locked.Add($path)
            } catch {
                Write-Line ('{0}: не удалось занять «{1}»: {2}' -f $Record.Id, $path, $_.Exception.Message) 'warn'
            }
        }
    }

    $entry = [ordered]@{
        id                 = $Record.Id
        category           = $Record.Category
        safety             = $Record.Safety
        minAgeDays         = $Record.MinAgeDays
        ruleLocator        = $Record.RuleLocator
        files              = $totalFiles
        bytes              = $totalBytes
        freshFiles         = $freshCount
        candidateFiles     = $candidateFiles
        candidateBytes     = $candidateBytes
        oldestWriteUtc     = (Format-Stamp $oldest)
        newestWriteUtc     = (Format-Stamp $newest)
        lockedFiles        = $lockedPaths
        # Только каталоги, в которые реально легли файлы. Каталог без файлов не
        # создаётся, и объявлять его в манифесте значило бы расписать то, чего
        # на диске нет: сценарий потом сравнил бы список с обходом и сошёл бы
        # с ума. Случай «профилей больше, чем файлов» — warned выше.
        directories        = @($stats | Where-Object { $_.files -gt 0 } | ForEach-Object {
            [ordered]@{
                path           = $_.path
                files          = $_.files
                bytes          = $_.bytes
                candidateFiles = $_.candidateFiles
                candidateBytes = $_.candidateBytes
            }
        })
    }
    return $entry
}

function Format-Stamp {
    # Время в манифесте — всегда UTC в формате без локали и миллисекунд: его
    # читает и сравнивает Pester, и по нему потом считают возраст.
    param([datetime]$Stamp)
    if ($Stamp.Kind -eq [System.DateTimeKind]::Local) {
        $Stamp = $Stamp.ToUniversalTime()
    }
    return $Stamp.ToString('yyyy-MM-ddTHH:mm:ssZ', [System.Globalization.CultureInfo]::InvariantCulture)
}

# --------------------------------------------------------------------- пути --

function Get-FixtureRoot {
    # Корень по умолчанию — в %TEMP%: фикстура одноразовая, и класть её в
    # репозиторий незачем (tests\fixtures\ в SPEC §9 отведён под эталонные
    # файлы, а не под то, что удаляют проверками).
    param([string]$Value, [string]$LayoutName)
    if (-not [string]::IsNullOrEmpty($Value)) { return [System.IO.Path]::GetFullPath($Value) }
    return (Join-Path $env:TEMP 'mrproper-junk-fixture')
}

function Test-Elevated {
    # Проверка прав администратора. Нужна системным категориям в -Layout Real:
    # C:\Windows\Temp и C:\ProgramData без повышения не создать, и молча
    # пропустить их — значит отдать сценарию фикстуру без половины объёма.
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-RulesVersion {
    # Версия набора правил попадает в манифест, чтобы сценарий знал, против
    # чего мерить объём. Набор может быть не собран — это не повод падать.
    param([string]$Value)
    $directory = $Value
    if ([string]::IsNullOrEmpty($directory)) {
        $directory = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'rules'
    }
    $manifest = Join-Path $directory 'manifest.json'
    try {
        if (-not (Test-Path -LiteralPath $manifest)) { return $null }
        $data = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
        return [string]$data.version
    } catch {
        # Молчаливый null выглядел бы как «набора правил нет», а на деле означал
        # бы «не прочитался» — сценарий потом сравнивал бы объём с чужим набором.
        Write-Line ('версия набора правил не прочитана ({0}): {1}' -f $manifest, $_.Exception.Message) 'warn'
        return $null
    }
}

# ------------------------------------------------------------------ действия --

function Invoke-List {
    # Каталог для авторов сценариев: что просить, что получится и почему
    # некоторых категорий не будет. Столбец «правило» — дословный locator из
    # rules\*.json, чтобы не сверяться со скриптом, а с набором.
    $rows = @()
    foreach ($record in $script:Categories) {
        $rows += [ordered]@{
            id          = $record.Id
            category    = $record.Category
            safety      = $record.Safety
            minAgeDays  = $record.MinAgeDays
            ruleLocator = $record.RuleLocator
            supported   = ($record.Supported -ne $false)
            typical     = [int64]$record.TypicalBytes
            note        = $record.Note
            reason      = $record.Reason
        }
    }

    if ($Json) {
        Write-Json $rows 5
        return
    }

    Write-Line 'Категории фикстур (SPEC §4 FR-3, набор rules\*.json):'
    Write-Line ''
    Write-Line ('{0,-34} {1,-12} {2,-7} {3,-7} {4}' -f 'id', 'категория', 'риск', 'порог', 'под дерево кладём')
    foreach ($row in $rows) {
        if ($row.supported) {
            $record = Get-CategoryRecord $row.id
            $shape = $record.SubPath
            if ($null -ne $record.Names) { $shape = ($record.Names -join ', ') }
            elseif ($null -ne $record.Template) { $shape = $shape + '\' + $record.Template }
            Write-Line ('{0,-34} {1,-12} {2,-7} {3,-7} {4}' -f `
                    $row.id, $row.category, $row.safety, ('{0} дн' -f $row.minAgeDays), $shape)
        } else {
            Write-Line ('{0,-34} {1,-12} {2,-7} {3,-7} {4}' -f `
                    $row.id, $row.category, $row.safety, ('{0} дн' -f $row.minAgeDays), '— файлами не синтезируется —')
        }
    }
    Write-Line ''
    Write-Line 'Не поддерживается файловой фикстурой:'
    foreach ($row in $rows) {
        if (-not $row.supported) {
            Write-Line ('  {0}: {1}' -f $row.id, $row.reason) 'warn'
        }
    }
    Write-Line ''
    Write-Line ('Служебные имена: default — {0} категорий проверки Этапа 2, all — все поддерживаемые без тяжёлых и Risky.' -f `
            (@($script:Categories | Where-Object { $_.InDefault -eq $true }).Count))
    Write-Line 'Пример: -Category temp.user=64MB,logs.system=200,browser.cache.chrome=128MB+Default+Profile 1'
}

function Invoke-New {
    param([string]$RootValue)

    if ($Layout -eq 'Real' -and -not $AllowRealPaths) {
        throw (New-UsageError ('-Layout Real пишет в настоящие пути (%LOCALAPPDATA%, ' + $env:SystemRoot +
            ', %ProgramData%) и годится только на одноразовой VM. ' +
            'Подтвердите -AllowRealPaths, либо оставьте -Layout Mirror.'))
    }

    $root = Get-FixtureRoot $RootValue $Layout
    $requests = Resolve-Requests $Category $TotalBytes

    $records = @()
    foreach ($request in $requests) {
        $records += (Get-CategoryRecord $request.Id)
    }

    # Права и запреты проверяем до того, как что-либо создано: половина
    # готовой фикстуры из-за прав администратора хуже, чем отказ с текстом.
    if ($Layout -eq 'Real' -and $null -eq $script:PrefixesReal['windows']) {
        throw (New-UsageError 'раскладка Real недоступна: переменные %LOCALAPPDATA% или %SystemRoot% не заданы')
    }
    $needAdmin = @($records | Where-Object { $_.Admin -eq $true -and $_.Supported -ne $false })
    if ($Layout -eq 'Real' -and $needAdmin.Count -gt 0 -and -not (Test-Elevated)) {
        throw (New-UsageError ('системные категории ({0}) в -Layout Real требуют повышения прав: запустите консоль от администратора' -f `
                (($needAdmin | ForEach-Object { $_.Id }) -join ', ')))
    }

    $preview = $WhatIfPreference -eq $true
    if (-not $preview) {
        if (-not [System.IO.Directory]::Exists($root)) {
            [void][System.IO.Directory]::CreateDirectory($root)
        }
    }

    # Префикс дерева: в Mirror это корень фикстуры, в Real — настоящая система,
    # и тогда отдельного «корня» нет вовсе: каталоги раскладываются по своим
    # местам. Пишем это один раз, чтобы в отчёте не выглядело, что всё лежит
    # в одной папке.
    $base = $root
    if ($Layout -eq 'Mirror') {
        Write-Line ('корень фикстуры: {0}' -f $root)
    } else {
        Write-Line 'раскладка Real: файлы пишутся по настоящим путям (профиль, {0}, ProgramData)' -f $env:SystemRoot
    }

    # Буфер заполняется один раз и переиспользуется всеми файлами: содержимое
    # детерминированное, поэтому пересоздавать его на каждый файл незачем.
    $script:Rng = [uint32]$Seed
    $chunk = New-Object byte[] $script:ChunkBytes
    $fill = [uint32]2463534242
    for ($index = 0; $index -lt $chunk.Length; $index++) {
        $fill = [uint32](($fill * 1103515245 + 12345) % 4294967296)
        $chunk[$index] = [byte](($fill -shr 16) -band 0xFF)
    }

    # Список профилей разбираем здесь, а не binder'ом: `powershell -File`
    # передаёт «Default,Profile 1» одним аргументом, и без этого разбора
    # каталог назывался бы «Default,Profile 1» — одна директория вместо двух.
    $profiles = @()
    foreach ($entry in $Profile) {
        foreach ($piece in ($entry -split ',')) {
            $name = (Remove-OuterQuotes $piece)
            if ($name.Length -gt 0) { $profiles += $name }
        }
    }
    if ($profiles.Count -eq 0) { $profiles = @('Default') }

    $context = @{
        Base          = $base
        Layout        = $Layout
        FreshRatio    = $FreshRatio
        AgeJitterDays = $AgeJitterDays
        MaxFiles      = $MaxFilesPerCategory
        Profiles      = $profiles
        NowUtc        = [datetime]::UtcNow
        Chunk         = $chunk
        LockFileCount = $LockFileCount
    }

    $entries = @()
    $summary = @()
    foreach ($index in 0..($requests.Count - 1)) {
        $request = $requests[$index]
        $record = $records[$index]
        if ($record.Supported -eq $false) {
            throw (New-UsageError ('{0}: {1}' -f $record.Id, $record.Reason))
        }

        # Префикс зеркала — относительный путь (user\LocalAppData), поэтому
        # приклеиваем его к корню фикстуры. Пропуск Join-Path здесь стоил 12 МиБ
        # мусора в корне репозитория: Join-Path внутри New-JunkCategories склеил
        # «user\LocalAppData» с каталогом категории и путь остался
        # относительным, то есть относительным к рабочему каталогу процесса.
        $prefixRoot = Join-Path $root $script:PrefixesMirror[$record.Prefix]
        if ($Layout -eq 'Real') {
            $prefixRoot = $script:PrefixesReal[$record.Prefix]
        }
        if ([string]::IsNullOrEmpty($prefixRoot)) {
            throw (New-UsageError ('{0}: префикс {1} не разрешился в путь' -f $record.Id, $record.Prefix))
        }

        # Каталоги категории нужны до ShouldProcess: -WhatIf должен показать
        # точный список, а не «примерно здесь».
        $profiles = $request.Profiles
        if ($profiles.Count -eq 0) { $profiles = $context.Profiles }
        $directories = New-FixtureDirectories $record $profiles $prefixRoot
        $plannedBytes = $request.SizeBytes
        if ($null -eq $plannedBytes) { $plannedBytes = [int64]$request.FileCount * [int64]$record.TypicalBytes }

        # Страховка от повторения той же ошибки: в раскладке Mirror ничего не
        # должно оказаться вне -Root, даже если путь собрался относительным.
        # Проверяем до создания, а не после: убирать пришлось бы вручную.
        if ($Layout -eq 'Mirror') {
            $fence = $root.TrimEnd('\') + '\'
            foreach ($directory in $directories) {
                if (-not $directory.ToLowerInvariant().StartsWith($fence.ToLowerInvariant())) {
                    throw (New-UsageError ('{0}: каталог {1} вышел за корень фикстуры {2} — отказ, чтобы не писать мусор мимо него' -f `
                            $record.Id, $directory, $root))
                }
            }
        }

        if (-not $PSCmdlet.ShouldProcess(($directories -join '; '), ('создать мусор {0} ({1})' -f $record.Id, (Format-Bytes $plannedBytes)))) {
            Write-Line ('{0}: пропущено (-WhatIf)' -f $record.Id) 'warn'
            continue
        }

        Write-Line ('{0}: {1} — {2}' -f $record.Id, ($directories -join '; '), (Format-Bytes $plannedBytes))
        $entry = New-JunkCategory -Request $request -Record $record -Context $context -Base $prefixRoot
        $entries += $entry
        $summary += ('  {0,-32} файлов {1,-6} байт {2,-12} кандидатов {3}' -f `
                $record.Id, $entry.files, $entry.bytes, $entry.candidateFiles)
    }

    if ($preview) {
        Write-Line ''
        Write-Line '-WhatIf: ничего не создано, манифест не пишется.' 'warn'
        return
    }

    $totals = [ordered]@{
        categories     = $entries.Count
        files          = ($entries | ForEach-Object { $_.files } | Measure-Object -Sum).Sum
        bytes          = [int64](($entries | ForEach-Object { $_.bytes } | Measure-Object -Sum).Sum)
        candidateFiles = ($entries | ForEach-Object { $_.candidateFiles } | Measure-Object -Sum).Sum
        candidateBytes = [int64](($entries | ForEach-Object { $_.candidateBytes } | Measure-Object -Sum).Sum)
    }

    $manifest = [ordered]@{
        schemaVersion = 1
        generator     = 'tests/e2e/New-JunkFixture.ps1'
        createdUtc     = (Format-Stamp ([datetime]::UtcNow))
        layout         = $Layout
        root           = $root
        seed           = $Seed
        freshRatio     = $FreshRatio
        ageJitterDays  = $AgeJitterDays
        rulesVersion   = (Get-RulesVersion $RepoRoot)
        # Оговорка для сценария: allocatedBytes отдаёт файловая система, и
        # файл меньше кластера округляется вверх (NTFS — 4 КиБ), поэтому
        # сверять сумму надо с допуском, а не в байт.
        bytesNote      = 'bytes/candidateBytes are logical sizes; allocated >= logical, NTFS rounds up to 4 KiB clusters'
        totals         = $totals
        categories     = $entries
    }

    $manifestPath = Join-Path $root 'fixture-manifest.json'
    # Манифест пишем в UTF-8 без BOM и только ASCII: его читает и Pester, и
    # возможно будущий golden-тест на C++, а BOM в начале JSON ломает
    # часть парсеров. Русские пояснения в манифест не кладутся намеренно.
    # Локальная переменная НЕ может называться $json: имена в PowerShell не
    # различают регистр, и присваивание $json = ... включало переключатель
    # -Json, после чего манифест печатался в stdout без всякого -Json.
    $document = $manifest | ConvertTo-Json -Depth 8
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($manifestPath, $document, $utf8)

    Write-Line ''
    foreach ($line in $summary) { Write-Line $line }
    Write-Line ('  {0,-32} байт {1,-12} кандидатов байт {2}' -f 'ИТОГО', (Format-Bytes $totals.bytes), (Format-Bytes $totals.candidateBytes))
    Write-Line ('  файлов всего {0}, кандидатов {1}' -f $totals.files, $totals.candidateFiles)
    Write-Line ('манифест: {0}' -f $manifestPath)
    if ($totals.candidateBytes -gt 0) {
        Write-Line ('допуск 0.5 % от {0} = {1}' -f (Format-Bytes $totals.candidateBytes), (Format-Bytes ([int64]($totals.candidateBytes * 0.005)))) 'step'
    }

    if ($Json) { Write-Json $manifest 8 }

    if ($HoldLockedSeconds -gt 0 -and $script:Locks.Count -gt 0) {
        Write-Line ('занято файлов: {0}, держу {1} с — сценарий должен успеть просканировать' -f $script:Locks.Count, $HoldLockedSeconds)
        Start-Sleep -Seconds $HoldLockedSeconds
    }
    Write-Line 'готово' 'ok'
}

function Remove-EmptyTree {
    # Снимает пустые каталоги снизу вверх и возвращает то, что удалить не
    # вышло (то есть непустое). Рекурсия отключена намеренно: пустой каталог
    # удаляется одним вызовом Directory.Delete(path, false), а на непустом
    # вызов бросает исключение — так чужие файлы под -Root останутся нетронутыми
    # ни при каком сценарии. Зеркала фикстуры многоуровневые
    # (user\LocalAppData\Temp), поэтому одного прохода по верхнему уровню мало.
    param([Parameter(Mandatory = $true)][string]$Path)

    $left = @()
    if (-not [System.IO.Directory]::Exists($Path)) { return $left }

    foreach ($child in [System.IO.Directory]::GetDirectories($Path)) {
        $left += Remove-EmptyTree $child
    }
    if ($left.Count -gt 0) { return $left }

    try {
        [System.IO.Directory]::Delete($Path, $false)
    } catch {
        $left += $Path
    }
    return $left
}

function Invoke-Remove {
    # Снос созданного дерева. Проверка по манифесту обязательна: -Root с
    # опечаткой (или с именем каталога, который скрипт когда-то не трогал) не
    # должен превращаться в rm -rf чужого.
    param([string]$RootValue)

    $root = Get-FixtureRoot $RootValue $Layout
    $manifest = Join-Path $root 'fixture-manifest.json'
    if (-not [System.IO.Directory]::Exists($root)) {
        Write-Line ('нет каталога {0} — удалять нечего' -f $root) 'warn'
        return
    }
    if (-not [System.IO.File]::Exists($manifest)) {
        throw (New-UsageError (('в {0} нет fixture-manifest.json: это не фикстура этого скрипта, удалять отказываюсь. ' +
               'Если каталог всё-таки мусорный — удалите его вручную.') -f $root))
    }

    $data = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
    $dirs = @($data.categories | ForEach-Object { $_.directories } | ForEach-Object { $_.path })
    $count = 0
    foreach ($dir in $dirs) {
        if ([System.IO.Directory]::Exists($dir) -and $PSCmdlet.ShouldProcess($dir, 'удалить каталог фикстуры')) {
            [System.IO.Directory]::Delete($dir, $true)
            $count = $count + 1
        }
    }
    # Манифест, зеркала и корень удаляются только пока каталоги пусты:
    # рекурсии нет (см. Remove-EmptyTree), поэтому всё, что сценарий положил в
    # -Root помимо фикстуры, останется нетронутым. Раньше зеркала переживали
    # уборку, и корень фикстуры не исчезал — повторный -Action Remove находил
    # пустоту.
    try { [System.IO.File]::Delete($manifest) } catch { }
    $left = @()
    foreach ($mirror in @('user', 'system')) {
        $path = Join-Path $root $mirror
        if ([System.IO.Directory]::Exists($path)) {
            $left += Remove-EmptyTree $path
        }
    }
    if ($left.Count -eq 0 -and [System.IO.Directory]::Exists($root)) {
        try {
            [System.IO.Directory]::Delete($root, $false)
            $count = $count + 1
        } catch {
            $left += $root
        }
    }
    foreach ($path in $left) {
        Write-Line ('не пусто, оставлено: {0}' -f $path) 'warn'
    }
    Write-Line ('удалено каталогов: {0}' -f $count) 'ok'
}

function Invoke-SelfTest {
    # Проверка скрипта, которой можно верить. Сначала разбор парсером PowerShell
    # (критерий приёмки задачи: файл должен разбираться без ошибок), затем
    # маленькая фикстура в %TEMP% с проверкой по факту: сколько байт и файлов
    # легло на диск, какие файлы молодые, какие старые, совпал ли манифест.
    $failures = @()

    $parseErrors = $null
    $tokens = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $PSCommandPath, [ref]$tokens, [ref]$parseErrors)
    if ($null -ne $parseErrors -and $parseErrors.Count -gt 0) {
        foreach ($error in $parseErrors) { $failures += ('разбор: {0}' -f $error.Message) }
        Write-Line 'разбор файла: ОШИБКИ' 'err'
    } else {
        Write-Line ('разбор файла: ок, токенов {0}' -f $tokens.Count) 'ok'
    }

    # Три категории с разными порогами возраста: 2, 14 и 30 дней. У каждой
    # задана ровно одна величина — либо объём, либо число файлов: если задать
    # обе, проверять нечего, кроме того, что приоритет у одной из них есть.
    $root = Join-Path $env:TEMP ('mrproper-junk-selftest-{0}' -f $PID)
    if ([System.IO.Directory]::Exists($root)) {
        [System.IO.Directory]::Delete($root, $true)
    }

    $script:Rng = [uint32]20260928
    $chunk = New-Object byte[] 65536
    for ($index = 0; $index -lt $chunk.Length; $index++) {
        $chunk[$index] = [byte](($index * 7) -band 0xFF)
    }
    $now = [datetime]::UtcNow

    $context = @{
        Base          = $root
        Layout        = 'Mirror'
        FreshRatio    = 0.25
        AgeJitterDays = 30
        MaxFiles      = 1000
        Profiles      = @('Default', 'Profile 1')
        NowUtc        = $now
        Chunk         = $chunk
        LockFileCount = 0
    }

    $cases = @(
        @{ Id = 'temp.user';       Bytes = 4MB;  Files = 0; MinAge = 2 }
        @{ Id = 'logs.system';     Bytes = 0;    Files = 4; MinAge = 14 }
        @{ Id = 'crash.dumps.app'; Bytes = 0;    Files = 3; MinAge = 30 }
    )

    $entries = @()
    $allDirectories = @()
    foreach ($case in $cases) {
        $record = Get-CategoryRecord $case.Id
        $request = Resolve-Request $case.Id
        if ($case.Bytes -gt 0) { $request.SizeBytes = [int64]$case.Bytes }
        if ($case.Files -gt 0) { $request.FileCount = [int]$case.Files }
        # Порог из правила обязан совпасть с ожиданием проверки: если правило
        # поменяют, упадёт тест, а не молча пройдёт с другим числом.
        if ($record.MinAgeDays -ne $case.MinAge) {
            $failures += ('{0}: порог в каталоге {1} дн, тест ждёт {2}' -f $case.Id, $record.MinAgeDays, $case.MinAge)
        }
        $prefixRoot = Join-Path $root $script:PrefixesMirror[$record.Prefix]
        $entry = New-JunkCategory -Request $request -Record $record -Context $context -Base $prefixRoot
        $entries += $entry

        # Каталог обязан лежать под своим префиксом-зеркалом. Проверка не
        # косметическая: без неё потерянный префикс даёт две категории в одном
        # каталоге, и сценарий «все шесть категорий» проходит на пяти.
        $expectedPrefix = $prefixRoot.ToLowerInvariant() + '\'
        foreach ($slot in $entry.directories) {
            if (-not $slot.path.ToLowerInvariant().StartsWith($expectedPrefix)) {
                $failures += ('{0}: каталог {1} вне зеркала {2}' -f $case.Id, $slot.path, $prefixRoot)
            }
            $allDirectories += $slot.path.ToLowerInvariant()
        }
    }
    $unique = @($allDirectories | Sort-Object -Unique)
    if ($unique.Count -ne $allDirectories.Count) {
        $failures += ('категории делят каталоги: {0} записей, {1} уникальных' -f $allDirectories.Count, $unique.Count)
    }

    foreach ($entry in $entries) {
        $case = $cases | Where-Object { $_.Id -eq $entry.id }
        $expectedBytes = [int64]$case.Bytes
        if ($expectedBytes -eq 0) { $expectedBytes = [int64]$case.Files * [int64](Get-CategoryRecord $case.Id).TypicalBytes }
        $expectedFiles = [int]$case.Files
        if ($expectedFiles -eq 0) {
            $expectedFiles = [int][Math]::Ceiling($expectedBytes / [int64](Get-CategoryRecord $case.Id).TypicalBytes)
        }

        # Считаем по диску, а не по манифесту: расхождение манифеста с диском и
        # есть тот баг, который ловит эта проверка.
        $onDisk = [int64]0
        $filesOnDisk = 0
        $underDay = 0
        $underThreshold = 0
        $underThresholdBytes = [int64]0
        foreach ($slot in $entry.directories) {
            if (-not [System.IO.Directory]::Exists($slot.path)) {
                $failures += ('{0}: каталога нет {1}' -f $entry.id, $slot.path)
                continue
            }
            foreach ($file in [System.IO.Directory]::GetFiles($slot.path)) {
                $info = New-Object System.IO.FileInfo($file)
                $onDisk = $onDisk + $info.Length
                $filesOnDisk = $filesOnDisk + 1
                $ageDays = ($now - $info.LastWriteTimeUtc).TotalDays
                if ($ageDays -lt 1.0) { $underDay = $underDay + 1 }
                if ($ageDays -lt $entry.minAgeDays) {
                    $underThreshold = $underThreshold + 1
                    $underThresholdBytes = $underThresholdBytes + $info.Length
                }
            }
        }

        if ($filesOnDisk -ne $expectedFiles) {
            $failures += ('{0}: файлов на диске {1}, ждали {2}' -f $entry.id, $filesOnDisk, $expectedFiles)
        }
        if ($onDisk -ne $expectedBytes) {
            $failures += ('{0}: байт на диске {1}, ждали {2}' -f $entry.id, $onDisk, $expectedBytes)
        }
        if ($entry.bytes -ne $onDisk) {
            $failures += ('{0}: манифест говорит {1} байт, на диске {2}' -f $entry.id, $entry.bytes, $onDisk)
        }
        if ($entry.files -ne $filesOnDisk) {
            $failures += ('{0}: манифест говорит {1} файлов, на диске {2}' -f $entry.id, $entry.files, $filesOnDisk)
        }
        # Свежие файлы (FreshRatio 0.25) обязаны быть моложе порога, и таких
        # должно быть ровно столько, сколько насчитано манифестом. Отдельно
        # проверяем, что «старые» действительно старше порога, а не просто не
        # попали в первый счётчик.
        $expectedFresh = $entry.freshFiles
        if ($underThreshold -ne $expectedFresh) {
            $failures += ('{0}: моложе порога {1} дн оказалось {2} файлов, манифест обещал свежих {3}' -f `
                    $entry.id, $entry.minAgeDays, $underThreshold, $expectedFresh)
        }
        if ($entry.candidateFiles -ne ($filesOnDisk - $underThreshold)) {
            $failures += ('{0}: кандидатов в манифесте {1}, по диску {2}' -f `
                    $entry.id, $entry.candidateFiles, ($filesOnDisk - $underThreshold))
        }
        if ($entry.candidateBytes -ne ($onDisk - $underThresholdBytes)) {
            $failures += ('{0}: байт кандидатов в манифесте {1}, по диску {2}' -f `
                    $entry.id, $entry.candidateBytes, ($onDisk - $underThresholdBytes))
        }
        Write-Line ('{0,-20} файлов {1,-4} байт {2,-10} моложе суток {3,-3} кандидатов {4}' -f `
                $entry.id, $filesOnDisk, $onDisk, $underDay, ($filesOnDisk - $underThreshold))
    }

    # Много профилей: правила браузеров группируют кандидатов по профилю, и
    # фикстура обязана создать каталог на каждый. Объёма берём ровно на два
    # файла — по типичному размеру кэша, — чтобы проверить ещё и раскладку
    # «по одному файлу на профиль», а не только число каталогов.
    $record = Get-CategoryRecord 'browser.cache.chrome'
    $perProfile = [int64]$record.TypicalBytes
    $cacheEntry = New-JunkCategory -Request (Resolve-Request ('browser.cache.chrome={0}b' -f ($perProfile * 2))) -Record $record `
        -Context $context -Base (Join-Path $root $script:PrefixesMirror[$record.Prefix])
    if ($cacheEntry.directories.Count -ne 2) {
        $failures += ('browser.cache.chrome: каталогов с файлами {0}, ждали по одному на каждый из двух профилей' -f $cacheEntry.directories.Count)
    }
    foreach ($slot in $cacheEntry.directories) {
        if ($slot.files -ne 1) {
            $failures += ('browser.cache.chrome: в «{0}» легло файлов {1}, ждали ровно 1' -f $slot.path, $slot.files)
        }
    }
    Write-Line ('browser.cache.chrome: каталогов с файлами — {0}, по {1} файла в каждом' -f `
            $cacheEntry.directories.Count, (($cacheEntry.directories | ForEach-Object { $_.files }) -join '/'))

    # Каталог, в который не легло ни одного файла, не создаётся и в манифесте
    # не значится: сценарий сверяет манифест с обходом диска.
    $record = Get-CategoryRecord 'browser.cache.edge'
    $edgeEntry = New-JunkCategory -Request (Resolve-Request 'browser.cache.edge=1') -Record $record `
        -Context $context -Base (Join-Path $root $script:PrefixesMirror[$record.Prefix])
    if ($edgeEntry.directories.Count -ne 1) {
        $failures += ('browser.cache.edge: при одном файле и двух профилях в манифесте {0} каталогов, ждали 1' -f $edgeEntry.directories.Count)
    }
    Write-Line ('browser.cache.edge: 1 файл на 2 профиля — каталогов в манифесте {0}' -f $edgeEntry.directories.Count)

    # Ошибки использования обязаны быть ошибками, а не пустой фикстурой:
    # сценарий, где категория не нашлась или объём не разобрался, должен упасть
    # на подготовке. Проверяются на своём слое: разбор «id=величина» живёт в
    # Resolve-Request, а поиск категории по имени — в Get-CategoryRecord.
    $rejected = 0
    $attempted = 0
    foreach ($bad in @(
        @{ Spec = 'temp.user=непонятно'; Layer = 'request' }
        @{ Spec = 'temp.user=0MB';       Layer = 'request' }
        @{ Spec = 'такой-категории-нет';  Layer = 'record' }
    )) {
        $attempted = $attempted + 1
        try {
            if ($bad.Layer -eq 'request') {
                [void](Resolve-Request $bad.Spec)
            } else {
                [void](Get-CategoryRecord $bad.Spec)
            }
        } catch {
            $rejected = $rejected + 1
        }
    }
    if ($rejected -ne $attempted) {
        $failures += ('мусорные -Category приняты: отклонено {0} из {1}' -f $rejected, $attempted)
    }
    Write-Line ('мусорные -Category отклонены: {0} из {1}' -f $rejected, $attempted)

    # Уборка за собой через сам Invoke-Remove: сценарий должен иметь
    # возможность снести фикстуру одной командой, поэтому проверяем, что
    # команда действительно убирает корень целиком, а не оставляет скелет из
    # зеркал (на этом ломалось удаление, и повторный Remove был уже не нужен).
    $removeRoot = Join-Path $env:TEMP ('mrproper-junk-remove-{0}' -f $PID)
    $removeCategory = Join-Path $removeRoot 'user\LocalAppData\Temp'
    [void][System.IO.Directory]::CreateDirectory($removeCategory)
    $removeFile = Join-Path $removeCategory 'tmp-000001.tmp'
    New-FixtureFile $removeFile 1024 $now $chunk
    $removeDoc = [ordered]@{
        schemaVersion = 1
        categories    = @([ordered]@{
            id          = 'temp.user'
            directories = @([ordered]@{ path = $removeCategory; files = 1; bytes = 1024 })
        })
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $removeRoot 'fixture-manifest.json'),
        ($removeDoc | ConvertTo-Json -Depth 6),
        (New-Object System.Text.UTF8Encoding($false)))

    Invoke-Remove $removeRoot
    if ([System.IO.Directory]::Exists($removeRoot)) {
        $failures += ('-Action Remove не убрал корень фикстуры: {0}' -f $removeRoot)
    }
    Write-Line ('-Action Remove: корень {0}' -f $(if ([System.IO.Directory]::Exists($removeRoot)) { 'остался' } else { 'убран' }))

    # Уборка за собой. Фикстура self-теста столько не стоит, чтобы её бросить
    # в %TEMP% на следующий прогон.
    try {
        if ([System.IO.Directory]::Exists($root)) {
            [System.IO.Directory]::Delete($root, $true)
        }
    } catch {
        $failures += ('не удалось убрать {0}: {1}' -f $root, $_.Exception.Message)
    }

    if ($failures.Count -gt 0) {
        foreach ($failure in $failures) { Write-Line $failure 'err' }
        throw ('self-тест не прошёл: замечаний {0}' -f $failures.Count)
    }
    Write-Line 'self-тест: ок' 'ok'
}

# --------------------------------------------------------------- диспетчер ---

# Открытые дескрипторы занятых файлов. Держим в списке, а не в локальной
# переменной: файлы должны оставаться занятыми до конца действия, а сборщик
# закроет их сам при выходе из скрипта.
$script:Locks = New-Object System.Collections.ArrayList

try {
    switch ($Action) {
        'List' { Invoke-List }
        'New' { Invoke-New $Root }
        'Remove' { Invoke-Remove $Root }
        'SelfTest' { Invoke-SelfTest }
        default { throw (New-UsageError ('неизвестное действие {0}' -f $Action)) }
    }
} catch {
    # Классификация по типу: New-UsageError — код 2, всё остальное (сбой
    # файловой системы, несошедшийся self-тест) — код 1.
    $code = 1
    if ($_.Exception -is [System.ArgumentException]) {
        $code = 2
    }
    Write-Fail $_.Exception.Message $code
}

foreach ($handle in $script:Locks) {
    try { $handle.Dispose() } catch { }
}

exit $script:ExitCode
