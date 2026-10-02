// Мост «переносимый сборщик кандидатов ↔ платформенный обход ФС»: реализация.
//
// Контракт, границы и список «чего модуль не делает» — в file_system_probe.hpp;
// здесь только код. Порядок чтения: счётчики и мелочи перевода (форма пути,
// FILETIME → unix-секунды, пара размеров) → перевод элемента обхода → перевод
// VisitStep в WalkStep → три метода FileSystemProbe → диагностика «degraded».
//
// Слой: единственный файл engine, который знает про Win32, и это ровно то, за
// что он отвечает. Сборщик кандидатов, скоринг и правила остаются переносимыми
// (SPEC §11.1), а мост — тонкий и проверяемый отдельно.
//
// Чего здесь нет намеренно: повторного обхода каталогов (он у platform::vfs),
// разбора локаторов и правил исключений (они у candidate_collector), учёта
// «свой путь внутри корня» (тот же кандидат проверяет это сам по нормализованным
// путям, которые отдаёт этот файл).
#include "file_system_probe.hpp"

#include <windows.h>  // NOLINT(bugprone-suspicious-include) — Win32-мост, единственное законное место

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "platform/vfs_paths.hpp"
#include "platform/vfs_size.hpp"
#include "platform/vfs_walk.hpp"
#include "platform/win_error.hpp"

namespace mvfs = mrproper::platform::vfs;
namespace mpaths = mrproper::platform::vfs_paths;

namespace mrproper::engine {
namespace {

// ---------------------------------------------------------------------------
// Мелочи перевода
// ---------------------------------------------------------------------------

// Предел глубины продублирован в заголовке, чтобы тот оставался переносимым.
// Здесь — проверка, что две правды не разошлись: молчаливый предел, отличный от
// задуманного, обошёлся бы поддеревом, которое никто не увидел бы.
static_assert(kDefaultVfsMaxDepth == mvfs::kDefaultMaxDepth,
              "MrProper: предел глубины обхода в мосте и в platform::vfs разошёлся");

// Префиксы форм пути. Снимаются с результата нормализации: модель (§6.3) и
// исключения правил написаны в форме «C:\Users\…», и префикс `\\?\` в модели
// быть не должен — иначе один и тот же каталог в кандидате, в отчёте и в
// исключении правила был бы записан тремя разными строками.
constexpr std::string_view kExtendedUncPrefix = "\\\\?\\UNC\\";
constexpr std::string_view kExtendedPrefix = "\\\\?\\";

[[nodiscard]] std::string_view stripExtendedPrefix(std::string_view path) noexcept {
    if (path.starts_with(kExtendedUncPrefix)) return path.substr(kExtendedUncPrefix.size());
    if (path.starts_with(kExtendedPrefix)) return path.substr(kExtendedPrefix.size());
    return path;
}

// Тик в 100 нс между 1601-01-01 (эпоха FILETIME) и 1970-01-01 (эпоха модели).
constexpr std::uint64_t kFileTimeTicksPerSecond = 10000000ull;
constexpr std::int64_t kFileTimeUnixEpochDelta = 11644473600;

// FILETIME → unix-секунды. Ноль на входе и любой отказ ФС дают 0, что в
// ProbeEntry означает ровно одно: «времени нет». Значение до 1970 года NTFS не
// отдаёт, а вычитание из него дало бы отрицательный возраст файла — поэтому
// такое тоже считается «времени нет».
[[nodiscard]] std::int64_t fileTimeToUnix(std::uint64_t ticks) noexcept {
    if (ticks < static_cast<std::uint64_t>(kFileTimeUnixEpochDelta) * kFileTimeTicksPerSecond) return 0;
    return static_cast<std::int64_t>(ticks / kFileTimeTicksPerSecond) - kFileTimeUnixEpochDelta;
}

[[nodiscard]] std::uint64_t toTicks(const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

// ---------------------------------------------------------------------------
// Счётчики процесса
// ---------------------------------------------------------------------------

// Накопители — общие для процесса, потому что отчёт читается один, а обходят
// диск несколько потоков пула. relaxed достаточно и честно: счётчик не
// синхронизирует между собой ничего, его читают после того, как все задачи
// завершились. (Так же устроены счётчики platform::vfs::sizeProbeStats.)
struct ProbeCounters {
    std::atomic<std::uint64_t> walks{0};
    std::atomic<std::uint64_t> entries{0};
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> directoriesListed{0};
    std::atomic<std::uint64_t> statsRead{0};
    std::atomic<std::uint64_t> allocatedMeasured{0};
    std::atomic<std::uint64_t> allocatedUnknown{0};
    std::atomic<std::uint64_t> rootRefused{0};
    std::atomic<std::uint64_t> elementErrors{0};
    std::atomic<std::uint64_t> pathRefused{0};
    // Время по этапам. Те же атомики с relaxed: это счётчики, а не синхронизация
    // (см. абзац выше про ProbeCounters).
    std::atomic<std::uint64_t> allocCalls{0};
    std::atomic<std::uint64_t> allocNs{0};
    std::atomic<std::uint64_t> convertCalls{0};
    std::atomic<std::uint64_t> convertNs{0};
    std::atomic<std::uint64_t> convertAllocNs{0};
    std::atomic<std::uint64_t> statCalls{0};
    std::atomic<std::uint64_t> statNs{0};
    std::atomic<std::uint64_t> listCalls{0};
    std::atomic<std::uint64_t> listNs{0};
    std::atomic<std::uint64_t> walkCalls{0};
    std::atomic<std::uint64_t> walkNs{0};
    std::atomic<std::uint64_t> visitCalls{0};
    std::atomic<std::uint64_t> visitNs{0};
};

ProbeCounters g_counters;

void addRelaxed(std::atomic<std::uint64_t>& counter, std::uint64_t by = 1) noexcept {
    counter.fetch_add(by, std::memory_order_relaxed);
}

// Накопление интервала в счётчик. Два чтения steady_clock на элемент обхода —
// это десятки наносекунд против миллисекунд, которые здесь считают: насытка
// получается точной, а накладные расходы не видны на фоне измеряемого.
class StageClock {
public:
    explicit StageClock(std::atomic<std::uint64_t>& sink) noexcept
        : sink_(&sink), started_(std::chrono::steady_clock::now()) {}
    ~StageClock() {
        const std::chrono::nanoseconds elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_);
        sink_->fetch_add(static_cast<std::uint64_t>(elapsed.count()), std::memory_order_relaxed);
    }
    StageClock(const StageClock&) = delete;
    StageClock& operator=(const StageClock&) = delete;

private:
    std::atomic<std::uint64_t>* sink_;
    std::chrono::steady_clock::time_point started_;
};

// ---------------------------------------------------------------------------
// Элемент обхода → ProbeEntry
// ---------------------------------------------------------------------------

// Пара размеров из platform::vfs в ProbeEntry. allocatedKnown == false — это
// «аллоцированного размера by design нет» (каталог, ссылка, отказ ФС, режим без
// измерения), и сборщик в этом случае честно берёт логический размер и
// ставит CollectStats::allocatedEstimated: оценка освобождения не занижается
// молча (FR-4). allocatedBytes при этом остаётся нулём — выдуманное число там,
// где измерения не было, было бы хуже отсутствия числа.
void applySize(const mvfs::FileSize& size, ProbeEntry& out) {
    if (mvfs::hasFlag(size.flags, mvfs::FileFlags::Directory)) {
        out.sizeKnown = false;  // каталог обход не суммирует: его стоимость считает вызывающий
        return;
    }
    out.sizeKnown = size.ok();
    out.logicalBytes = size.logicalBytes;
    out.allocatedKnown = size.allocatedKnown;
    if (size.allocatedKnown) {
        out.allocatedBytes = size.allocatedBytes;
    }
}

// Перевод элемента обхода в форму сборщика. false — путь или имя не перевелись в
// UTF-8: элемент пропускается (одно негодное имя не должно отменять скан), но
// спуск в такой каталог запрещается — иначе обход ушёл бы в поддерево, где
// имена тоже не переводятся.
[[nodiscard]] bool convertEntry(const mvfs::WalkEntry& source, const VfsProbeOptions& options, ProbeEntry& out) {
    std::string path;
    if (!platform::tryToUtf8(source.path, path)) return false;
    std::string name;
    if (!platform::tryToUtf8(source.name, name)) return false;

    out = ProbeEntry{};
    out.path = stripExtendedPrefix(path);
    out.name = std::move(name);
    out.isDirectory = source.kind == mvfs::EntryKind::Directory;
    out.reparsePoint = source.reparsePoint;
    out.writeTime = fileTimeToUnix(source.lastWriteTime);
    out.accessTime = fileTimeToUnix(source.lastAccessTime);

    if (source.kind != mvfs::EntryKind::File) return true;  // каталог и «прочее» не измеряем

    out.sizeKnown = true;
    out.logicalBytes = source.logicalBytes;
    // Ссылка: аллоцированный размер у неё by design не запрашивается (vfs_size),
    // а обход в неё всё равно не пойдёт (FR-6). Отсутствие размера у ссылки —
    // не отказ и не повод считать прогон неполным, поэтому в счётчики оно не
    // попадает.
    if (source.reparsePoint) return true;
    if (!options.measureAllocated) {
        addRelaxed(g_counters.allocatedUnknown);
        return true;
    }
    // Один вызов на элемент: логический размер уже есть в записи каталога, и
    // platform::vfs::queryAllocatedSize — ровно тот путь, который vfs_size.hpp
    // называет основным для обхода. Время вызова считается ОТДЕЛЬНО от времени
    // перевода записи: это единственный системный вызов на файл в горячем
    // месте скана, и его цену надо знать числом, а не ощущением.
    addRelaxed(g_counters.allocCalls);
    const std::uint64_t allocNsBefore = g_counters.allocNs.load(std::memory_order_relaxed);
    const mvfs::AllocatedSizeResult allocated = [&] {
        const StageClock clock(g_counters.allocNs);
        return mvfs::queryAllocatedSize(source.path);
    }();
    // allocNs накопительный, поэтому в convertAllocNs идёт ДЕЛЬТА одного вызова,
    // а не значение счётчика: иначе сумма умножилась бы на число файлов.
    addRelaxed(g_counters.convertAllocNs, g_counters.allocNs.load(std::memory_order_relaxed) - allocNsBefore);
    if (allocated.ok()) {
        out.allocatedKnown = true;
        out.allocatedBytes = allocated.bytes;
        addRelaxed(g_counters.allocatedMeasured);
    } else {
        addRelaxed(g_counters.allocatedUnknown);
    }
    return true;
}

// Решение посетителя сборщика на решение посетителя обхода. Skip на файле —
// безобидная просьба (спускаться всё равно некуда), Stop останавливает обход
// ровно там, где его остановил бы вызывающий сборщика.
[[nodiscard]] mvfs::WalkStep convertStep(VisitStep step) noexcept {
    switch (step) {
        case VisitStep::Skip:
            return mvfs::WalkStep::SkipDirectory;
        case VisitStep::Stop:
            return mvfs::WalkStep::Stop;
        case VisitStep::Continue:
            break;
    }
    return mvfs::WalkStep::Continue;
}

[[nodiscard]] mvfs::WalkStep visitEntry(const VfsProbeOptions& options, const EntryVisitor& visit,
                                        const mvfs::WalkEntry& entry) {
    ProbeEntry converted;
    addRelaxed(g_counters.convertCalls);
    const bool convertedOk = [&] {
        const StageClock clock(g_counters.convertNs);
        return convertEntry(entry, options, converted);
    }();
    if (!convertedOk) {
        addRelaxed(g_counters.pathRefused);
        core::LogFields fields;
        fields.push_back(core::logField("name", entry.name));
        core::logDebug("engine.probe.path", "элемент пропущен: путь не переводится в UTF-8", std::move(fields));
        return entry.kind == mvfs::EntryKind::Directory ? mvfs::WalkStep::SkipDirectory : mvfs::WalkStep::Continue;
    }
    addRelaxed(g_counters.entries);
    if (!converted.isDirectory) addRelaxed(g_counters.files);
    addRelaxed(g_counters.visitCalls);
    const VisitStep step = [&] {
        const StageClock clock(g_counters.visitNs);
        return visit(converted);
    }();
    return convertStep(step);
}

// Отказ чтения корня обхода. WalkResult::completed в этом случае тоже true
// (обход честно дошёл до конца того, что есть), поэтому «корень не открылся» при-
// ходится отличать иначе: иначе «каталог недоступен» и «каталог пуст» были бы
// одним и тем же ответом.
//
// Отказ корня записывается ПЕРВЫМ в errors, и список ограничен kMaxStoredErrors
// записями, поэтому в нём он всегда есть: искать путь корня в списке безопасно.
[[nodiscard]] bool rootUnreadable(const mvfs::WalkResult& result, std::wstring_view requested) noexcept {
    if (result.rootSkippedReparse) return true;
    for (const mvfs::WalkError& error : result.errors) {
        if (error.path == requested) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// FileSystemProbe
// ---------------------------------------------------------------------------

bool VfsFileSystemProbe::listDirectory(std::string_view directory, std::stop_token token,
                                       std::vector<ProbeEntry>& out) {
    addRelaxed(g_counters.listCalls);
    const StageClock listClock(g_counters.listNs);
    out.clear();
    if (directory.empty() || token.stop_requested()) return false;

    try {
        std::wstring wide;
        if (!platform::tryToUtf16(directory, wide)) {
            addRelaxed(g_counters.pathRefused);
            return false;
        }

        // Один уровень содержимого. Обход при этом всё равно делает то, что
        // умеет: нормализует корень (FR-6), не идёт по ссылкам, ловит петли и
        // перечисляет каталог по одному буферу ядра. maxDepth = 1 означает «дети
        // предъявлены, их содержимое не читается» — ровно один уровень.
        mvfs::WalkOptions walkOptions;
        walkOptions.maxDepth = 1;
        // Размеры здесь не меряются: разворот локатора смотрит только на имя,
        // вид и путь, а лишний GetCompressedFileSizeW на каждом элементе каждого
        // каталога удвоил бы syscall-ы в самом горячем месте скана.
        VfsProbeOptions listOptions = options_;
        listOptions.measureAllocated = false;

        const mvfs::WalkResult result = mvfs::walk(
            wide, walkOptions,
            [&out, &listOptions](const mvfs::WalkEntry& entry) {
                // Исключение из посетителя platform::vfs::walk поймал бы сам и
                // записал в errors, но тогда результат выглядел бы полным при
                // неполном списке. Поэтому граница своя: не смогли — Stop, и
                // вызывающий получает честный false.
                try {
                    ProbeEntry converted;
                    if (convertEntry(entry, listOptions, converted)) {
                        out.push_back(std::move(converted));
                    } else {
                        addRelaxed(g_counters.pathRefused);
                    }
                } catch (const std::exception&) {
                    return mvfs::WalkStep::Stop;
                }
                // Спускаться некуда: maxDepth = 1, и Continue здесь безопасен.
                return mvfs::WalkStep::Continue;
            },
            token);

        addRelaxed(g_counters.directoriesListed);
        addRelaxed(g_counters.elementErrors, result.stats.errors);
        if (rootUnreadable(result, wide)) {
            addRelaxed(g_counters.rootRefused);
            return false;
        }
        return result.completed;
    } catch (const std::exception& failure) {
        // Наружу из адаптера не летит ничего (SPEC §12): нехватка памяти на
        // векторе результата — это «здесь ничего не нашлось», а не падение скана.
        addRelaxed(g_counters.rootRefused);
        core::logError("engine.probe.failure", failure.what());
        return false;
    }
}

bool VfsFileSystemProbe::statPath(std::string_view path, std::stop_token token, ProbeEntry& out) {
    addRelaxed(g_counters.statCalls);
    const StageClock statClock(g_counters.statNs);
    if (path.empty() || token.stop_requested()) return false;

    try {
        const std::string requested(path);
        // Настоящее имя объекта (FR-6): снимает 8.3, разрешает ссылки, приводит
        // том к букве диска. Именно этот путь потом сравнивается с путями
        // элементов обхода, поэтому обе стороны обязаны быть одной формы.
        const mpaths::Resolution resolution = mpaths::resolve(requested);
        if (!resolution.resolved) {
            addRelaxed(g_counters.rootRefused);
            core::logFailure("engine.probe.stat", "путь не читается: элемента нет или нет прав", requested,
                             static_cast<std::int64_t>(platform::hresultFromWin32(resolution.lastError)));
            return false;
        }

        out = ProbeEntry{};
        out.path = std::string(stripExtendedPrefix(resolution.path));
        out.isDirectory = resolution.directory;
        out.reparsePoint = resolution.reparsePoint;
        addRelaxed(g_counters.statsRead);

        // Две вещи, которые мост сообщает, но о которых не решает.
        //
        // 1) Корень-ссылка. GetFinalPathNameByHandleW идёт по ссылке, поэтому
        //    out.path — имя ЦЕЛИ, а out.reparsePoint честно говорит, что сам
        //    объект точкой перехода. Обходить ли такой корень — решение
        //    сборщика (FR-6), и добавлять его здесь означало бы завести вторую
        //    правду о ссылках. Элементы дерева при этом ссылками остаются:
        //    platform::vfs::walk их не раскрывает (пункт 2 шапки vfs_walk.hpp).
        //
        // 2) strong == false означает, что имя получено «как открыли»
        //    (FILE_NAME_OPENED), то есть 8.3 не развёрнут. Обычный обход
        //    нормализует корень сам (FILE_NAME_NORMALIZED) и вернёт длинное
        //    имя, поэтому в этом редком углу кандидат может получить
        //    outsideRoot на своих же файлах. Молча чинить это здесь нельзя —
        //    подмена пути обошлась бы удалением не того каталога (FR-6); зато
        //    видно это в CollectStats::outsideRoot и в notes отчёта.

        // Время и атрибуты одного элемента: FindFirstFileW на путь без
        // подстановочных знаков — это stat, а не обход (никакой рекурсии и
        // перечисления содержимого здесь не происходит).
        std::wstring extended;
        if (!platform::tryToUtf16(resolution.path, extended)) {
            addRelaxed(g_counters.pathRefused);
            return true;
        }
        WIN32_FIND_DATAW findData{};
        const HANDLE find = ::FindFirstFileW(extended.c_str(), &findData);
        if (find != INVALID_HANDLE_VALUE) {
            ::FindClose(find);
            out.writeTime = fileTimeToUnix(toTicks(findData.ftLastWriteTime));
            out.accessTime = fileTimeToUnix(toTicks(findData.ftLastAccessTime));
            // Запись уже в руках, поэтому размеры — одним вызовом
            // (GetCompressedFileSizeW), а не двумя чтениями с разных моментов.
            applySize(mvfs::measureFindData(findData, extended), out);
        } else {
            // Время не отдалось — это не повод объявить путь несуществующим:
            // нормализация уже доказала, что объект есть. Размер меряем по пути.
            applySize(mvfs::measurePath(extended), out);
        }
        // Счётчики осмысленны только для того, что считается кандидатом: у
        // каталога аллоцированного размера нет by design (его стоимость считает
        // вызывающий), у ссылки — тоже, и ни то ни другое не делает прогон
        // неполным.
        if (!out.isDirectory && !out.reparsePoint) {
            if (out.allocatedKnown) {
                addRelaxed(g_counters.allocatedMeasured);
            } else {
                addRelaxed(g_counters.allocatedUnknown);
            }
        }
        return true;
    } catch (const std::exception& failure) {
        addRelaxed(g_counters.rootRefused);
        core::logError("engine.probe.failure", failure.what());
        return false;
    }
}

bool VfsFileSystemProbe::walk(std::string_view root, std::stop_token token, const EntryVisitor& visit) {
    addRelaxed(g_counters.walkCalls);
    const StageClock walkClock(g_counters.walkNs);
    if (root.empty() || !visit) {
        addRelaxed(g_counters.rootRefused);
        return false;
    }

    try {
        std::wstring wide;
        if (!platform::tryToUtf16(root, wide)) {
            addRelaxed(g_counters.pathRefused);
            return false;
        }

        addRelaxed(g_counters.walks);
        mvfs::WalkOptions walkOptions;
        walkOptions.maxDepth = options_.maxWalkDepth;
        // followReparsePoint остаётся false навсегда: FR-6 запрещает идти по
        // ссылкам, а ProbeEntry помечает их reparsePoint для сборщика.
        const mvfs::WalkResult result = mvfs::walk(
            wide, walkOptions,
            [this, &visit](const mvfs::WalkEntry& entry) { return visitEntry(options_, visit, entry); }, token);

        addRelaxed(g_counters.elementErrors, result.stats.errors);
        if (rootUnreadable(result, wide)) addRelaxed(g_counters.rootRefused);
        return result.completed;
    } catch (const std::exception& failure) {
        addRelaxed(g_counters.rootRefused);
        core::logError("engine.probe.failure", failure.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Фабрика и диагностика
// ---------------------------------------------------------------------------

std::unique_ptr<FileSystemProbe> makeVfsFileSystemProbe(VfsProbeOptions options) {
    return std::make_unique<VfsFileSystemProbe>(options);
}

// ---------------------------------------------------------------------------
// Профиль по этапам (docs/scan-performance.md)
// ---------------------------------------------------------------------------

namespace {

// Путь файла профиля из переменной окружения. GetEnvironmentVariableW, а не
// std::getenv: MSVC помечает узкий getenv как C4996, а сборка идёт с /WX, и
// широкая функция отдаёт путь в UTF-8, а не в кодовой странице консоли.
[[nodiscard]] std::string stageProfileTarget() {
    wchar_t buffer[1024];
    const DWORD written =
        ::GetEnvironmentVariableW(L"MRPROPER_SCAN_PROFILE", buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) return {};
    std::string out;
    out.reserve(written);
    for (DWORD index = 0; index < written; ++index) {
        out.push_back(buffer[index] < 0x80 ? static_cast<char>(buffer[index]) : '?');
    }
    return out;
}

}  // namespace

bool stageProfileEnabled() {
    // Один раз на процесс: переменная окружения в горячем коде не читается, а
    // на время работы она не меняется.
    static const bool enabled = !stageProfileTarget().empty();
    return enabled;
}

void appendVfsProbeProfileLine(const std::string& path, const std::string& line) {
    if (path.empty() || !stageProfileEnabled()) return;
    static std::mutex writeMutex;
    const std::lock_guard<std::mutex> guard(writeMutex);
    // fopen помечен в MSVC как небезопасный (C4996 при /W4 /WX), fopen_s есть
    // только у CRT от Microsoft — ровно как в core/log.cpp и platform/vfs_walk.cpp.
    std::FILE* file = nullptr;
    if (::fopen_s(&file, (path + ".probe").c_str(), "ab") != 0 || file == nullptr) return;
    std::fwrite(line.data(), 1, line.size(), file);
    std::fclose(file);
}

namespace {

// Сводка счётчиков слоя одной строкой «ключ=значение». Наносекунды печатаются
// и в миллисекундах (для отчёта) и в наносекундах (для деления на элементы):
// округление до миллисекунды на десятках тысяч вызовов съело бы всю разницу.
std::string probeProfileLine(const char* tag, const VfsProbeStats& s) {
    const auto ms = [](std::uint64_t ns) { return std::to_string(ns / 1000000ull); };
    std::string line = tag;
    line += " walks=" + std::to_string(s.walks);
    line += " walkMs=" + ms(s.walkNs) + " walkNs=" + std::to_string(s.walkNs);
    line += " listCalls=" + std::to_string(s.listCalls) + " listMs=" + ms(s.listNs);
    line += " statCalls=" + std::to_string(s.statCalls) + " statMs=" + ms(s.statNs);
    line += " convertCalls=" + std::to_string(s.convertCalls) + " convertMs=" + ms(s.convertNs);
    line += " allocCalls=" + std::to_string(s.allocCalls) + " allocMs=" + ms(s.allocNs) +
            " allocUs=" + std::to_string(s.allocCalls != 0 ? s.allocNs / s.allocCalls / 1000ull : 0ull);
    line += " visitCalls=" + std::to_string(s.visitCalls) + " visitMs=" + ms(s.visitNs);
    line += " entries=" + std::to_string(s.entries);
    line += " files=" + std::to_string(s.files);
    line += " dirsListed=" + std::to_string(s.directoriesListed);
    line += " statsRead=" + std::to_string(s.statsRead);
    line += " allocMeasured=" + std::to_string(s.allocatedMeasured);
    line += " allocUnknown=" + std::to_string(s.allocatedUnknown);
    line += " rootRefused=" + std::to_string(s.rootRefused);
    line += " elementErrors=" + std::to_string(s.elementErrors);
    line += " pathRefused=" + std::to_string(s.pathRefused);
    line += "\n";
    return line;
}

}  // namespace

VfsProbeStats vfsProbeStats() noexcept {
    VfsProbeStats stats;
    stats.walks = g_counters.walks.load(std::memory_order_relaxed);
    stats.entries = g_counters.entries.load(std::memory_order_relaxed);
    stats.files = g_counters.files.load(std::memory_order_relaxed);
    stats.directoriesListed = g_counters.directoriesListed.load(std::memory_order_relaxed);
    stats.statsRead = g_counters.statsRead.load(std::memory_order_relaxed);
    stats.allocatedMeasured = g_counters.allocatedMeasured.load(std::memory_order_relaxed);
    stats.allocatedUnknown = g_counters.allocatedUnknown.load(std::memory_order_relaxed);
    stats.rootRefused = g_counters.rootRefused.load(std::memory_order_relaxed);
    stats.elementErrors = g_counters.elementErrors.load(std::memory_order_relaxed);
    stats.pathRefused = g_counters.pathRefused.load(std::memory_order_relaxed);
    stats.allocCalls = g_counters.allocCalls.load(std::memory_order_relaxed);
    stats.allocNs = g_counters.allocNs.load(std::memory_order_relaxed);
    stats.convertCalls = g_counters.convertCalls.load(std::memory_order_relaxed);
    stats.convertNs = g_counters.convertNs.load(std::memory_order_relaxed);
    stats.convertAllocNs = g_counters.convertAllocNs.load(std::memory_order_relaxed);
    stats.statCalls = g_counters.statCalls.load(std::memory_order_relaxed);
    stats.statNs = g_counters.statNs.load(std::memory_order_relaxed);
    stats.listCalls = g_counters.listCalls.load(std::memory_order_relaxed);
    stats.listNs = g_counters.listNs.load(std::memory_order_relaxed);
    stats.walkCalls = g_counters.walkCalls.load(std::memory_order_relaxed);
    stats.walkNs = g_counters.walkNs.load(std::memory_order_relaxed);
    stats.visitCalls = g_counters.visitCalls.load(std::memory_order_relaxed);
    stats.visitNs = g_counters.visitNs.load(std::memory_order_relaxed);
    return stats;
}

void resetVfsProbeStats() noexcept {
    // Профиль: сброс — единственный момент, когда «предыдущая серия» уже не с
    // чем смешать. Сводка пишется ДО обнуления, иначе это была бы пустая строка.
    if (stageProfileEnabled()) {
        appendVfsProbeProfileLine(stageProfileTarget(), probeProfileLine("PROBE", vfsProbeStats()));
    }

    g_counters.walks.store(0, std::memory_order_relaxed);
    g_counters.entries.store(0, std::memory_order_relaxed);
    g_counters.files.store(0, std::memory_order_relaxed);
    g_counters.directoriesListed.store(0, std::memory_order_relaxed);
    g_counters.statsRead.store(0, std::memory_order_relaxed);
    g_counters.allocatedMeasured.store(0, std::memory_order_relaxed);
    g_counters.allocatedUnknown.store(0, std::memory_order_relaxed);
    g_counters.rootRefused.store(0, std::memory_order_relaxed);
    g_counters.elementErrors.store(0, std::memory_order_relaxed);
    g_counters.pathRefused.store(0, std::memory_order_relaxed);
    g_counters.allocCalls.store(0, std::memory_order_relaxed);
    g_counters.allocNs.store(0, std::memory_order_relaxed);
    g_counters.convertCalls.store(0, std::memory_order_relaxed);
    g_counters.convertNs.store(0, std::memory_order_relaxed);
    g_counters.convertAllocNs.store(0, std::memory_order_relaxed);
    g_counters.statCalls.store(0, std::memory_order_relaxed);
    g_counters.statNs.store(0, std::memory_order_relaxed);
    g_counters.listCalls.store(0, std::memory_order_relaxed);
    g_counters.listNs.store(0, std::memory_order_relaxed);
    g_counters.walkCalls.store(0, std::memory_order_relaxed);
    g_counters.walkNs.store(0, std::memory_order_relaxed);
    g_counters.visitCalls.store(0, std::memory_order_relaxed);
    g_counters.visitNs.store(0, std::memory_order_relaxed);
}

std::string describeVfsProbeStats() {
    const VfsProbeStats stats = vfsProbeStats();
    // Профиль печатается здесь тоже: resetVfsProbeStats вызывает не каждый
    // вызывающий, а отчёт читается всегда. Флаг держит таблицу одноразовой,
    // иначе она дописывалась бы в файл на каждый вызов.
    if (stageProfileEnabled()) {
        static std::atomic<bool> dumped{false};
        if (!dumped.exchange(true, std::memory_order_relaxed)) {
            appendVfsProbeProfileLine(stageProfileTarget(), probeProfileLine("PROBE", stats));
        }
    }
    if (!stats.degraded()) return {};

    std::string text = "скан прошёл в неполных условиях (нет прав админа или часть элементов исчезла):";
    const auto part = [&text](std::uint64_t value, std::string_view title) {
        if (value == 0) return;
        text += ' ';
        text.append(title);
        text += " — ";
        text += std::to_string(value);
        text += ';';
    };
    part(stats.rootRefused, "корней недоступно");
    part(stats.elementErrors, "ошибок чтения в обходе");
    part(stats.allocatedUnknown, "файлов без аллоцированного размера (оценка по логическому)");
    part(stats.pathRefused, "элементов с нечитаемым именем");
    return text;
}

}  // namespace mrproper::engine
