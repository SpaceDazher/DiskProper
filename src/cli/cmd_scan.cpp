// mrproper-cli scan — прогон сканирования, отчёт в stdout, прогресс в stderr.
//
// Разделение каналов, ради которого написана команда, и её слой описаны в
// cmd_scan.hpp. Здесь — реализация: разбор аргументов, загрузка набора правил,
// сбор задач для engine::ScanCoordinator, печать прогресса в stderr и сборка
// детерминированного отчёта core::Report для stdout.
//
// ---------------------------------------------------------------------------
// Чего в этом файле нет и кто это должен сделать
// ---------------------------------------------------------------------------
//
// 1) Адаптера engine::FileSystemProbe над platform::vfs. Его нет в
//    репозитории: engine объявляет интерфейс (engine/candidate_collector.hpp),
//    platform умеет обход (platform::vfs::walk, vfs::measureFindData,
//    vfs_paths::toExtendedPath), а мост между ними не принадлежит ни одной
//    задаче волны. Мост тонкий и однозначный: UTF-8 пути правил → UTF-16,
//    FILETIME → unix-секунды, аллоцированный размер из vfs::measureFindData,
//    обход через vfs::walk с переводом WalkStep в VisitStep. Пока моста нет,
//    команда без ScanServices::createProbe печатает в stderr точное требование
//    и возвращает ScanFailed — вместо того чтобы «сканировать» выдуманные
//    нули. Зависимостью, а не вызовом platform внутри CLI, он сделаен ещё и
//    потому, что только так команда тестируется без диска (§11.1).
//
// 2) Карты разделов: её добывает platform::devices, а `scan` по §6.2 занимается
//    кандидатами. Карта попадает в отчёт только с --with-disks и только если
//    вызывающий передал listDisks.
//
// 3) Отчёта об очистке и human-readable вывода: это --plan/--apply (§8 Этап 3)
//    и report (§8 Этап 4), то есть чужие команды. Их место в справке занято
//    честной строкой «не реализовано», а не флагом, который молча ничего
//    не делает.
//
// ---------------------------------------------------------------------------
// Исключения
// ---------------------------------------------------------------------------
//
// Наружу из runScan не летит ничего: SPEC §5 («ни один отказ не роняет процесс»)
// и §12 («0 необработанных исключений»). Всё, что может бросить std-библиотека
// или std::bad_alloc, ловится на границе команды и превращается в код возврата
// с текстом в stderr. Внутри задач пула исключения не бросаются: там обход
// диска, а не разбор аргументов.
#include "cmd_scan.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/model.hpp"
#include "core/report_json.hpp"
#include "core/rules.hpp"
#include "core/rulesync.hpp"
#include "core/units.hpp"
#include "engine/candidate_collector.hpp"
#include "engine/scan_coordinator.hpp"
#include "engine/scoring_bridge.hpp"

namespace mrproper::cli {
namespace {

// ---------------------------------------------------------------------------
// Константы команды
// ---------------------------------------------------------------------------

// Потолок --workers. Движок и так ограничивает пул kDefaultMaxWorkers (16), но
// CLI не должен молча принимать «--workers 10000»: SPEC §5 требует не более 8
// потоков на скан, а 64 — верхняя граница, на которой отказ виден сразу и не
// мешает ни одному разумному прогону.
constexpr std::size_t kMaxRequestedWorkers = 64;

// Сколько замечаний сборщика («правило X: неразрешённая переменная…») попадает
// в notes отчёта. Список замечаний в самом прогоне ограничен kMaxScanIssues, а
// notes — свободный текст, и без предела он раздувает артефакт настолько, что
// перестаёт читаться. Остальное считается числом, а не выбрасывается молча.
constexpr std::size_t kMaxNotesInReport = 32;

// Нижний предел интервала прогресса. Меньше 100 мс — это уже не прогресс для
// человека, а поток строк в stderr, который в логе CI съедает больше, чем
// содержит. Движок всё равно публикует снимки чаще (§6.4 — 100 мс), печать
// просто отбрасывает промежуточные.
constexpr std::chrono::milliseconds kMinProgressInterval{100};

// Сутки — предел осмысленного значения --timeout и --progress-interval.
constexpr std::uint64_t kMaxDurationMilliseconds = 24ull * 60 * 60 * 1000;

// Имена событий в журнале. Один префикс на команду: по нему в логе виден весь
// прогон CLI, не читая сообщений.
constexpr std::string_view kEventStart = "cli.scan.start";
constexpr std::string_view kEventFinish = "cli.scan.finish";

// Переменные окружения, которые подставляются в локаторы правил
// (docs/rules-authoring.md §4). Список закрытый и повторяет имена из набора
// правил: полный дамп окружения ни в отчёт, ни в журнал не попадает, а лишние
// переменные в подстановке ничего не меняют.
constexpr std::string_view kRuleEnvironmentNames[] = {
    "ALLUSERSPROFILE", "APPDATA",       "LOCALAPPDATA", "ProgramData",  "ProgramFiles",
    "ProgramFiles(x86)", "ProgramW6432", "PUBLIC",       "SystemDrive",  "SystemRoot",
    "TEMP",           "TMP",            "USERPROFILE",  "windir",
};

// ---------------------------------------------------------------------------
// Прерывание пользователем (Ctrl+C)
// ---------------------------------------------------------------------------
//
// Обработчик сигнала умеет только взвести флаг: ловить что-либо внутри
// обработчика нельзя по стандарту, а попытка взять mutex или позвать
// координатор из него — верный путь к взаимоблокировке. Дальше флаг читает
// поток прогресса координатора (снимки приходят каждые 100 мс) и просит
// остановки. Отмена кооперативная (§6.4), поэтому «мягкое» Ctrl+C и есть
// правильное поведение: обход дописывает текущий элемент и возвращается.
volatile std::sig_atomic_t g_interrupted = 0;

extern "C" void onInterruptSignal(int) { g_interrupted = 1; }

// Ставит обработчик SIGINT на время команды и восстанавливает прежний при
// выходе: оставшийся глобальный обработчик изменил бы поведение любой другой
// команды того же процесса.
class InterruptGuard {
public:
    InterruptGuard() {
        previous_ = std::signal(SIGINT, onInterruptSignal);
        installed_ = previous_ != SIG_ERR;
    }
    ~InterruptGuard() {
        if (installed_) std::signal(SIGINT, previous_);
    }

    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
    InterruptGuard(InterruptGuard&&) = delete;
    InterruptGuard& operator=(InterruptGuard&&) = delete;

    [[nodiscard]] static bool interrupted() noexcept { return g_interrupted != 0; }

private:
    void (*previous_)(int){nullptr};
    bool installed_{false};
};

// ---------------------------------------------------------------------------
// Разбор аргументов
// ---------------------------------------------------------------------------

// «--имя=значение» → имя и значение. false для позиционного аргумента: команда
// их не принимает, и это ошибка, а не «лишнее слово».
[[nodiscard]] bool splitOption(const std::string& argument, std::string& name, std::optional<std::string>& value) {
    if (argument.size() < 3 || argument[0] != '-' || argument[1] != '-') return false;
    const std::size_t equals = argument.find('=');
    if (equals == std::string::npos) {
        name = argument.substr(2);
        value.reset();
        return true;
    }
    name = argument.substr(2, equals - 2);
    value = argument.substr(equals + 1);
    return true;
}

// Число без знака: --workers и подобные. Лишние символы — ошибка: «--workers 8x»
// не должен молча превращаться в 8.
[[nodiscard]] bool parseUnsigned(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    for (const char symbol : text) {
        if (symbol < '0' || symbol > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(symbol - '0');
        if (value > (kMax - digit) / 10) return false;  // переполнение — ошибка формата
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// Целое со знаком: --min-age может быть отрицательным (-1 — «как в правиле»).
[[nodiscard]] bool parseSigned(const std::string& text, std::int64_t& out) {
    if (text.empty()) return false;
    std::size_t offset = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        offset = 1;
    }
    std::uint64_t magnitude = 0;
    if (offset >= text.size() || !parseUnsigned(text.substr(offset), magnitude)) return false;
    const std::uint64_t limit =
        negative ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u
                 : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (magnitude > limit) return false;
    out = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    return true;
}

// Длительность: «1500», «1500ms», «2s», «3m». Без суффикса — миллисекунды, как у
// --progress-interval. Пустая строка и мусор — ошибка формата.
[[nodiscard]] bool parseDuration(const std::string& text, std::chrono::milliseconds& out) {
    if (text.empty()) return false;
    std::size_t digits = text.size();
    std::uint64_t multiplier = 1;
    const char suffix = text[digits - 1];
    if (suffix == 's' || suffix == 'S') {
        multiplier = 1000;
        --digits;
    } else if (suffix == 'm' || suffix == 'M') {
        multiplier = 60 * 1000;
        --digits;
    }
    std::uint64_t value = 0;
    if (digits == 0 || !parseUnsigned(text.substr(0, digits), value)) return false;
    if (value > kMaxDurationMilliseconds / multiplier) return false;
    out = std::chrono::milliseconds(static_cast<std::int64_t>(value * multiplier));
    return true;
}

// Категории набора в алфавитном порядке — для текста ошибки и сводки. Порядок
// «как в файле» не годится: сообщение об ошибке должно быть одинаковым от
// запуска к запуску.
[[nodiscard]] std::vector<std::string> collectCategories(const core::RuleSet& rules) {
    std::vector<std::string> categories;
    categories.reserve(rules.rules.size());
    for (const core::Rule& rule : rules.rules) {
        if (std::find(categories.begin(), categories.end(), rule.category) == categories.end()) {
            categories.push_back(rule.category);
        }
    }
    std::sort(categories.begin(), categories.end());
    return categories;
}

[[nodiscard]] std::string joinList(const std::vector<std::string>& items) {
    std::string text;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) text += ", ";
        text += items[index];
    }
    return text;
}

// ---------------------------------------------------------------------------
// Набор правил
// ---------------------------------------------------------------------------

// Значение переменной окружения в UTF-8. Отсутствие переменной и пустое
// значение неразличимы намеренно: в локаторе правил «%TEMP%» и «%TEMP%» с
// пустым значением означают одно и то же — подставлять нечего.
//
// Почему не std::getenv, два независимые причины:
//
//  1) MSVC помечает узкий getenv как C4996 («используйте _dupenv_s»), а слой
//     собирается с /WX (src/cli/CMakeLists.txt) — то есть предупреждение
//     становится ошибкой сборки и роняет всю цель mrproper_cli.
//  2) Узкий вариант теряет не-ASCII. На русской Windows типичное
//     USERPROFILE = «C:\Users\Дмитрий», и подстановка в локатор дала бы
//     «C:\Users\??????» — тихая потеря мусора из отчёта, то есть скан врёт.
//
// Широкая пара _wgetenv_s + core::toUtf8 даёт UTF-8 — ровно то, чего ждёт
// core::expandEnvironment (core/glob.hpp: значения наполняются из
// GetEnvironmentVariableW). На хостах без широкой CRT — обычный getenv, там
// окружение само по себе узкое.
//
// У MSVC _wgetenv_s не выделяет буфер, а заполняет выделенный вызывающим, и
// сигналит о нехватке места через ERANGE вместе с нужным размером, поэтому
// здесь буфер растёт по требованию. Три попытки достаточно: первая покрывает
// всё, кроме нестандартно длинного значения, а предел нужен, чтобы «буфер не
// растёт» не превратилось в бесконечный цикл.
//
// Определение скрыто #if, а не только вызов: на хосте без широкой CRT функция
// была бы неиспользуемой, а -Wall -Wextra -Werror (src/cli/CMakeLists.txt)
// превращают это в ошибку сборки — ровно тот класс поломки, которого §11.1
// требует избежать, проверяя переносимый слой на любом хосте.
#if defined(_WIN32)
[[nodiscard]] std::optional<std::string> readWideEnvironmentValue(const std::string& name) {
    constexpr std::size_t kInitialSlots = 1024;  // 2 КиБ: длиннее в Windows не бывает
    constexpr int kMaxAttempts = 3;

    // Имя переменной — ASCII-константа из kRuleEnvironmentNames, поэтому
    // расширение символ в символ корректно без MultiByteToWideChar.
    std::wstring wideName;
    wideName.reserve(name.size());
    for (const char symbol : name) {
        wideName.push_back(static_cast<wchar_t>(static_cast<unsigned char>(symbol)));
    }

    std::wstring buffer(kInitialSlots, L'\0');
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::size_t required = 0;
        const int status = _wgetenv_s(&required, buffer.data(), buffer.size(), wideName.c_str());
        if (status == ERANGE) {
            if (required <= buffer.size()) return std::nullopt;  // размер не растёт — выхода нет
            buffer.assign(required, L'\0');
            continue;
        }
        // EINVAL/ENOMEM/неизвестная ошибка и «переменной нет» трактуются
        // одинаково: подставлять в локатор нечего, а различать их команде
        // незачем — core::expandEnvironment в обоих случаях оставит шаблон
        // неразрешённым.
        if (status != 0) return std::nullopt;
        // Буфер всегда нуль-терминирован (SAL _Out_writes_opt_z_), а wstring
        // после записи CRT длины не знает — читаем до нуля.
        const std::wstring_view text(buffer.c_str());
        if (text.empty()) return std::nullopt;
        return core::toUtf8(text);
    }
    return std::nullopt;
}
#endif  // defined(_WIN32)

[[nodiscard]] std::optional<std::string> readEnvironmentValue(const std::string& name) {
#if defined(_WIN32)
    return readWideEnvironmentValue(name);
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
#endif
}

// Дамп переменных окружения для подстановки в локаторы. Формат — тот, что ждёт
// core::loadRuleFiles: «NAME=value» по строке на переменную. Переменных, которых
// нет, в дампе нет вовсе: core::expandEnvironment помечает %НЕИЗВЕСТНО% как
// неразрешённое, и такой локатор затем отбрасывается целиком.
[[nodiscard]] std::string processEnvironmentDump() {
    std::string dump;
    for (const std::string_view name : kRuleEnvironmentNames) {
        const std::string key(name);
        const std::optional<std::string> value = readEnvironmentValue(key);
        if (!value.has_value()) continue;
        dump += key;
        dump += '=';
        dump += *value;
        dump += '\n';
    }
    return dump;
}

// Прочитать набор правил из каталога. rulesVersion заполняется версией из
// manifest.json (§9.2) — это та версия, которую SPEC §4 FR-8 требует положить в
// отчёт. Файл правил такой версии не содержит, поэтому пустой результат
// честнее выдуманного «2026.01».
[[nodiscard]] bool loadRuleSetFromDirectory(const std::string& directory, const std::string& environment,
                                            core::RuleSet& out, std::string& rulesVersion, std::string& error) {
    namespace fs = std::filesystem;

    std::error_code code;
    const fs::path root(directory);
    if (!fs::is_directory(root, code)) {
        error = "каталог набора правил не найден: " + directory;
        return false;
    }

    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, code)) {
        if (entry.is_regular_file(code) && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    // Порядок чтения фиксирован: иначе первая же ошибка валидации называла бы
    // другой файл в зависимости от порядка каталога на диске.
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        error = "в каталоге " + directory + " нет ни одного файла *.json";
        return false;
    }

    std::vector<std::pair<std::string, std::string>> texts;
    texts.reserve(files.size());
    for (const fs::path& path : files) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "файл правил не читается: " + path.string();
            return false;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string text = buffer.str();
        const std::string name = path.filename().string();

        // Манифест правилами не является, но лежит рядом. Форма у них разная
        // («version» + «files» против «rules»), поэтому манифест опознаётся
        // попыткой разбора, а не именем файла: переименованный набор из правил
        // не должен перестать работать.
        try {
            rulesVersion = core::parseRuleSetManifest(text, name).version;
            continue;
        } catch (const core::RuleSyncError&) {
            // Не манифест — идёт в набор правил, разберётся загрузчик.
        }
        texts.emplace_back(name, text);
    }
    if (texts.empty()) {
        error = "в каталоге " + directory + " нет ни одного файла правил (только manifest.json)";
        return false;
    }

    try {
        out = core::loadRuleFiles(texts, environment);
        core::validateRuleSet(out);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
    if (rulesVersion.empty()) rulesVersion = out.version;
    return true;
}

// ---------------------------------------------------------------------------
// Сводка прогона
// ---------------------------------------------------------------------------

// Куда складывают задачи пула. Счётчики прогресса живут в координаторе и
// публикуются атомарно (§6.4), а кандидаты и замечания собираются здесь: их
// пишут несколько потоков, а читает один, после конца прогона. Мьютекс здесь —
// не горячий путь: обход уже закончен, сборка не трогает диск.
class ScanSink {
public:
    void add(std::vector<core::CleanupCandidate> candidates, const engine::CollectStats& stats) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (core::CleanupCandidate& candidate : candidates) candidates_.push_back(std::move(candidate));
        for (const std::string& note : stats.notes) {
            if (notes_.size() < kMaxNotesInReport) notes_.push_back(note);
        }
        notesDropped_ += stats.notesDropped;
        filesSeen_ += stats.filesSeen;
        probeErrors_ += stats.probeErrors;
        truncated_ += stats.rootsTruncated;
    }

    [[nodiscard]] std::vector<core::CleanupCandidate> candidates() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return candidates_;
    }

    [[nodiscard]] std::vector<std::string> notes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return notes_;
    }

    [[nodiscard]] std::uint64_t notesDropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return notesDropped_;
    }

    [[nodiscard]] std::uint64_t filesSeen() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return filesSeen_;
    }

    [[nodiscard]] std::uint64_t probeErrors() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return probeErrors_;
    }

    [[nodiscard]] std::uint64_t truncatedRoots() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return truncated_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<core::CleanupCandidate> candidates_;
    std::vector<std::string> notes_;
    std::uint64_t notesDropped_{};
    std::uint64_t filesSeen_{};
    std::uint64_t probeErrors_{};
    std::uint64_t truncated_{};
};

// ---------------------------------------------------------------------------
// Печать прогресса
// ---------------------------------------------------------------------------

// Секунды с одной десятой: «4,7 с» в stderr, где разделитель — точка (машинный
// канал рядом, а локаль CLI не переключает). В JSON та же длительность
// приходит числом durationMs — формат там не при чтении человеком.
[[nodiscard]] std::string formatTenthsOfSecond(std::chrono::milliseconds elapsed) {
    return std::to_string(elapsed.count() / 1000) + "." + std::to_string((elapsed.count() / 100) % 10) + " с";
}

[[nodiscard]] std::string progressTail(const engine::ProgressSnapshot& snapshot) {
    std::ostringstream text;
    text << formatTenthsOfSecond(snapshot.elapsed) << ", элементов " << core::formatCount(snapshot.itemsDone)
         << ", найдено " << core::formatBytes(snapshot.bytesFound) << ", задач " << snapshot.tasksDone << '/'
         << snapshot.tasksTotal << ", потоков " << snapshot.workersActive << '/' << snapshot.workersTotal;
    return text.str();
}

// Пишет прогресс в stderr не чаще заданного интервала и превращает Ctrl+C в
// отмену прогона.
//
// Потокобезопасность: снимки приходят из потока прогресса координатора и
// приходят оттуда последовательно (emitProgress зовётся только оттуда), а
// итоговую строку пишет главный поток уже после run(), когда поток координатора
// присоединён. Мьютекс в stderr на каждом кадре не нужен и стоил бы лишних
// системных вызовов в горячем цикле печати.
class ProgressWriter {
public:
    ProgressWriter(std::ostream& stream, std::chrono::milliseconds interval, bool enabled)
        : stream_(stream), enabled_(enabled) {
        interval_ = interval < kMinProgressInterval ? kMinProgressInterval : interval;
    }

    // Запрос отмены задаётся после создания координатора (он и есть приёмник),
    // поэтому обработчик прогресса можно было настроить раньше.
    void setStopRequest(std::function<void()> request) { requestStop_ = std::move(request); }

    void onSnapshot(const engine::ProgressSnapshot& snapshot) {
        // Проверка прерывания — на каждом снимке, до троттлинга: иначе
        // --quiet-прогон, который вообще ничего не печатает, нельзя было бы
        // остановить.
        if (InterruptGuard::interrupted() && !interruptRequested_) {
            interruptRequested_ = true;
            if (requestStop_) requestStop_();
            return;
        }
        if (!enabled_ || snapshot.finished) return;
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now < nextPrint_) return;
        nextPrint_ = now + interval_;
        stream_ << "[scan] " << progressTail(snapshot) << '\n';
    }

    // Итог одной строкой в stderr. Вызывается главным потоком после прогона.
    void printSummary(const engine::ScanRunReport& report, std::size_t candidates, std::uint64_t reclaimable) const {
        if (!enabled_) return;
        stream_ << "[scan] " << (report.cancelled ? "прервано" : "готово") << " за " << formatTenthsOfSecond(report.duration)
                << ": кандидатов " << candidates << ", освободится " << core::formatBytes(reclaimable)
                << ", задач выполнено " << report.completed << " из " << report.tasks.size() << ", ошибок "
                << report.failed << ", отменено " << report.cancelledTasks << '\n';
    }

private:
    std::ostream& stream_;
    std::function<void()> requestStop_;
    std::chrono::milliseconds interval_{kMinProgressInterval};
    std::chrono::steady_clock::time_point nextPrint_{};
    bool enabled_{true};
    bool interruptRequested_{false};
};

// Сторож --timeout: просит остановить прогон через заданное время. Спит на
// условии, а не на sleep, чтобы при более раннем завершении прогона поток
// просыпался немедленно и не держал процесс до конца команды.
class TimeoutWatchdog {
public:
    TimeoutWatchdog(std::chrono::milliseconds timeout, std::function<void()> onTimeout) {
        if (timeout <= std::chrono::milliseconds::zero() || !onTimeout) return;
        thread_ = std::thread([this, timeout, onTimeout = std::move(onTimeout)]() mutable {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock<std::mutex> lock(mutex);
            if (wake.wait_for(lock, timeout, [this] { return stop_.stop_requested(); })) return;
            onTimeout();
        });
    }

    ~TimeoutWatchdog() {
        stop_.request_stop();
        if (thread_.joinable()) thread_.join();
    }

    TimeoutWatchdog(const TimeoutWatchdog&) = delete;
    TimeoutWatchdog& operator=(const TimeoutWatchdog&) = delete;
    TimeoutWatchdog(TimeoutWatchdog&&) = delete;
    TimeoutWatchdog& operator=(TimeoutWatchdog&&) = delete;

private:
    std::stop_source stop_;  // объявлен раньше thread_: поток читает его сразу после старта
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Отчёт
// ---------------------------------------------------------------------------

// Порядок кандидатов: категория, правило, путь. Задачи пула заканчиваются в
// произвольном порядке, и без сортировки один и тот же вход давал бы разные
// байты в stdout — golden-тест (§11.4) мигал бы, а «повторный скан идемпотентен»
// (§8 Этап 2) нельзя было бы проверить сравнением файлов.
void sortCandidates(std::vector<core::CleanupCandidate>& candidates) {
    std::sort(candidates.begin(), candidates.end(),
              [](const core::CleanupCandidate& left, const core::CleanupCandidate& right) {
                  if (left.category != right.category) return left.category < right.category;
                  if (left.ruleId != right.ruleId) return left.ruleId < right.ruleId;
                  return left.path < right.path;
              });
}

[[nodiscard]] std::uint64_t reclaimableBytes(const std::vector<core::CleanupCandidate>& candidates) {
    std::uint64_t total = 0;
    for (const core::CleanupCandidate& candidate : candidates) total += candidate.allocatedBytes;
    return total;
}

// notes отчёта: сводка прогона, заметки сборщика («правило X: …») и то, что
// человек попросил через --note. Порядок фиксирован, лишнее считается.
[[nodiscard]] std::string buildNotes(const ScanOptions& options, const std::vector<std::string>& sinkNotes,
                                    std::uint64_t notesDropped, const engine::ScanRunReport& report,
                                    std::size_t candidates) {
    std::ostringstream text;
    text << "кандидатов: " << candidates << "; задач: " << report.tasks.size() << "; выполнено: " << report.completed
         << "; прервано: " << report.cancelledTasks << "; пропущено: " << report.skipped << "; ошибок: " << report.failed;
    if (report.cancelled) text << "; прогон прерван";
    if (notesDropped != 0) text << "; замечаний сборщика опущено: " << notesDropped;
    for (const std::string& note : sinkNotes) text << '\n' << note;
    if (!options.note.empty()) text << '\n' << options.note;
    return text.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Публичный интерфейс
// ---------------------------------------------------------------------------

const char* toString(ScanExit code) noexcept {
    switch (code) {
        case ScanExit::Ok:
            return "ok";
        case ScanExit::Usage:
            return "usage";
        case ScanExit::RulesUnavailable:
            return "rules-unavailable";
        case ScanExit::ScanFailed:
            return "scan-failed";
        case ScanExit::Interrupted:
            return "interrupted";
    }
    return "unknown";
}

std::string scanUsageText() {
    return "Использование: mrproper-cli scan [опции]\n"
           "\n"
           "Сканирует систему по набору правил и печатает отчёт в JSON в stdout.\n"
           "Прогресс, итоги и ошибки идут в stderr — stdout остаётся машинным.\n"
           "\n"
           "Опции:\n"
           "  --json                      JSON в stdout (формат по умолчанию; в скриптах пишется явно)\n"
           "  --compact                   JSON одной строкой (indent = -1); по умолчанию с отступами\n"
           "  --rules <каталог>           набор правил, по умолчанию ./rules\n"
           "  --category <id>             сканировать только категорию; можно повторять\n"
           "  --min-confidence <0..100>   оставить кандидатов с уверенностью не ниже порога\n"
           "  --min-age <дней>            переопределить minAgeDays (-1 — как объявлено, 0 — любой возраст)\n"
           "  --workers <N>               потоков пула (0 — выбрать автоматически)\n"
           "  --progress-interval <длит>  как часто печатать прогресс в stderr (по умолчанию 1s)\n"
           "  --timeout <длит>            остановить прогон через указанное время (0 — без предела)\n"
           "  --with-disks                добавить карту разделов (нужен listDisks; серийники маскируются)\n"
           "  --note <текст>              свободный текст в поле notes отчёта\n"
           "  --quiet                     не печатать прогресс в stderr (итог и ошибки остаются)\n"
           "  -h, --help                  эта справка\n"
           "\n"
           "Длительность: 1500, 1500ms, 2s, 3m.\n"
           "Коды возврата: 0 — отчёт напечатан, 2 — ошибка аргументов, 3 — набор правил недоступен,\n"
           "4 — прогон не дал результата, 130 — прервано.\n"
           "\n"
           "Не реализовано (SPEC §8): plan, apply, report, rules — отдельные команды.\n"
           "Ctrl+C просит прогон остановиться: обход дописывает текущий элемент и возвращается (§6.4).\n";
}

bool parseScanOptions(const std::vector<std::string>& args, ScanOptions& options, std::string& error) {
    options = ScanOptions{};
    error.clear();

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];

        if (argument == "-h" || argument == "--help") {
            options.help = true;
            return true;
        }
        if (argument == "--") {
            // Разделитель: после него позиционные аргументы, а команда их не
            // принимает. Молча проигнорировать их нельзя — скрипт с опечаткой
            // получил бы «успешный» скан не того, что хотел.
            if (index + 1 < args.size()) {
                error = "команда scan не принимает позиционных аргументов: " + args[index + 1];
                return false;
            }
            return true;
        }

        std::string name;
        std::optional<std::string> inlineValue;
        if (!splitOption(argument, name, inlineValue)) {
            error = "неожиданный аргумент: " + argument;
            return false;
        }

        // Флаги значения не принимают: «--quiet=1» — это опечатка, а не «тихий
        // режим», и молчать о ней нельзя.
        const auto rejectValue = [&error, &name](const std::optional<std::string>& value) {
            if (!value.has_value()) return true;
            error = "опция --" + name + " не принимает значение";
            return false;
        };
        // Значение опции: «--имя=значение» или следующий аргумент.
        const auto takeValue = [&args, &index, &error, &name](const std::optional<std::string>& inlineValueValue) {
            if (inlineValueValue.has_value()) return inlineValueValue;
            if (index + 1 >= args.size()) {
                error = "опции --" + name + " нужно значение";
                return std::optional<std::string>{};
            }
            ++index;
            return std::optional<std::string>(args[index]);
        };

        if (name == "json") {
            if (!rejectValue(inlineValue)) return false;
            options.pretty = true;
        } else if (name == "compact") {
            if (!rejectValue(inlineValue)) return false;
            options.pretty = false;
        } else if (name == "quiet") {
            if (!rejectValue(inlineValue)) return false;
            options.quiet = true;
        } else if (name == "with-disks") {
            if (!rejectValue(inlineValue)) return false;
            options.withDisks = true;
        } else if (name == "rules" || name == "category" || name == "note") {
            const std::optional<std::string> value = takeValue(inlineValue);
            if (!value.has_value() || value->empty()) {
                if (error.empty()) error = "опции --" + name + " нужно непустое значение";
                return false;
            }
            if (name == "rules") {
                options.rulesPath = *value;
            } else if (name == "category") {
                options.categories.push_back(*value);
            } else {
                options.note = *value;
            }
        } else if (name == "min-confidence" || name == "min-age" || name == "workers" || name == "progress-interval" ||
                   name == "timeout") {
            const std::optional<std::string> value = takeValue(inlineValue);
            if (!value.has_value()) return false;
            if (name == "workers") {
                std::uint64_t workers = 0;
                if (!parseUnsigned(*value, workers) || workers > kMaxRequestedWorkers) {
                    error = "--workers: нужно целое от 0 до " + std::to_string(kMaxRequestedWorkers);
                    return false;
                }
                options.workers = static_cast<std::size_t>(workers);
            } else if (name == "min-confidence") {
                if (!parseSigned(*value, options.minConfidence) || options.minConfidence < 0 ||
                    options.minConfidence > 100) {
                    error = "--min-confidence: нужно целое от 0 до 100";
                    return false;
                }
            } else if (name == "min-age") {
                if (!parseSigned(*value, options.minAgeDays) || options.minAgeDays < -1) {
                    error = "--min-age: нужно целое не меньше -1 (-1 — как объявлено в правиле)";
                    return false;
                }
            } else {
                std::chrono::milliseconds duration{};
                if (!parseDuration(*value, duration)) {
                    error = "--" + name + ": нужно число миллисекунд или длительность вида 2s, 3m";
                    return false;
                }
                if (name == "progress-interval") {
                    options.progressInterval = duration;
                } else {
                    options.timeout = duration;
                }
            }
        } else {
            error = "неизвестная опция: --" + name;
            return false;
        }
    }
    return true;
}

std::string formatScanProgress(const engine::ProgressSnapshot& snapshot) { return progressTail(snapshot); }

int runScan(const std::vector<std::string>& args, const ScanServices& services, std::ostream& out, std::ostream& err) {
    ScanOptions options;
    std::string error;
    if (!parseScanOptions(args, options, error)) {
        err << "[scan] " << error << "\n[scan] " << toString(ScanExit::Usage) << " — справка: mrproper-cli scan --help\n";
        return static_cast<int>(ScanExit::Usage);
    }
    if (options.help) {
        err << scanUsageText();
        return static_cast<int>(ScanExit::Ok);
    }
    if (!services.createProbe) {
        err << "[scan] не передан адаптер обхода ФС (ScanServices::createProbe): моста engine::FileSystemProbe над\n"
               "[scan] platform::vfs в репозитории пока нет, сканировать нечем. Требования — в cmd_scan.cpp.\n";
        return static_cast<int>(ScanExit::ScanFailed);
    }

    const std::string environment = services.environmentDump ? services.environmentDump() : processEnvironmentDump();

    core::RuleSet rules;
    std::string rulesVersion;
    if (!loadRuleSetFromDirectory(options.rulesPath, environment, rules, rulesVersion, error)) {
        err << "[scan] " << error << "\n[scan] " << toString(ScanExit::RulesUnavailable)
            << " — набор правил обязателен: без него неизвестно, что считать мусором (§4 FR-3).\n";
        return static_cast<int>(ScanExit::RulesUnavailable);
    }

    // Неизвестная категория — ошибка аргументов, а не пустой скан: молча
    // проигнорированная категория выглядела бы как «мусора нет».
    if (!options.categories.empty()) {
        const std::vector<std::string> known = collectCategories(rules);
        for (const std::string& wanted : options.categories) {
            if (std::find(known.begin(), known.end(), wanted) == known.end()) {
                err << "[scan] неизвестная категория: " << wanted << "\n[scan] Доступные категории: " << joinList(known)
                    << ".\n";
                return static_cast<int>(ScanExit::Usage);
            }
        }
        std::vector<core::Rule> filtered;
        filtered.reserve(rules.rules.size());
        for (const core::Rule& rule : rules.rules) {
            if (std::find(options.categories.begin(), options.categories.end(), rule.category) !=
                options.categories.end()) {
                filtered.push_back(rule);
            }
        }
        rules.rules = std::move(filtered);
    }

    const auto nowUnix = [&services]() -> std::int64_t {
        if (services.nowUnix) return services.nowUnix();
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    const std::int64_t startedAtUnix = nowUnix();

    const std::vector<std::string> categories = collectCategories(rules);

    core::LogFields startFields;
    startFields.push_back(core::logField("rulesVersion", rulesVersion));
    startFields.push_back(core::logField("rules", rules.size()));
    startFields.push_back(core::logField("categories", categories.size()));
    core::logInfo(kEventStart, "скан запущен", startFields);

    err << "[scan] набор правил " << (rulesVersion.empty() ? std::string("без версии") : rulesVersion) << ", правил "
        << rules.size() << ", категорий " << categories.size() << "\n";

    // Сборка задач: одна задача на категорию. Категория, а не отдельное
    // правило, потому что правила одной категории обходятся вместе и приходят
    // в отчёт одной строкой дерева (FR-5), а раздельный обход дробит прогресс
    // и платит накладными расходами на каждую задачу.
    std::vector<std::string> categoryOrder;
    for (const core::Rule& rule : rules.rules) {
        if (std::find(categoryOrder.begin(), categoryOrder.end(), rule.category) == categoryOrder.end()) {
            categoryOrder.push_back(rule.category);
        }
    }

    const engine::scoring_bridge::RuleLookup lookup = engine::scoring_bridge::makeRuleLookup(rules);
    engine::scoring_bridge::ScoringContext scoringContext;
    scoringContext.now = startedAtUnix;
    scoringContext.userProfileRoot = services.userProfileRoot;
    scoringContext.systemRoots = services.systemRoots;
    // Состояние процессов не проверялось: определение блокировок в этой
    // команде не участвует, и мост скажет об этом строкой в причинах, а не
    // промолчит (FR-4 LockedBy остаётся пустым, и это видно).
    scoringContext.processClosedStateKnown = false;

    ScanSink sink;
    const std::function<std::unique_ptr<engine::FileSystemProbe>()> probeFactory = services.createProbe;
    std::vector<engine::ScanTask> tasks;
    tasks.reserve(categoryOrder.size());
    for (const std::string& category : categoryOrder) {
        core::RuleSet subset;
        subset.schemaVersion = rules.schemaVersion;
        subset.version = rules.version;
        subset.minAppVersion = rules.minAppVersion;
        for (const core::Rule& rule : rules.rules) {
            if (rule.category == category) subset.rules.push_back(rule);
        }

        engine::CollectOptions collectOptions;
        collectOptions.now = startedAtUnix;
        collectOptions.minAgeDaysOverride = options.minAgeDays;

        tasks.push_back(engine::ScanTask{
            category,
            category,
            0,  // число элементов заранее неизвестно: обход ФС не знает счётчика
            [subset = std::move(subset), collectOptions, scoringContext, probeFactory, &lookup,
             &sink](engine::ScanTaskContext& context) {
                // Свой адаптер на задачу: обход идёт в пуле, и требовать от
                // реализации потокобезопасности — это требование без владельца.
                std::unique_ptr<engine::FileSystemProbe> probe = probeFactory();
                if (!probe) return;  // адаптер не выдался: задача пуста, а не падение прогона
                engine::CollectResult collected =
                    engine::collectCandidates(subset, *probe, collectOptions, context.stopToken());
                // Оценка после сбора, а не в CollectOptions::finalize: мост
                // работает по всему списку кандидатов разом и строит таблицу
                // правил один раз (engine/scoring_bridge.hpp).
                engine::scoring_bridge::scoreCandidates(collected.candidates, lookup, scoringContext);

                // Счётчики прогресса: элементы — сколько файлов увидел обход,
                // байты — сумма аллоцированных размеров найденных кандидатов.
                // Это оценка «сколько освободится», а не весь объём обхода:
                // пока идёт обход очередного каталога, его файлы ещё не
                // посчитаны. Для полосы прогресса такая оценка честнее нуля.
                std::uint64_t found = 0;
                for (const core::CleanupCandidate& candidate : collected.candidates) {
                    found += candidate.allocatedBytes;
                }
                context.addItems(collected.stats.filesSeen);
                context.addBytes(found);
                sink.add(std::move(collected.candidates), collected.stats);
            },
        });
    }

    ProgressWriter progress(err, options.progressInterval, !options.quiet);

    engine::ScanCoordinatorOptions coordinatorOptions;
    coordinatorOptions.workers = options.workers;
    // Прогресс идёт в stderr (видит человек), а не в журнал по умолчанию: иначе
    // каждая строка прогресса оказалась бы ещё и в файле журнала.
    coordinatorOptions.logProgress = false;
    coordinatorOptions.onProgress = [&progress](const engine::ProgressSnapshot& snapshot) { progress.onSnapshot(snapshot); };

    engine::ScanCoordinator coordinator(coordinatorOptions);
    progress.setStopRequest([&coordinator] { coordinator.requestStop(); });

    // Сторож и обработчик прерывания ставятся до старта прогона: иначе Ctrl+C в
    // первую секунду (а скан C: занимает до 60 с, §12) остался бы без реакции.
    const InterruptGuard interruptGuard;
    std::atomic<bool> timedOut{false};
    const TimeoutWatchdog watchdog(options.timeout, [&coordinator, &timedOut] {
        timedOut.store(true, std::memory_order_release);
        coordinator.requestStop();
    });

    const engine::ScanRunReportPtr runReport = coordinator.run(std::move(tasks));
    if (runReport == nullptr) {
        err << "[scan] прогон не дал отчёта\n";
        return static_cast<int>(ScanExit::ScanFailed);
    }

    const bool timeoutFired = timedOut.load(std::memory_order_acquire);
    const bool cancelled = runReport->cancelled || InterruptGuard::interrupted() || timeoutFired;

    std::vector<core::CleanupCandidate> candidates = sink.candidates();
    sortCandidates(candidates);

    std::size_t belowThreshold = 0;
    if (options.minConfidence >= 0) {
        std::vector<core::CleanupCandidate> kept;
        kept.reserve(candidates.size());
        for (core::CleanupCandidate& candidate : candidates) {
            if (candidate.confidence >= options.minConfidence) {
                kept.push_back(std::move(candidate));
            } else {
                ++belowThreshold;
            }
        }
        candidates = std::move(kept);
    }

    const std::uint64_t reclaimable = reclaimableBytes(candidates);
    progress.printSummary(*runReport, candidates.size(), reclaimable);
    if (cancelled) {
        err << "[scan] прогон остановлен";
        if (timeoutFired) err << " по --timeout";
        if (InterruptGuard::interrupted()) err << " по Ctrl+C";
        err << ": результат неполный, кандидаты показаны частично (§6.4)\n";
    }
    if (belowThreshold != 0) {
        err << "[scan] отсеяно по --min-confidence " << options.minConfidence << ": " << belowThreshold << '\n';
    }
    if (sink.truncatedRoots() != 0) {
        err << "[scan] список корней обрезан пределом разворота локаторов: " << sink.truncatedRoots()
            << " — оценка освобождения занижена (см. notes отчёта)\n";
    }
    if (sink.probeErrors() != 0) {
        err << "[scan] отказов чтения каталогов: " << sink.probeErrors() << " — см. errors отчёта\n";
    }

    // ---- Сборка отчёта. Дальше в stdout уходит только JSON. ----
    const std::int64_t finishedAtUnix = nowUnix();

    core::Report report;
    report.kind = core::ReportKind::Scan;
    report.environment.appVersion = services.appVersion;
    report.environment.rulesVersion = rulesVersion;
    report.environment.osCaption = services.osCaption;
    report.environment.osVersion = services.osVersion;
    report.environment.osBuild = services.osBuild;
    report.environment.architecture = services.architecture;
    report.timing.startedAtUnix = startedAtUnix;
    report.timing.finishedAtUnix = finishedAtUnix;
    report.timing.durationMs = runReport->duration.count();
    report.options.includeDisks = options.withDisks;
    report.options.includeCandidates = true;
    report.options.includeOperations = false;  // операций у скана нет: это --apply (§8 Этап 3)
    report.options.includeUntouched = false;
    report.options.includeErrors = true;
    report.options.maskSerials = true;      // §5: серийник по умолчанию уходит в баг-репорт замазанным
    report.options.maskVolumeGuids = true;
    report.candidates = std::move(candidates);
    if (options.withDisks && services.listDisks) {
        try {
            report.disks = services.listDisks();
        } catch (const std::exception& failure) {
            core::ReportError inventory;
            inventory.scope = "scan";
            inventory.operation = "disks";
            inventory.message = std::string("инвентаризация дисков не удалась: ") + failure.what();
            inventory.atUnix = finishedAtUnix;
            report.errors.push_back(std::move(inventory));
        }
    }
    for (const engine::TaskReport& task : runReport->tasks) {
        if (task.status != engine::TaskStatus::Failed) continue;
        core::ReportError taskError;
        taskError.scope = "scan";
        taskError.operation = task.name;
        taskError.message = "задача не выполнена: " + (task.error.empty() ? std::string("причина не сообщена") : task.error);
        taskError.atUnix = finishedAtUnix;
        report.errors.push_back(std::move(taskError));
    }
    if (cancelled) {
        core::ReportError cancelError;
        cancelError.scope = "scan";
        cancelError.operation = "run";
        cancelError.message =
            timeoutFired ? "прогон остановлен по --timeout" : "прогон прерван пользователем (Ctrl+C)";
        cancelError.atUnix = finishedAtUnix;
        report.errors.push_back(std::move(cancelError));
    }
    report.notes = buildNotes(options, sink.notes(), sink.notesDropped(), *runReport, report.candidates.size());

    for (const std::string& problem : core::validateReport(report)) {
        err << "[scan] отчёт не согласован: " << problem << '\n';
    }

    core::LogFields finishFields;
    finishFields.push_back(core::logField("candidates", report.candidates.size()));
    finishFields.push_back(core::logField("reclaimable", reclaimable));
    finishFields.push_back(core::logField("failed", runReport->failed));
    finishFields.push_back(core::logField("cancelled", cancelled));
    core::logInfo(kEventFinish, "скан завершён", finishFields);

    // Единственная запись в stdout за весь вызов. Строка документа уже
    // заканчивается переводом строки (core::report_json), поэтому ничего
    // дописывать не надо: лишний перевод ломал бы побайтовое сравнение
    // golden-файла (§11.4).
    out << core::reportToJson(report, options.pretty ? 2 : -1);
    out.flush();
    if (!out) {
        // Часть документа могла уйти до обрыва: об этом честно сообщаем в
        // stderr, потому что разбирать частичный JSON должен вызывающий.
        err << "[scan] stdout не принял отчёт (канал закрыт или устройство отказало)\n";
        return static_cast<int>(ScanExit::ScanFailed);
    }

    if (cancelled) return static_cast<int>(ScanExit::Interrupted);
    // Отчёт, в котором не выполнена ни одна задача, — не результат, а пустота:
    // в CI это должно быть видно, иначе «скан ничего не нашёл» и «скан сломался»
    // неразличимы (§8 Этап 2 проверяет состав категорий, а не ноль).
    if (!runReport->tasks.empty() && runReport->completed == 0) {
        err << "[scan] ни одна задача не выполнена — смотрите errors отчёта\n";
        return static_cast<int>(ScanExit::ScanFailed);
    }
    return static_cast<int>(ScanExit::Ok);
}

}  // namespace mrproper::cli
