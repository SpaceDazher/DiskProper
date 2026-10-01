// mrproper-cli: каркас процесса — реализация (SPEC §6.2, §8 Этап 0, §5, §12).
//
// Разбор командной строки, тексты --version и --help, реестр команд и точка
// входа runCli. Что именно за это отвечает и почему — в args.hpp; здесь только
// реализация, по тому же принципу, по которому cmd_scan.cpp повторяет контракт
// cmd_scan.hpp.
//
// Слой переносимый: ни windows.h, ни COM (SPEC §6.1, ADR-004). Единственное,
// что знает про машину, — код возврата команды. Проверяется юнит-тестом на
// любом хосте (§11.1).
//
// Написание новой команды — это одна строка в kCommands плюс одна ветка в
// dispatchCommand(). Больше каркас про неё не знает и знать не должен: разбор
// ключей, вывод и коды остаются у команды (SPEC §6.2: «cli зависит от engine»,
// а не наоборот).
#include "args.hpp"

#include <cctype>
#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cmd_apply.hpp"
#include "cmd_disks.hpp"
#include "cmd_rules.hpp"
#include "cmd_scan.hpp"
#include "cmd_undo.hpp"

namespace mrproper::cli {

namespace {

// Объявление cmdReport живёт в args.hpp: файл cmd_report.cpp самодостаточен, и
// перечислять его здесь ещё раз значило бы завести третью правду об одном
// API. У cmd_rules.cpp заголовок теперь есть (cmd_rules.hpp), и он подключён
// выше: контракт команды — её подкоманды, коды возврата и шов для подписи —
// объявлен там, где команда живёт, а повторное объявление cmdRules в args.hpp
// (оно осталось) компиляции не мешает: расходиться может только определение.

// Версия приложения. CMake обязан передать её из project(MrProper VERSION
// …) — иначе версия в --version и в отчёте разъедутся (packaging/README.md
// §3). Без определения берётся значение, совпадающее с project() сейчас:
// печатать «0.0.0» при непрочитанном макросе хуже, чем показать ту версию,
// которую действительно собирают.
#if defined(MRPROPER_VERSION_STRING)
constexpr const char* kAppVersion = MRPROPER_VERSION_STRING;
#else
constexpr const char* kAppVersion = "0.1.0";
#endif

// Конфигурация сборки: сообщение в --version должно отличать Debug от Release,
// иначе баг-репорт из CI не отличить от прогона разработчика. При
// многоконфигурационном генераторе (Visual Studio) CMAKE_BUILD_TYPE пуст,
// поэтому значение приходит генераторным выражением из src/cli/CMakeLists.txt.
#if defined(MRPROPER_BUILD_TYPE)
constexpr const char* kBuildType = MRPROPER_BUILD_TYPE;
#else
constexpr const char* kBuildType = "unknown";
#endif

// Ширина колонки описаний в общей справке. Фиксирована, а не «по самой
// длинной строке»: --help должен выглядеть одинаково от запуска к запуску и
// не прыгать при добавлении команды.
constexpr std::size_t kSummaryColumn = 8u;

// ASCII в нижний регистр. Не std::tolower с явным приведением: он зависит от
// текущей локали процесса, а сравнение имён команд не должно зависеть от
// того, какая кодовая страница активна в консоли (§5 «Локализация»). В ASCII
// регистр меняет только A-Z, поэтому кириллица и пути не затрагиваются.
std::string toLowerAscii(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(byte < 0x80u ? static_cast<char>(std::tolower(byte)) : character);
    }
    return result;
}

// Аргумент из одних пробелов (или пустой) — не команда. Молча проглотить его
// нельзя, как и трактовать как имя команды: ошибка с точным указанием места
// дешевле прогона не той команды.
bool isBlank(std::string_view text) {
    for (const char character : text) {
        if (!std::isspace(static_cast<unsigned char>(character))) return false;
    }
    return true;
}

// Реестр команд. Порядок — порядок справки: от «что посмотреть» к «что
// изменить». detail непуст у команды, которая объявлена спекой, но ещё не
// подключена: молчаливый «успех» на такой команде опаснее явного отказа,
// потому что скрипт решит, что отчёт получен.
const std::vector<CommandDescription> kCommands{
    {Command::Scan, "scan", "скан мусора по набору правил, отчёт в JSON (§4 FR-3, FR-4)", ""},
    {Command::Plan, "plan", "показать план очистки и ничего не удалить (§4 FR-5)", ""},
    {Command::Apply,
     "apply",
     "очистка с подтверждением; реальное удаление — только с --execute (FR-5, FR-6)",
     ""},
    {Command::Report, "report", "отчёт scan/plan/apply в нормализованный JSON или самодостаточный HTML (FR-8)",
     ""},
    // Команда отмены (FR-7, §7.2): список транзакций корзины и возврат файлов по
    // идентификатору. Стоит рядом с apply, потому что отменяет именно его:
    // apply кладёт в корзину, undo возвращает.
    {Command::Undo, "undo", "отмена очистки: транзакции корзины и возврат файлов (FR-7, §7.2)", ""},
    // Подкоманды rules перечисляет сама команда (её справка печатается из
    // cmd_rules.cpp), здесь — только то, что человек видит в общем списке.
    // «update» в списке не было никогда реализовано, и объявлять его значило
    // бы обещать несуществующую команду: «rules update» молча разбирался как
    // проверка каталога с именем «update». Теперь такой вызов даёт явный отказ
    // с кодом 69, а обновление набора живёт там, где оно и происходит по
    // §9.2, — в rulesync при запуске и в настройках.
    {Command::Rules, "rules", "validate | verify: набор правил разбирается и проверяется по подписи (§9.2, ADR-008)",
     ""},
    {Command::Disks, "disks", "карта разделов в JSON — эталонный дамп для сравнения (§4 FR-1, §8 Этап 1)",
     ""},
};

const CommandDescription* findDescription(Command command) {
    for (const CommandDescription& description : kCommands) {
        if (description.id == command) return &description;
    }
    return nullptr;
}

// Отказ по неподключённой или неизвестной команде. Один текст на оба случая:
// различие только в причине, а различать их в stderr полезно не настолько,
// чтобы держать две строки, которые однажды разъедутся.
int reportUnavailable(const std::string& name, Command command, const CliStreams& streams) {
    const CommandDescription* description = findDescription(command);
    if (command == Command::Unknown || description == nullptr) {
        streams.err << kProgramName << ": ОШИБКА: неизвестная команда: '" << name << "'\n"
                    << kProgramName << ": подсказка: " << kProgramName << " --help\n";
        return static_cast<int>(CliExit::Usage);
    }
    streams.err << kProgramName << ": ОШИБКА: команда '" << description->name << "' не готова: " << description->detail
                << "\n"
                << kProgramName << ": подсказка: " << kProgramName << " --help\n";
    return static_cast<int>(CliExit::Usage);
}

// Собирает то, что каркас знает про scan, — и ничего сверх того. Сканеру нужен
// адаптер обхода ФС (engine::FileSystemProbe над platform::vfs), а моста в
// репозитории нет: он не принадлежит ни одной задаче волны, и требования к
// нему перечислены в шапке cmd_scan.cpp. Подключать выдуманный обход здесь
// нельзя — команда без адаптера печатает точное требование в stderr и
// возвращает ScanFailed, а не печатает «нулевой» отчёт, который CI принял бы
// за «мусора нет». Версию приложения в отчёт отдаём: она известна точно.
ScanServices makeScanServices() {
    ScanServices services;
    services.appVersion = kAppVersion;
    return services;
}

int runPlanOrApply(const CommandLine& parsed, const CliStreams& streams) {
    const ApplyIo io{streams.out, streams.err, streams.in};
    // Кандидаты — из готового JSON (--candidates): источника от живого скана у
    // каркаса нет по той же причине, что и у scan. Исполнителя операций тоже
    // нет, поэтому --execute честно отказывает (NoExecutor), а не изображает
    // очистку.
    const ApplyEnvironment environment = makeFileEnvironment();
    if (parsed.command == Command::Plan) {
        return runPlanCommand(parsed.args, io, environment);
    }
    return runApplyCommand(parsed.args, io, environment);
}

// Ветка на команду. Коды команд уходят как есть — каркас их не переводит
// (см. args.hpp). Ни одна ветка не бросает наружу: команды ловят свои
// исключения сами, а граница процесса в main.cpp.
int dispatchCommand(const CommandLine& parsed, const CliStreams& streams) {
    switch (parsed.command) {
        case Command::Scan:
            return runScan(parsed.args, makeScanServices(), streams.out, streams.err);
        case Command::Disks:
            // Инвентаризация дисков умеет ходить в платформу сама: адаптер
            // makePlatformDisksServices() — её собственный (FR-1), в отличие от
            // scan, где адаптера обхода ФС в репозитории пока нет.
            return runDisks(parsed.args, makePlatformDisksServices(), streams.out, streams.err);
        case Command::Plan:
        case Command::Apply:
            return runPlanOrApply(parsed, streams);
        case Command::Report:
            return cmdReport(parsed.args, streams.out, streams.err);
        case Command::Undo: {
            // Отмена подключается к своему движку здесь и только здесь: движок
            // восстановления (engine::undo_service) нужен слою cli для одной
            // команды, и подключать его в каркасе для всех нельзя.
            const UndoIo io{streams.out, streams.err, streams.in};
            return runUndoCommand(parsed.args, io, makeFileUndoEnvironment());
        }
        case Command::Rules:
            return cmdRules(parsed.args, streams.out, streams.err);
        // Сюда попадает только команда, объявленная, но не подключённая: каркас
        // обязан сказать об этом прямо, а не изображать успех (см. kCommands).
        case Command::Unknown:
        case Command::None:
        case Command::Help:
        case Command::Version:
            break;
    }
    return reportUnavailable(parsed.name, parsed.command, streams);
}

}  // namespace

// ---------------------------------------------------------------------------
// Версия и команды
// ---------------------------------------------------------------------------

const char* appVersion() noexcept { return kAppVersion; }

std::string versionText() {
    // Разрядность — sizeof(void*), а не макрос: одна правда, и она не может
    // разойтись с той, что собрана. Имя ОС читать не нужно: версия обязана
    // печататься всегда, даже когда реестр и WMI недоступны (§5 «Устойчивость»).
    const char* const bits = (sizeof(void*) == 8u) ? "x64" : "x86";
    std::string text;
    text += kProgramName;
    text += ' ';
    text += kAppVersion;
    text += "\nMrProper — десктоп-утилита очистки диска для Windows 10/11 (SPEC §6.2)\n";
    text += "Конфигурация сборки: ";
    text += kBuildType;
    text += ", платформа: Windows ";
    text += bits;
    text += '\n';
    return text;
}

const std::vector<CommandDescription>& commandTable() noexcept { return kCommands; }

Command commandByName(std::string_view name) noexcept {
    // Порядок совпадает с kCommands: добавление команды в реестр без строки
    // здесь даст команду, которая печатается в справке, но не вызывается, —
    // это лучше, чем обратное.
    if (name == "scan") return Command::Scan;
    if (name == "plan") return Command::Plan;
    if (name == "apply") return Command::Apply;
    if (name == "report") return Command::Report;
    if (name == "undo") return Command::Undo;
    if (name == "rules") return Command::Rules;
    if (name == "disks") return Command::Disks;
    if (name == "help") return Command::Help;
    if (name == "version") return Command::Version;
    return Command::Unknown;
}

const char* commandName(Command command) noexcept {
    if (const CommandDescription* description = findDescription(command)) return description->name;
    switch (command) {
        case Command::Help:
            return "help";
        case Command::Version:
            return "version";
        case Command::None:
        case Command::Unknown:
            break;
    }
    return "?";
}

bool isAvailable(Command command) noexcept {
    const CommandDescription* description = findDescription(command);
    return description != nullptr && description->detail[0] == '\0';
}

// ---------------------------------------------------------------------------
// Разбор командной строки
// ---------------------------------------------------------------------------

bool parseCommandLine(const std::vector<std::string>& argv, CommandLine& parsed, std::string& error) {
    parsed = CommandLine{};
    error.clear();

    bool nameOnly = false;  // после «--» ключи каркаса не распознаются

    for (std::size_t index = 0; index < argv.size(); ++index) {
        const std::string& argument = argv[index];

        if (isBlank(argument)) {
            error = "пустой аргумент (позиция " + std::to_string(index + 1u) + ")";
            return false;
        }

        if (!nameOnly) {
            if (argument == "--") {
                // Разделитель перед именем команды: ключи после него командами
                // каркаса не читаются. Нужен ровно один случай — файл
                // программы переименовали, а вызвать надо именно этот бинарь.
                nameOnly = true;
                continue;
            }

            // Ключи каркаса. Регистр не важен: -V и -v одно и то же, имя
            // набирает человек, а в скриптах оно встречается с обеих сторон.
            const std::string lowered = toLowerAscii(argument);
            if (lowered == "--help" || lowered == "-h" || lowered == "help") {
                parsed.command = Command::Help;
                parsed.name = "help";
                // «help <команда>»: остаток — имя команды, для которой нужна
                // справка. Больше одного аргумента у help не бывает: второй
                // молча проигнорированный был бы хуже явной ошибки, но
                // исправлять его нечем — печатать будем справку по первому.
                if (index + 1u < argv.size()) {
                    parsed.args.emplace_back(argv[index + 1u]);
                }
                return true;
            }
            if (lowered == "--version" || lowered == "-v" || lowered == "-version") {
                parsed.command = Command::Version;
                parsed.name = "version";
                return true;
            }
        }

        // Не ключ каркаса — значит, имя команды. Всё, что после, уходит
        // дословно: разбор ключей команды не наша забота (см. args.hpp).
        const Command command = commandByName(toLowerAscii(argument));
        if (command == Command::Unknown) {
            error = "неизвестная команда: '" + argument + "'";
            return false;
        }
        parsed.command = command;
        parsed.name = argument;
        parsed.args.assign(argv.begin() + static_cast<std::ptrdiff_t>(index) + 1, argv.end());
        return true;
    }

    // Пустая командная строка или один «--»: решения не принято, а решает
    // вызывающий (справка в stderr и код Usage).
    parsed.command = Command::None;
    parsed.name.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Запуск
// ---------------------------------------------------------------------------

std::string usageText() {
    std::string text;
    text += "Использование: ";
    text += kProgramName;
    text += " <команда> [опции]\n       ";
    text += kProgramName;
    text += " --version\n       ";
    text += kProgramName;
    text += " help [команда]\n"
            "\n"
            "Headless-режим MrProper для CI и e2e (SPEC §6.2): тот же движок, что и у\n"
            "приложения, но без окна. Машинный вывод (JSON) идёт в stdout, человеческий\n"
            "текст и ошибки — в stderr: `mrproper-cli scan --json | jq` не должен падать\n"
            "на строке прогресса.\n"
            "\n"
            "Команды:\n";

    for (const CommandDescription& description : kCommands) {
        const std::size_t nameLength = std::string_view(description.name).size();
        const std::size_t padding = (nameLength < kSummaryColumn) ? (kSummaryColumn - nameLength) : 1u;
        text += "  ";
        text += description.name;
        text.append(padding, ' ');
        text += description.summary;
        if (description.detail[0] != '\0') {
            text += "\n           ";
            text.append(padding, ' ');
            text += "не подключено: ";
            text += description.detail;
        }
        text += '\n';
    }

    text += "  help       справка: эта, по команде или по ключу команды\n"
            "\n"
            "Ключи каркаса (пишутся ДО имени команды; после — ключи самой команды):\n"
            "  -h, --help        эта справка; `mrproper-cli <команда> --help` — справка команды\n"
            "  -V, -v, --version версия приложения\n"
            "  help [команда]   то же, что --help\n"
            "\n"
            "Коды возврата каркаса:\n"
            "  0   команда отработала; успех означает её собственный код\n"
            "  2   команда не задана, неизвестна или ещё не подключена\n"
            "  70  необработанное исключение на границе процесса (EX_SOFTWARE)\n"
            "  Коды команд каркас не переводит: scan — 0/2/3/4/130, plan и apply —\n"
            "  0/2/3/4/5/6, undo — 0/2/3/4/5/6/7 (4 — отменять нечего, 5 — корень\n"
            "  корзины неизвестен), report и rules — 0/64/65/66/69/70. Подробности — в справке\n"
            "  самой команды: CI обязан отличать «отчёт напечатан» от «набор правил\n"
            "  недоступен», иначе упавший скан выглядит как «мусора нет».\n"
            "\n"
            "Ограничения этой сборки:\n"
            "  * scan работает через engine::FileSystemProbe над platform::vfs: при\n"
            "    неполных условиях (нет прав админа, часть элементов исчегла) он не\n"
            "    объявляет неудачу, а печатает в stderr строку degraded и код 0;\n"
            "  * plan и apply берут кандидатов из --candidates; исполнитель операций\n"
            "    не подключён, поэтому --execute отказывает (код 5), ничего не удаляя.\n";

    return text;
}

int printCommandHelp(std::string_view name, const CliStreams& streams) {
    const Command command = commandByName(toLowerAscii(name));
    if (command == Command::None || command == Command::Help || command == Command::Version) {
        streams.out << usageText();
        return static_cast<int>(CliExit::Ok);
    }
    if (!isAvailable(command)) {
        return reportUnavailable(std::string(name), command, streams);
    }

    // Справку печатает сама команда: её текст живёт в одном месте, иначе «что
    // умеет scan» пришлось бы читать в двух файлах и поддерживать оба.
    CommandLine helpLine;
    helpLine.command = command;
    helpLine.name = commandName(command);
    helpLine.args = {"--help"};
    return dispatchCommand(helpLine, streams);
}

int runCli(const std::vector<std::string>& argv, const CliStreams& streams) {
    CommandLine parsed;
    std::string error;
    if (!parseCommandLine(argv, parsed, error)) {
        streams.err << kProgramName << ": ОШИБКА: " << error << "\n\n" << usageText();
        return static_cast<int>(CliExit::Usage);
    }

    switch (parsed.command) {
        case Command::None:
            // Команды нет: справка в stderr (stdout остаётся пустым, чтобы
            // скрипт не спутал справку с данными) и код Usage — как у grep
            // без опечаток. Это ошибка вызова, а не просьба о справке, поэтому
            // «успех» здесь означал бы, что CI ничего не выполнил и остался
            // доволен.
            streams.err << usageText();
            return static_cast<int>(CliExit::Usage);
        case Command::Version:
            streams.out << versionText();
            return static_cast<int>(CliExit::Ok);
        case Command::Help:
            if (parsed.args.empty()) {
                streams.out << usageText();
                return static_cast<int>(CliExit::Ok);
            }
            return printCommandHelp(parsed.args.front(), streams);
        case Command::Scan:
        case Command::Plan:
        case Command::Apply:
        case Command::Report:
        case Command::Undo:
        case Command::Rules:
        case Command::Disks:
        case Command::Unknown:
            break;
    }
    return dispatchCommand(parsed, streams);
}

}  // namespace mrproper::cli
