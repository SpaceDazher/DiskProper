// Исполнение очистки по манифесту: удаляется ровно перечисленное, и только
// перечисленное.
//
// Задача H4. Спека: §4 FR-6 (защита от съедания чужих данных: нормализация
// пути, принадлежность корню правила, пропуск reparse points), FR-7 (корзина
// приложения и отмена), FR-5 (план показывается до удаления), §10 (критический
// риск «Повреждение системных данных при ошибке в правиле»), §12 («отсутствие
// изменений вне корней правил», «все ошибки в логе с путём и HRESULT»).
//
// ---------------------------------------------------------------------------
// Что здесь проверяется и почему это нельзя проверить юнит-тестом
// ---------------------------------------------------------------------------
//
// Проверяемое свойство — не решение, а поведение на диске: после прогона
// исполнителя файл из списка разрешённого отсутствует, а соседний файл того же
// каталога, которого в списке нет, остался. Переносимый юнит-тест на
// core::plan доказывает только другую половину — что план вообще не выдаст
// действие кандидату без списка (tests/unit/plan_tests.cpp,
// plan_manifestProducesOperationsWithoutItFailsClosed). Соблазн «исполнитель
// удалит по списку» на живом томе проверяется только здесь: подделать
// deleteTree в юнит-тесте нельзя, а обход F-01 был именно на диске.
//
// Пять проверок, по одной на каждое утверждение:
//
//   1. executor_manifest_listedFileIsDeletedAndNothingElse
//      Файл из списка удалён, а сам корень кандидата — нет: право снести корень
//      есть только у манифеста с rootDeleteAllowed, и его здесь нет.
//   2. executor_manifest_unlistedNeighbourSurvives        <- главная, P0-класса
//      Файл рядом, в том же каталоге, НЕ в списке остаётся на месте, тогда как
//      перечисленный файл того же каталога удалён. Контроль обязателен: если бы
//      удалилось «всё понемногу» или «ничего», проверка была бы зелёной и
//      ничего бы не доказывала.
//   3. executor_manifest_partialDirectoryRemovesOnlyListedFiles
//      Каталог с несколькими файлами, где в списке часть: перечисленное уходит,
//      остальное остаётся; каталог, из которого ушёл весь список, убирается, а
//      непустой — нет.
//   4. executor_manifest_listEntryOutsideRuleRootIsRefused
//      Путь в списке, оказавшийся за пределами корня правила, даёт отказ с
//      кодом HRESULT, и файл на месте, — а СВОЙ файл из того же списка
//      удаляется. Это проверка границы FR-6 на уровне элемента списка:
//      корень защищает каталог, а не «любой путь из плана». Свой файл в
//      списке обязателен: при отказе всего подряд проверка «отказ с кодом»
//      осталась бы зелёной и не доказывала бы ничего.
//   5. executor_manifest_candidateOutsideRuleRootIsRefused
//      Тот же приём на кандидате: он вне корня правила, поэтому
//      SkippedOutsideRoot до единого обращения к файловой системе (фаза A),
//      файл цел — а второй кандидат, внутри границы, в этом же прогоне
//      удаляется.
//   6. executor_manifest_trashTransactionIsRestorable
//      Транзакция корзины появляется на диске (каталог + manifest.json +
//      committed), перенесён только перечисленный файл, а восстановление
//      возвращает его на место с тем же содержимым (FR-7: «перенос отменяем»).
//
// ---------------------------------------------------------------------------
// Песочница и её границы (правило задачи: никакого реального удаления)
// ---------------------------------------------------------------------------
//
// Всё создаётся в ОДНОМ каталоге %TEMP%\MrProper-exec-<pid>-<N>\, который тест
// и его каталог «правила», и каталог корзины прогона (trashRoot). Настоящий
// %TEMP% пользователя, AppData, кэши браузеров и ProgramData не затрагиваются
// никогда: корзина по умолчанию — это %ProgramData%\MrProper\Trash, и без
// явного trashRoot прогон писал бы туда. Собственный корень корзины — не
// удобство, а условие проверки: тест обязана быть уборщиком, не создателем
// мусора в ProgramData.
//
// Уборка фикстуры — собственный код этого файла (removeTreeW), а не
// vfs::deleteTree: проверяемый модуль не должен убирать за собой последствия
// собственных сбоев, иначе красный прогон оставил бы в %TEMP% пачку каталогов.
// Удаляется только дерево собственной песочницы, путь к которой известен из
// GetTempPathW плюс сгенерированное имя.
//
// ---------------------------------------------------------------------------
// Честность результата
// ---------------------------------------------------------------------------
//
// Ни одна проверка здесь не требует прав администратора: файлы, каталоги и
// корзина создаются в пользовательском %TEMP%, а Restart Manager опрашивается
// обычным процессом. Предпосылки, которые на конкретной машине могут не
// выполниться (нет %TEMP%, временный том без прав на запись, корень правила
// отвергнут как точка монтирования), печатаются как «[skip] <имя>: <причина>»
// и не выдают за успех. Молчаливый пропуск здесь означал бы «исполнитель
// проверен», хотя не проверен ни разу.
//
// ---------------------------------------------------------------------------
// Ссылка на слой engine
// ---------------------------------------------------------------------------
//
// CleanupExecutor живёт в mrproper_engine, а tests/integration/CMakeLists.txt
// линкует набор только с mrproper_platform и харнессом. Правка чужого файла мне
// не принадлежит, поэтому библиотека подключается отсюда: путь отсчитывается от
// каталога проекта сборки (build\<слот>\tests\integration), конфигурация — из
// _DEBUG. Это единственное место в файле, где завязана сборка, и оно же должно
// исчезнуть, когда набор начнёт линковать слой engine штатно.
#include "harness.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#if defined(_DEBUG)
#pragma comment(lib, "..\\..\\src\\engine\\Debug\\mrproper_engine.lib")
#else
#pragma comment(lib, "..\\..\\src\\engine\\Release\\mrproper_engine.lib")
#endif
#endif

#include "core/model.hpp"
#include "core/plan.hpp"
#include "core/trash.hpp"
#include "engine/executor.hpp"
#include "platform/vfs_delete.hpp"
#include "platform/vfs_paths.hpp"
#include "platform/vfs_trash.hpp"
#include "platform/win_error.hpp"
#include "platform/win_handle.hpp"

namespace {

namespace core = mrproper::core;
namespace engine = mrproper::engine;
namespace pf = mrproper::platform::vfs_paths;
namespace trash = mrproper::platform;
namespace vfs = mrproper::platform::vfs;

using ScopedHandle = mrproper::platform::unique_handle<mrproper::platform::KernelHandlePolicy>;

// Порядковый номер фикстуры: два прогона, случившихся одновременно, не должны
// делить один каталог (иначе счётчики «удалено/осталось» мешали бы друг другу).
std::atomic<unsigned> g_fixtureCounter{0u};

// Размер и содержимое файлов-фикстуры. Содержимое разное у каждого файла:
// после восстановления из корзины сравнение байтов и есть доказательство, что
// вернулся ТОТ ЖЕ файл, а не «какой-то файл того же размера».
constexpr std::size_t kPayloadBytes = 4096u;

// HRESULT, которым слой платформы помечает отказ по границе правила: политика
// «путь вне корня» даёт ERROR_ACCESS_DENIED (5). Сравнение идёт с этим числом,
// а не с «просто отказом»: иначе проверка кода превратилась бы в повторную
// проверку самого факта отказа.
const std::int32_t kAccessDeniedHresult = static_cast<std::int32_t>(trash::hresultFromWin32(ERROR_ACCESS_DENIED));

void skip(const char* testName, const std::string& reason) {
    std::printf("  [skip] %s: %s\n", testName, reason.c_str());
}

[[nodiscard]] std::string payloadFor(char seed) {
    std::string data(kPayloadBytes, seed);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<char>(seed + static_cast<char>(index % 23u));
    }
    return data;
}

// Расширенная форма пути: Win32 обрезает путь на MAX_PATH. Собирается здесь, а
// не через vfs_trash, — фикстуру создаёт сам тест, иначе поломка нормализации в
// проверяемом модуле была бы неотличима от «тест не смог создать фикстуру».
[[nodiscard]] std::wstring extended(std::wstring_view path) {
    static const std::wstring prefix = L"\\\\?\\";
    if (path.rfind(prefix, 0) == 0) return std::wstring(path);
    if (path.size() >= 2u && path[1] == L':') return prefix + std::wstring(path);
    return std::wstring(path);
}

[[nodiscard]] bool makeDirectories(const std::wstring& path, std::string& why) {
    // Родитель раньше потомка: «C:\Temp\a\b» требует, чтобы «C:\Temp\a» уже был.
    // Путь режется в исходном виде, без разбора префикса «\\?\» обратно в
    // сегменты: префикс отключает разбор, и «C:\Temp\a\b» после среза префикса
    // осталось бы «?:\C:\Temp\a\b» с буквой диска в имени каталога.
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 3u) {
        if (!makeDirectories(path.substr(0, slash), why)) return false;
    }
    if (::CreateDirectoryW(extended(path).c_str(), nullptr) != FALSE) return true;
    const DWORD error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS) return true;
    why = "не удалось создать каталог " + trash::toUtf8(path) + ": " + trash::win32ErrorText(error) + " (" +
          std::to_string(error) + ")";
    return false;
}

[[nodiscard]] bool writeFile(const std::wstring& path, char seed, std::string& why) {
    const std::string data = payloadFor(seed);
    ScopedHandle file(::CreateFileW(extended(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD error = ::GetLastError();
        why = "не удалось создать файл " + trash::toUtf8(path) + ": " + trash::win32ErrorText(error) + " (" +
              std::to_string(error) + ")";
        return false;
    }
    std::size_t written = 0;
    while (written < data.size()) {
        const std::size_t left = data.size() - written;
        const DWORD chunk = static_cast<DWORD>(left > 0x10000u ? 0x10000u : left);
        DWORD done = 0;
        if (::WriteFile(file.get(), data.data() + written, chunk, &done, nullptr) == FALSE || done == 0) {
            why = "не удалось записать файл фикстуры " + trash::toUtf8(path);
            return false;
        }
        written += done;
    }
    return true;
}

[[nodiscard]] bool readFile(const std::wstring& path, std::string& out, std::string& why) {
    ScopedHandle file(::CreateFileW(extended(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        why = "не удалось открыть файл " + trash::toUtf8(path) + ": " +
              trash::win32ErrorText(::GetLastError());
        return false;
    }
    out.assign(kPayloadBytes, '\0');
    std::size_t read = 0;
    while (read < out.size()) {
        DWORD done = 0;
        const std::size_t left = out.size() - read;
        const DWORD chunk = static_cast<DWORD>(left > 0x10000u ? 0x10000u : left);
        if (::ReadFile(file.get(), out.data() + read, chunk, &done, nullptr) == FALSE || done == 0) break;
        read += done;
    }
    out.resize(read);
    if (read != kPayloadBytes) {
        why = "файл " + trash::toUtf8(path) + " прочитан не полностью: " + std::to_string(read) + " из " +
              std::to_string(kPayloadBytes);
        return false;
    }
    return true;
}

[[nodiscard]] bool exists(const std::wstring& path) {
    return ::GetFileAttributesW(extended(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

[[nodiscard]] bool isDirectoryW(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(extended(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Уборка песочницы. Три прохода: антивирус и индексатор Windows берут каталог на
// долю секунды после удаления содержимого, и один проход оставил бы мусор.
void removeTreeW(const std::wstring& path) {
    const std::wstring ext = extended(path);
    if (::GetFileAttributesW(ext.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!isDirectoryW(path)) {
        ::SetFileAttributesW(ext.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::DeleteFileW(ext.c_str());
        return;
    }
    WIN32_FIND_DATAW data{};
    ScopedHandle find(::FindFirstFileW((ext + L"\\*").c_str(), &data));
    if (find) {
        do {
            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            removeTreeW(ext + L"\\" + name);
        } while (::FindNextFileW(find.get(), &data) != FALSE);
    }
    for (int attempt = 0; attempt < 3 && ::GetFileAttributesW(ext.c_str()) != INVALID_FILE_ATTRIBUTES; ++attempt) {
        if (attempt > 0) ::Sleep(150);
        if (::RemoveDirectoryW(ext.c_str()) != FALSE) return;
    }
}

// ---------------------------------------------------------------------------
// Фикстура
// ---------------------------------------------------------------------------
//
// Раскладка (всё внутри одного каталога в %TEMP%, корень правила и корневая
// песочница — разные каталоги, чтобы «вне корня правила» означало настоящий
// другой каталог, а не подкаталог):
//
//   <sandbox>\rule\                 корень правила (ruleRoot)
//   <sandbox>\rule\a-listed.txt     файл, который будет в списке
//   <sandbox>\rule\b-unlisted.txt   файл РЯДОМ, которого в списке не будет
//   <sandbox>\rule\sub\a-listed.txt в списке; после прогона каталог пуст и уходит
//   <sandbox>\rule\sub\b-unlisted.txt не в списке; из-за него sub остаётся
//   <sandbox>\foreign\outside.txt   за пределами корня правила: не трогать ни при каких списках
//   <sandbox>\trash\                корень корзины прогона (вместо %ProgramData%)
class Sandbox {
public:
    Sandbox() {
        wchar_t buffer[MAX_PATH + 1] = {};
        const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
        if (length == 0 || length > MAX_PATH) {
            reason_ = "GetTempPathW не дал пригодный путь";
            return;
        }
        std::wstring base(buffer);
        if (base.empty() || base.back() != L'\\') base.push_back(L'\\');
        root_ = base + L"MrProper-exec-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(g_fixtureCounter.fetch_add(1u));
        ruleRoot_ = root_ + L"\\rule";
        subDir_ = ruleRoot_ + L"\\sub";
        lonelyDir_ = ruleRoot_ + L"\\lonely";
        foreignDir_ = root_ + L"\\foreign";
        trashRoot_ = root_ + L"\\trash";
        listedTop_ = ruleRoot_ + L"\\a-listed.txt";
        unlistedTop_ = ruleRoot_ + L"\\b-unlisted.txt";
        listedSub_ = subDir_ + L"\\a-listed.txt";
        unlistedSub_ = subDir_ + L"\\b-unlisted.txt";
        listedLonely_ = lonelyDir_ + L"\\a-listed.txt";
        outside_ = foreignDir_ + L"\\outside.txt";
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    // Деструктор обязан отработать и при провале проверки: harness ловит
    // исключение, и без RAII в %TEMP% после каждого красного прогона осталась бы
    // пачка «MrProper-exec-…». Никаких CHECK здесь — только best effort.
    ~Sandbox() {
        if (root_.empty()) return;  // %TEMP% не найден: создавать было нечего
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (::GetFileAttributesW(extended(root_).c_str()) == INVALID_FILE_ATTRIBUTES) return;
            removeTreeW(root_);
            ::Sleep(150);
        }
        if (::GetFileAttributesW(extended(root_).c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::printf("  [warn] не удалось убрать песочницу %s\n", trash::toUtf8(root_).c_str());
        }
    }

    [[nodiscard]] bool create(std::string& why) {
        if (root_.empty()) {
            why = reason_;
            return false;
        }
        if (!makeDirectories(subDir_, why) || !makeDirectories(lonelyDir_, why) ||
            !makeDirectories(foreignDir_, why) || !makeDirectories(trashRoot_, why)) {
            return false;
        }
        // Содержимое разное у каждого файла: после прогона «файл на месте» должно
        // означать «тот же файл», а не «файл того же размера».
        if (!writeFile(listedTop_, 'A', why) || !writeFile(unlistedTop_, 'B', why) ||
            !writeFile(listedSub_, 'C', why) || !writeFile(unlistedSub_, 'D', why) ||
            !writeFile(listedLonely_, 'E', why) || !writeFile(outside_, 'X', why)) {
            return false;
        }
        // Предпосылка, без которой фаза A откажет всему подряд: корень правила
        // обязан быть годной границей (не точка монтирования, разрешимый путь).
        // Не выполниться это может на томе, где %TEMP% сам является ссылкой, —
        // тогда проверка честно сообщает причину, а не падает загадочно.
        const pf::RootGuard guard = pf::prepareRoot(ruleRootUtf8());
        if (!guard.resolved || guard.rejected) {
            why = "корень правила не пригоден как граница (resolved=" + std::to_string(guard.resolved) +
                  ", rejected=" + std::to_string(guard.rejected) + ", win32 " + std::to_string(guard.lastError) +
                  "): " + trash::toUtf8(ruleRoot_);
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::wstring& root() const noexcept { return root_; }
    [[nodiscard]] const std::wstring& ruleRoot() const noexcept { return ruleRoot_; }
    [[nodiscard]] const std::wstring& subDir() const noexcept { return subDir_; }
    [[nodiscard]] const std::wstring& lonelyDir() const noexcept { return lonelyDir_; }
    [[nodiscard]] const std::wstring& foreignDir() const noexcept { return foreignDir_; }
    [[nodiscard]] const std::wstring& trashRoot() const noexcept { return trashRoot_; }
    [[nodiscard]] const std::wstring& listedTop() const noexcept { return listedTop_; }
    [[nodiscard]] const std::wstring& unlistedTop() const noexcept { return unlistedTop_; }
    [[nodiscard]] const std::wstring& listedSub() const noexcept { return listedSub_; }
    [[nodiscard]] const std::wstring& unlistedSub() const noexcept { return unlistedSub_; }
    [[nodiscard]] const std::wstring& listedLonely() const noexcept { return listedLonely_; }
    [[nodiscard]] const std::wstring& outside() const noexcept { return outside_; }

    [[nodiscard]] std::string rootUtf8() const { return trash::toUtf8(root_); }
    [[nodiscard]] std::string ruleRootUtf8() const { return trash::toUtf8(ruleRoot_); }
    [[nodiscard]] std::string trashRootUtf8() const { return trash::toUtf8(trashRoot_); }
    [[nodiscard]] std::string foreignDirUtf8() const { return trash::toUtf8(foreignDir_); }

private:
    std::wstring root_;
    std::wstring ruleRoot_;
    std::wstring subDir_;
    std::wstring lonelyDir_;
    std::wstring foreignDir_;
    std::wstring trashRoot_;
    std::wstring listedTop_;
    std::wstring unlistedTop_;
    std::wstring listedSub_;
    std::wstring unlistedSub_;
    std::wstring listedLonely_;
    std::wstring outside_;
    std::string reason_;
};

// ---------------------------------------------------------------------------
// Сборка прогона
// ---------------------------------------------------------------------------

// Кандидат ровно такой, каким его отдаёт сборщик: каталог-корн��ь, объём равен
// сумме списка разрешённого (иначе validatePlan отверг бы план, а «обещание
// освободится N» не опиралось бы ни на что), уровень Safe, уверенность 100.
[[nodiscard]] core::CleanupCandidate candidateFor(const std::string& pathUtf8, const std::string& displayName,
                                                  const std::vector<core::AllowedEntry>& allowed) {
    core::CleanupCandidate candidate;
    candidate.ruleId = "h4.sandbox";
    candidate.category = "temp";
    candidate.path = pathUtf8;
    candidate.displayName = displayName;
    candidate.safety = core::SafetyLevel::Safe;
    candidate.confidence = 100;
    for (const core::AllowedEntry& entry : allowed) {
        candidate.allocatedBytes += entry.allocatedBytes;
        candidate.logicalBytes += entry.allocatedBytes;
        ++candidate.fileCount;
    }
    candidate.oldestWrite = 1'000'000'000;  // далеко в прошлом: правило «старше N дней»
    candidate.newestWrite = 1'000'000'000;
    candidate.lastAccess = 1'000'000'000;
    candidate.reasons.push_back("песочница интеграционной проверки H4");
    return candidate;
}

[[nodiscard]] core::AllowedEntry entryFor(const std::wstring& path) {
    return core::AllowedEntry{trash::toUtf8(path), static_cast<std::uint64_t>(kPayloadBytes)};
}

// Манифест кандидата: список разрешённого есть, права снести корень целиком —
// нет. Именно эта комбинация и обязана приводить к удалению поштучно.
[[nodiscard]] core::CandidateManifest manifestFor(std::size_t index, const std::string& ruleId,
                                                  const std::string& rootPathUtf8,
                                                  const std::vector<core::AllowedEntry>& allowed) {
    core::CandidateManifest manifest;
    manifest.candidateIndex = index;
    manifest.ruleId = ruleId;
    manifest.rootPath = rootPathUtf8;
    manifest.allowed = core::CandidateManifest::makeAllowedSet(allowed);
    manifest.rootDeleteAllowed = false;
    return manifest;
}

struct RunOutcome {
    engine::ExecutionRefusal refusal{engine::ExecutionRefusal::Completed};
    engine::ExecutionReportPtr report;
};

// Прогон без отказов, отмен и незакрытых дел: только такие строки и не
// показываются в выводе.
[[nodiscard]] bool reportIsClean(const engine::ExecutionReport& report) noexcept {
    return report.errors.empty() && report.errorsDropped == 0 && report.cancelledItems == 0 && !report.cancelled &&
           report.failed == 0 && report.requiresReboot == 0;
}

// Один прогон исполнителя. checkLocks остаётся включённым: блокировки — часть
// боевого пути (FR-6), и выключать их «ради теста» означало бы проверять не то,
// что работает у пользователя. deleteAttempts выше спецификационных трёх:
// единственный, кто может держать файл в %TEMP%, — антивирус, и его отпускание
// занимает доли секунды; отказ из-за этого означал бы «тест не дождался», а не
// «исполнитель не удалил».
[[nodiscard]] RunOutcome runExecutor(const std::vector<core::CleanupCandidate>& candidates,
                                     const core::CleanupPlan& plan, const std::string& ruleRootUtf8,
                                     const std::string& trashRootUtf8) {
    engine::CleanupExecutorOptions options;
    options.checklist.ruleRootOverride = ruleRootUtf8;
    options.trashRoot = trashRootUtf8;
    options.appVersion = "h4-test";
    options.checkLocks = true;
    options.deleteAttempts = 5;
    options.deleteBackoff = std::chrono::milliseconds{100};
    options.maxDeleteBackoff = std::chrono::milliseconds{1000};
    options.logProgress = false;
    options.logFailures = false;

    engine::CleanupExecutor executor(options);
    const engine::Checklist checklist = engine::buildChecklist(candidates, plan, options.checklist);
    RunOutcome outcome;
    outcome.refusal = executor.run(candidates, plan, checklist);
    outcome.report = executor.result();
    // Подробности строк — только когда прогон не сошёлся с ожиданием. На зелёном
    // прогоне это шум в выводе набора, где проверки принадлежат и другим агентам;
    // на красном — единственное место, где видно СОСТОЯНИЕ исполнителя, а
    // CHECK сообщает только номер строки.
    if (outcome.report != nullptr && !reportIsClean(*outcome.report)) {
        for (const engine::ItemReport& it : outcome.report->items) {
            std::printf("  [состояние] item idx=%zu outcome=%s code=%s hr=%d files=%u dirs=%u probs=%u root=[%s] %s\n",
                        it.candidateIndex, engine::toString(it.outcome), it.code.c_str(), it.hr, it.filesDone,
                        it.dirsDone, it.problems, it.ruleRoot.c_str(), it.detail.c_str());
        }
    }
    return outcome;
}

// Строка отчёта по индексу кандидата. nullptr — строки нет.
[[nodiscard]] const engine::ItemReport* itemFor(const engine::ExecutionReport& report, std::size_t candidateIndex) {
    for (const engine::ItemReport& item : report.items) {
        if (item.candidateIndex == candidateIndex) return &item;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) Файл из разрешённого списка удаляется
// ---------------------------------------------------------------------------

// Самый простой случай и одновременно контроль для остальных: манифест есть,
// в списке один файл, право снести корень — нет. Ожидаемый исход — файл ушёл,
// а каталог-кандидат остался: снести его целиком может только манифест с
// rootDeleteAllowed, иначе «удалить по списку» незаметно превратилось бы в
// «удалить всё» (docs/review-02.md F-01).
TEST(executor_manifest_listedFileIsDeletedAndNothingElse) {
    Sandbox box;
    std::string why;
    if (!box.create(why)) {
        skip("executor_manifest_listedFileIsDeletedAndNothingElse", why);
        return;
    }

    const std::vector<core::AllowedEntry> allowed = {entryFor(box.listedTop())};
    const std::vector<core::CleanupCandidate> candidates = {candidateFor(box.ruleRootUtf8(), "песочница H4", allowed)};
    const std::vector<core::CandidateManifest> manifests = {
        manifestFor(0, "h4.sandbox", box.ruleRootUtf8(), allowed)};

    // Прямое удаление, без корзины: проверка 1 — про границу списка, а не про
    // FR-7 (корзина — проверка 6).
    core::PlanOptions planOptions;
    planOptions.profile = core::SelectionProfile::Everything;
    planOptions.useTrash = false;
    planOptions.dryRun = true;  // FR-5: план сначала показывается
    const core::CleanupPlan plan = core::buildPlan(candidates, planOptions, &manifests);

    {
        // Кто отвечает за отказ. Тот же вызов платформы с теми же аргументами,
        // что и у исполнителя, и ЖИВОЙ корень: view переживает блок, в отличие от
        // deleteAllowedList, где корень берётся из временного объекта. Пока граница
        // считает этот путь разрешённым, отказ deleteAllowedList — его, а не
        // фикстуры. Без этой строки красный прогон читался бы как «тест неверно
        // выбрал путь», и причину пришлось бы искать заново: сейчас она находится
        // за одной строкой CHECK, а не за перебором гипотез.
        const core::CandidateManifest* probeManifest = plan.manifestFor(0);
        CHECK(probeManifest != nullptr);
        if (probeManifest != nullptr && probeManifest->allowed != nullptr) {
            const std::wstring rootWide = trash::toUtf16(probeManifest->rootPath);
            vfs::DeleteOptions probeOptions;
            probeOptions.allowedRoot = rootWide;
            for (const core::AllowedEntry& entry : probeManifest->allowed->entries) {
                CHECK_EQ(static_cast<int>(vfs::checkProtected(trash::toUtf16(entry.path), probeOptions).verdict),
                         static_cast<int>(vfs::ProtectionVerdict::Allowed));
            }
        }
    }

    // FR-5: подтверждённый dry-run — условие удаления, и план этому условию
    // удовлетворяет (тот же отпечаток, что и у подтверждённого плана).
    core::DryRunGate gate;
    CHECK(gate.mustShowBeforeExecute(plan));
    gate.acknowledge(plan);
    CHECK(!gate.mustShowBeforeExecute(plan));

    // План, который уходит в исполнитель, обязан быть согласован с кандидатами:
    // иначе «освободится N» не опирается ни на что.
    CHECK(core::validatePlan(candidates, plan).empty());
    CHECK_EQ(plan.operationCount(), std::size_t{1});
    CHECK_EQ(plan.totals.selectedCount, std::size_t{1});
    CHECK(plan.item(0) != nullptr);
    CHECK(plan.item(0) != nullptr && plan.item(0)->action == core::PlanAction::Delete);
    const core::CandidateManifest* inPlan = plan.manifestFor(0);
    CHECK(inPlan != nullptr);
    CHECK(inPlan != nullptr && inPlan->deletable());
    CHECK(inPlan != nullptr && !inPlan->rootDeleteAllowed);
    CHECK(inPlan != nullptr && inPlan->allowed != nullptr &&
          inPlan->allowed->entries.size() == allowed.size());
    // Список виден и в dry-run: подтверждение относится к операции, а не к пути
    // корня, который на экране выглядит шире того, что будет удалено.
    const core::DryRunReport dryRun = core::makeDryRunReport(candidates, plan);
    CHECK_EQ(dryRun.operations.size(), std::size_t{1});
    if (!dryRun.operations.empty()) {
        CHECK(!dryRun.operations.front().rootDeleteOnly);
        CHECK_EQ(dryRun.operations.front().allowedCount, allowed.size());
    }

    const RunOutcome outcome = runExecutor(candidates, plan, box.ruleRootUtf8(), box.trashRootUtf8());
    CHECK(outcome.refusal == engine::ExecutionRefusal::Completed);
    CHECK(outcome.report != nullptr);
    if (outcome.report == nullptr) return;

    CHECK_EQ(outcome.report->items.size(), std::size_t{1});
    CHECK_EQ(outcome.report->done, std::size_t{1});
    CHECK_EQ(outcome.report->failed, std::size_t{0});
    CHECK_EQ(outcome.report->skipped, std::size_t{0});
    CHECK(outcome.report->errors.empty());
    CHECK(outcome.report->complete());
    CHECK_EQ(outcome.report->plannedBytes, static_cast<std::uint64_t>(kPayloadBytes));

    const engine::ItemReport* item = itemFor(*outcome.report, 0);
    CHECK(item != nullptr);
    if (item != nullptr) {
        CHECK(item->outcome == engine::ItemOutcome::Done);
        CHECK_EQ(std::string(item->code), std::string(engine::toString(engine::ItemOutcome::Done)));
        CHECK_EQ(item->filesDone, std::uint32_t{1});
        // Каталог не снесён: dirsDone == 0, и на диске он есть.
        CHECK_EQ(item->dirsDone, std::uint32_t{0});
        CHECK_EQ(item->hr, std::int32_t{0});
        CHECK(item->ok());
    }

    CHECK(!exists(box.listedTop()));
    CHECK(exists(box.ruleRoot()));      // корень кандидата не тронут
    CHECK(exists(box.unlistedTop()));  // сосед по каталогу не в списке — цел
    CHECK(exists(box.outside()));      // за пределами корня правила — цел
}

// ---------------------------------------------------------------------------
// 2) Файл рядом, но НЕ в списке, остаётся (главная защита P0-класса)
// ---------------------------------------------------------------------------

// Ровно тот обход, который закрывал манифест: правило отобрало файлы по
// min-age/locatorExcludes, отчёт написал «по возрасту отсечено», а исполнитель
// получал путь корня и сносил его целиком — вместе со «свежим» файлом, который
// правило собиралось оставить. Здесь такой свежий файл рядом есть, он НЕ в
// списке, и он обязан уцелеть.
//
// Контроль встроен в ту же проверку: сосед по списку (a-listed.txt) удаляется.
// Иначе «осталось всё» было бы неотличимо от «исполнитель ничего не сделал».
TEST(executor_manifest_unlistedNeighbourSurvives) {
    Sandbox box;
    std::string why;
    if (!box.create(why)) {
        skip("executor_manifest_unlistedNeighbourSurvives", why);
        return;
    }

    const std::vector<core::AllowedEntry> allowed = {entryFor(box.listedTop())};
    const std::vector<core::CleanupCandidate> candidates = {candidateFor(box.ruleRootUtf8(), "песочница H4", allowed)};
    const std::vector<core::CandidateManifest> manifests = {
        manifestFor(0, "h4.sandbox", box.ruleRootUtf8(), allowed)};

    core::PlanOptions planOptions;
    planOptions.profile = core::SelectionProfile::Everything;
    planOptions.useTrash = false;
    const core::CleanupPlan plan = core::buildPlan(candidates, planOptions, &manifests);
    CHECK_EQ(plan.operationCount(), std::size_t{1});

    const RunOutcome outcome = runExecutor(candidates, plan, box.ruleRootUtf8(), box.trashRootUtf8());
    CHECK(outcome.refusal == engine::ExecutionRefusal::Completed);
    CHECK(outcome.report != nullptr);
    if (outcome.report == nullptr) return;

    // Исполнитель отработал, а не простаил: ровно один файл из списка ушёл.
    CHECK_EQ(outcome.report->done, std::size_t{1});
    CHECK_EQ(outcome.report->failed, std::size_t{0});
    const engine::ItemReport* item = itemFor(*outcome.report, 0);
    CHECK(item != nullptr);
    if (item != nullptr) {
        CHECK(item->outcome == engine::ItemOutcome::Done);
        CHECK_EQ(item->filesDone, std::uint32_t{1});
        CHECK(item->ok());
    }

    // Главное утверждение проверки.
    CHECK(!exists(box.listedTop()));   // перечисленного больше нет
    CHECK(exists(box.unlistedTop()));  // а этот — на месте

    // «Остался» — значит «не тронут», а не «восстановлен потом»: содержимое и
    // размер те же, файл не пересоздавался.
    std::string content;
    CHECK(readFile(box.unlistedTop(), content, why));
    CHECK_EQ(content.size(), kPayloadBytes);
    CHECK(content == payloadFor('B'));

    // Соседи в подкаталогах и за пределами корня — тоже целы: граница списка не
    // «закончилась» на подкаталоге.
    CHECK(exists(box.listedSub()));     // в списке его не было
    CHECK(exists(box.unlistedSub()));
    CHECK(exists(box.listedLonely()));
    CHECK(exists(box.outside()));       // за пределами корня правила
    CHECK(exists(box.subDir()));
    CHECK(exists(box.ruleRoot()));
}

// ---------------------------------------------------------------------------
// 3) Каталог с файлами, где в списке часть
// ---------------------------------------------------------------------------

// Частичный список — обычное дело (min-age, исключения, порог размера). Проверка
// требует обоих исходов сразу:
//
//   * из подкаталога, где в списке был весь список файлов, каталог УБИРАЕТСЯ —
//     пустой каталог после чистки это мусор;
//   * подкаталог, где остался неучтённый файл, ОСТАЁТСЯ, и неучтённый файл цел;
//   * корень кандидата остаётся в любом случае (список не даёт права снести его).
TEST(executor_manifest_partialDirectoryRemovesOnlyListedFiles) {
    Sandbox box;
    std::string why;
    if (!box.create(why)) {
        skip("executor_manifest_partialDirectoryRemovesOnlyListedFiles", why);
        return;
    }

    // Часть списка: один файл из корня, один из sub (там второй останется) и
    // единственный файл lonely (каталог опустеет и должен уйти).
    const std::vector<core::AllowedEntry> allowed = {
        entryFor(box.listedTop()), entryFor(box.listedSub()), entryFor(box.listedLonely())};
    const std::vector<core::CleanupCandidate> candidates = {candidateFor(box.ruleRootUtf8(), "песочница H4", allowed)};
    const std::vector<core::CandidateManifest> manifests = {
        manifestFor(0, "h4.sandbox", box.ruleRootUtf8(), allowed)};

    core::PlanOptions planOptions;
    planOptions.profile = core::SelectionProfile::Everything;
    planOptions.useTrash = false;
    const core::CleanupPlan plan = core::buildPlan(candidates, planOptions, &manifests);
    CHECK_EQ(plan.operationCount(), std::size_t{1});
    CHECK(plan.manifestFor(0) != nullptr && plan.manifestFor(0)->allowed != nullptr);
    CHECK(plan.manifestFor(0) != nullptr &&
          plan.manifestFor(0)->allowed->entries.size() == allowed.size());

    const RunOutcome outcome = runExecutor(candidates, plan, box.ruleRootUtf8(), box.trashRootUtf8());
    CHECK(outcome.refusal == engine::ExecutionRefusal::Completed);
    CHECK(outcome.report != nullptr);
    if (outcome.report == nullptr) return;

    CHECK_EQ(outcome.report->done, std::size_t{1});
    CHECK_EQ(outcome.report->failed, std::size_t{0});
    const engine::ItemReport* item = itemFor(*outcome.report, 0);
    CHECK(item != nullptr);
    if (item != nullptr) {
        CHECK(item->outcome == engine::ItemOutcome::Done);
        CHECK_EQ(item->filesDone, static_cast<std::uint32_t>(allowed.size()));
        // Отказов внутри списка нет, иначе «удалено 3» означало бы «попытались 3».
        CHECK_EQ(item->problems, std::uint32_t{0});
        // Каталоги удалены только там, где список выел содержимое: lonely — да,
        // sub — нет, корень кандидата — никогда.
        CHECK_EQ(item->dirsDone, std::uint32_t{1});
        CHECK(!item->detail.empty());
    }

    // Перечисленного больше нет — все три.
    CHECK(!exists(box.listedTop()));
    CHECK(!exists(box.listedSub()));
    CHECK(!exists(box.listedLonely()));
    // Каталог, из которого ушёл весь список, убран.
    CHECK(!exists(box.lonelyDir()));
    // Неучтённое осталось: и файл, и каталог, в котором он лежит.
    CHECK(exists(box.unlistedTop()));
    CHECK(exists(box.unlistedSub()));
    CHECK(exists(box.subDir()));
    // Корень кандидата цел: право снести его есть только у rootDeleteAllowed.
    CHECK(exists(box.ruleRoot()));
    CHECK(exists(box.outside()));
}

// ---------------------------------------------------------------------------
// 4) Элемент списка за пределами корня правила — отказ с кодом
// ---------------------------------------------------------------------------

// Здесь корень кандидата ВНУТРИ границы (иначе отказ пришёл бы ещё в фазе A, как
// в следующей проверке), а в списке — чужой путь ЗА границей и свой, внутри её:
// оба проходят один и тот же deleteEntry, и отказ обязан быть адресным.
// правилом, никогда не удаляется» (§10). Список, в который просочился чужой путь,
// обязан дать отказ с кодом, а файл — остаться на месте.
//
// Здесь корень кандидата ВНУТРИ границы (иначе отказ пришёл бы ещё в фазе A, как
// в следующей проверке), а в списке — только путь за границей.
TEST(executor_manifest_listEntryOutsideRuleRootIsRefused) {
    Sandbox box;
    std::string why;
    if (!box.create(why)) {
        skip("executor_manifest_listEntryOutsideRuleRootIsRefused", why);
        return;
    }

    // Список из двух путей: чужой, за границей, и СВОЙ разрешённый. Второй —
    // контроль против «молчаливого отказа всего подряд»: пока исполнитель
    // отказывает в обоих, проверка «отказ с кодом» остаётся зелёной и не
    // доказывает ничего. С удалением своего файла в том же прогоне отказ
    // становится выборочным, а не тотальным.
    const std::vector<core::AllowedEntry> allowed = {entryFor(box.outside()), entryFor(box.listedTop())};
    const std::vector<core::CleanupCandidate> candidates = {candidateFor(box.ruleRootUtf8(), "песочница H4", allowed)};
    const std::vector<core::CandidateManifest> manifests = {
        manifestFor(0, "h4.sandbox", box.ruleRootUtf8(), allowed)};

    core::PlanOptions planOptions;
    planOptions.profile = core::SelectionProfile::Everything;
    planOptions.useTrash = false;
    const core::CleanupPlan plan = core::buildPlan(candidates, planOptions, &manifests);
    CHECK_EQ(plan.operationCount(), std::size_t{1});

    const RunOutcome outcome = runExecutor(candidates, plan, box.ruleRootUtf8(), box.trashRootUtf8());
    CHECK(outcome.refusal == engine::ExecutionRefusal::Completed);
    CHECK(outcome.report != nullptr);
    if (outcome.report == nullptr) return;

    // Итог строки — Failed, а не Done: deleteAllowedList ставит Failed, когда
    // пропущен хоть один элемент (src/engine/executor.cpp), поэтому «удалил своё
    // и отказал чужому» — это одна отказавшая строка, а не две строки по одной.
    CHECK_EQ(outcome.report->done, std::size_t{0});
    CHECK_EQ(outcome.report->failed, std::size_t{1});
    CHECK(!outcome.report->complete());
    // Отказ виден и в списке отказов: §12 требует, чтобы у отказа были путь и
    // HRESULT, иначе журнал объясняет «ничего не удалилось» только догадками.
    CHECK(!outcome.report->errors.empty());
    if (!outcome.report->errors.empty()) {
        CHECK(!outcome.report->errors.front().path.empty());
        CHECK(!outcome.report->errors.front().message.empty());
        CHECK_EQ(outcome.report->errors.front().hr, kAccessDeniedHresult);
    }

    const engine::ItemReport* item = itemFor(*outcome.report, 0);
    CHECK(item != nullptr);
    if (item != nullptr) {
        CHECK(!engine::isSuccess(item->outcome));
        CHECK(item->failed());
        // Код в строке отчёта: без него «пропущено» и «отказано» неразличимы.
        CHECK(!item->code.empty());
        // Контроль против тотального отказа: свой файл из списка УДАЛЁН (1), чужой
        // пропущен (1). Отказ всего подряд дал бы filesDone == 0.
        CHECK_EQ(item->filesDone, std::uint32_t{1});
        CHECK_EQ(item->problems, std::uint32_t{1});
        // HRESULT из политики границы (ERROR_ACCESS_DENIED), а не 0: «отказали»
        // должно отличаться от «нечего было делать».
        CHECK_EQ(item->hr, kAccessDeniedHresult);
        CHECK(!item->detail.empty());
    }

    // Главное: чужой файл цел, и содержимое то же.
    CHECK(exists(box.outside()));
    std::string content;
    CHECK(readFile(box.outside(), content, why));
    CHECK(content == payloadFor('X'));
    // Свой файл из того же списка УДАЛЁН: отказ был выборочным, а не тотальным.
    CHECK(!exists(box.listedTop()));
    // Каталог правила не тронут, и сосед, которого в списке не было, тоже: список
    // разрешённого не превращается в «удалить всё в корне».
    CHECK(exists(box.ruleRoot()));
    CHECK(exists(box.unlistedTop()));
}

// ---------------------------------------------------------------------------
// 5) Кандидат вне корня правила — отказ до операции
// ---------------------------------------------------------------------------

// Второй конец той же границы: кандидат сам оказался за пределами корня своего
// правила (правило уехало, профиль сменился, корень вывели неверно). Фаза A
// обязана отказать ДО операции, и исход здесь однозначен — SkippedOutsideRoot, а
// не «удалено частично».
TEST(executor_manifest_candidateOutsideRuleRootIsRefused) {
    Sandbox box;
    std::string why;
    if (!box.create(why)) {
        skip("executor_manifest_candidateOutsideRuleRootIsRefused", why);
        return;
    }

    // Два кандидата в одном прогоне: первый — каталог ЗА пределами корня правила,
    // второй — ВНУТРИ него. Второй контролирует «молчаливый отказ всего подряд»:
    // отказ первого сам по себе не доказывает ничего, пока второй кандидат в том
    // же прогоне не удалён.
    const std::vector<core::AllowedEntry> allowedForeign = {entryFor(box.outside())};
    const std::vector<core::AllowedEntry> allowedOwn = {entryFor(box.listedTop())};
    const std::vector<core::CleanupCandidate> candidates = {
        candidateFor(box.foreignDirUtf8(), "чужая папка", allowedForeign),
        candidateFor(box.ruleRootUtf8(), "своя папка", allowedOwn)};
    const std::vector<core::CandidateManifest> manifests = {
        manifestFor(0, "h4.sandbox", box.ruleRootUtf8(), allowedForeign),
        manifestFor(1, "h4.sandbox", box.ruleRootUtf8(), allowedOwn)};

    core::PlanOptions planOptions;
    planOptions.profile = core::SelectionProfile::Everything;
    planOptions.useTrash = false;
    const core::CleanupPlan plan = core::buildPlan(candidates, planOptions, &manifests);
    CHECK_EQ(plan.operationCount(), std::size_t{2});

    const RunOutcome outcome = runExecutor(candidates, plan, box.ruleRootUtf8(), box.trashRootUtf8());
    CHECK(outcome.refusal == engine::ExecutionRefusal::Completed);
    CHECK(outcome.report != nullptr);
    if (outcome.report == nullptr) return;

    // Ровно одна строка отказана фазой A и ровно одна выполнена: тотальный отказ
    // дал бы skipped == 2, и проверка перестала бы что-либо доказывать.
    CHECK_EQ(outcome.report->done, std::size_t{1});
    CHECK_EQ(outcome.report->skipped, std::size_t{1});
    CHECK_EQ(outcome.report->failed, std::size_t{0});
    CHECK_EQ(outcome.report->count(engine::ItemOutcome::SkippedOutsideRoot), std::size_t{1});

    const engine::ItemReport* item = itemFor(*outcome.report, 0);
    CHECK(item != nullptr);
    if (item != nullptr) {
        CHECK(item->outcome == engine::ItemOutcome::SkippedOutsideRoot);
        // Код сравнивается с toString того же исхода, а не с выписанной строкой:
        // иначе смена соглашения об именах молча ломала бы проверку, и её
        // пришлось бы «чинить» новым ожиданием вместо того, чтобы спросить,
        // что именно исполнитель записал в отчёт.
        CHECK_EQ(std::string(item->code), std::string(engine::toString(engine::ItemOutcome::SkippedOutsideRoot)));
        CHECK_EQ(item->filesDone, std::uint32_t{0});
        CHECK(item->skipped());
        // Причина названа: по журналу человек должен понять, ЧЕМ отказано.
        CHECK(!item->detail.empty());
    }

    // Файл за границей цел, и каталог, в котором он лежит, тоже.
    CHECK(exists(box.outside()));
    CHECK(exists(box.foreignDir()));
    std::string content;
    CHECK(readFile(box.outside(), content, why));
    CHECK(content == payloadFor('X'));
    // Свой кандидат удалён: отказ был адресным, а не тотальным. Сосед, которого
    // в списке не было, при этом остался — граница списка действует и здесь.
    CHECK(!exists(box.listedTop()));
    CHECK(exists(box.unlistedTop()));
}

// ---------------------------------------------------------------------------
// 6) Транзакция корзины появляется и восстановление возвращает файлы
// ---------------------------------------------------------------------------

// FR-7: перенос отменяем. Проверка идёт по всей цепочке, а не по одному вызову:
// план выбрал Trash -> исполнитель завёл транзакцию в СВОЕМ корне (в %ProgramData%
// он не пишет: trashRoot задан песочницей) -> на диске есть каталог транзакции и
// манифест в состоянии committed -> перенесён только перечисленный файл ->
// план восстановления (core::planRestoreAll) и платформа возвращают файл на
// исходное место с тем же содержимым.
//
// Манифест в состоянии committed — отдельное утверждение: состояние open читается
// движком отмены как «манифест ещё не записан», то есть элементы лежали бы на
// диске целиком, а восстановить их было бы нельзя.
TEST(executor_manifest_trashTransactionIsRestorable) {
    Sandbox box;
    std::string why;
    if (!box.create(why)) {
        skip("executor_manifest_trashTransactionIsRestorable", why);
        return;
    }

    // В списке один файл: соседний обязан остаться на месте и здесь тоже —
    // перенос в корзину не должен отличаться от удаления по границам.
    const std::vector<core::AllowedEntry> allowed = {entryFor(box.listedTop())};
    const std::vector<core::CleanupCandidate> candidates = {candidateFor(box.ruleRootUtf8(), "песочница H4", allowed)};
    const std::vector<core::CandidateManifest> manifests = {
        manifestFor(0, "h4.sandbox", box.ruleRootUtf8(), allowed)};

    // Маленький объём + useTrash -> план выбирает Trash, а не прямое удаление.
    core::PlanOptions planOptions;
    planOptions.profile = core::SelectionProfile::Everything;
    planOptions.useTrash = true;
    const core::CleanupPlan plan = core::buildPlan(candidates, planOptions, &manifests);
    CHECK_EQ(plan.operationCount(), std::size_t{1});
    CHECK(plan.item(0) != nullptr && plan.item(0)->action == core::PlanAction::Trash);

    const RunOutcome outcome = runExecutor(candidates, plan, box.ruleRootUtf8(), box.trashRootUtf8());
    CHECK(outcome.refusal == engine::ExecutionRefusal::Completed);
    CHECK(outcome.report != nullptr);
    if (outcome.report == nullptr) return;

    CHECK_EQ(outcome.report->done, std::size_t{1});
    CHECK_EQ(outcome.report->failed, std::size_t{0});
    CHECK(!outcome.report->trashTxId.empty());
    CHECK(core::isValidTxId(outcome.report->trashTxId));

    const engine::ItemReport* item = itemFor(*outcome.report, 0);
    CHECK(item != nullptr);
    if (item != nullptr) {
        CHECK(item->outcome == engine::ItemOutcome::Done);
        CHECK_EQ(item->txId, outcome.report->trashTxId);
        CHECK(!item->payload.empty());
        CHECK_EQ(item->filesDone, std::uint32_t{1});
    }

    // Перенесён только перечисленный файл: неучтённый сосед и файл за границей
    // на месте, каталог транзакции — ВНУТРИ песочницы, а не в %ProgramData%.
    CHECK(!exists(box.listedTop()));
    CHECK(exists(box.unlistedTop()));
    CHECK(exists(box.outside()));

    const std::string transactionDir = core::transactionDir(box.trashRootUtf8(), outcome.report->trashTxId);
    CHECK(isDirectoryW(trash::toUtf16(transactionDir)));
    CHECK(exists(trash::toUtf16(core::manifestPath(transactionDir))));

    core::TrashTransaction tx;
    const trash::TrashStatus readStatus = trash::readManifest(transactionDir, tx);
    CHECK(readStatus == trash::TrashStatus::Ok);
    if (readStatus != trash::TrashStatus::Ok) return;
    // committed, а не open: только тогда транзакция отменяема.
    CHECK(tx.state == core::TrashTxState::Committed);
    CHECK_EQ(tx.items.size(), std::size_t{1});
    CHECK_EQ(std::string(tx.txId), outcome.report->trashTxId);
    if (tx.items.empty()) return;
    CHECK_EQ(tx.items.front().originalPath, trash::toUtf8(box.listedTop()));
    CHECK_EQ(tx.items.front().bytes, static_cast<std::uint64_t>(kPayloadBytes));
    CHECK(core::isValidPayloadName(tx.items.front().payload));

    // Содержимое лежит в каталоге транзакции под своим именем элемента.
    const std::wstring stagedPath = trash::toUtf16(core::joinPath(transactionDir, tx.items.front().payload));
    CHECK(exists(stagedPath));
    std::string staged;
    CHECK(readFile(stagedPath, staged, why));
    CHECK(staged == payloadFor('A'));

    // --- Восстановление (FR-7) -------------------------------------------------
    // Места назначения свободны (файл убран), поэтому план восстановления не
    // должен знать конфликтов: конфликт означал бы, что «файла там не было».
    const core::RestorePlan restorePlan = core::planRestoreAll(tx, trash::existsProbe());
    CHECK_EQ(restorePlan.restoreCount, std::size_t{1});
    CHECK_EQ(restorePlan.conflictCount, std::size_t{0});
    CHECK_EQ(restorePlan.missingCount, std::size_t{0});
    CHECK_EQ(restorePlan.restoreBytes, static_cast<std::uint64_t>(kPayloadBytes));

    // Граница у восстановления не задаётся: это внутренняя операция сервиса
    // корзины, а не снос по границе правила (tests/integration/trash_boundary_tests.cpp
    // проверяет, что пустая граница сохраняет сервисное поведение).
    trash::TrashOptions restoreOptions;
    restoreOptions.logFailures = false;
    const trash::TrashRestoreSummary restored = trash::restoreTrashItems(transactionDir, restorePlan, restoreOptions);
    CHECK_EQ(restored.failedCount, std::uint32_t{0});
    CHECK_EQ(restored.conflictCount, std::uint32_t{0});
    CHECK_EQ(restored.restoredCount, std::uint32_t{1});
    CHECK(restored.allRestored());
    CHECK_EQ(restored.restoredBytes, static_cast<std::uint64_t>(kPayloadBytes));

    // Файл вернулся на место, с тем же содержимым: это и есть «перенос отменяем».
    CHECK(exists(box.listedTop()));
    std::string restoredContent;
    CHECK(readFile(box.listedTop(), restoredContent, why));
    CHECK(restoredContent == payloadFor('A'));
    CHECK_EQ(restoredContent.size(), kPayloadBytes);
    // Содержимого в корзине не осталось: перенос, а не копирование.
    CHECK(!exists(stagedPath));
    // Всё, чего не было в списке, не тронуто ни на одном шаге.
    CHECK(exists(box.unlistedTop()));
    CHECK(exists(box.outside()));
    CHECK(exists(box.ruleRoot()));
}
