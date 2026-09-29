// Интеграционный тест обхода ФС: «создать дерево каталогов → просканировать →
// сверить сумму». Спека §11.2 («обёртки WinAPI против реальных путей: создать
// структуру каталогов → просканировать → сверить с суммой GetCompressedFileSize»).
//
// Почему файл в integration, а не в unit. Проверяется не арифметика, а договор
// Win32-слоя на настоящей файловой системе: нормализация пути через
// GetFinalPathNameByHandleW, перечисление через
// GetFileInformationByHandleEx(FileIdBothDirectoryInfo), логический размер из
// записи каталога и аллоцированный из GetCompressedFileSizeW. На виртуальной
// фикстуре всё это проверялось бы тем же кодом, который и выдаёт числа, —
// то есть никак.
//
// Главное свойство файла: СУММА ПРОВЕРЯЕТСЯ НЕ ТЕМ ЖЕ КОДОМ. Ожидаемые числа
// берутся из двух независимых источников:
//
//   1) описание фикстуры (массив kTree) — что именно мы создали;
//   2) «голый» обход на FindFirstFileW/FindNextFileW в этом же файле — что
//      видит файловая система, когда её не спрашивают у модуля обхода.
//
// Эталонный обход намеренно оставлен на FindFirstFileW: он идёт ДРУГИМ кодом,
// чем модуль, и поэтому способен поймать ошибку, общую у обоих. Обратная
// сторона: FindFirstFileW не понимает шаблон в пути с префиксом \\?\ (отвечает
// ERROR_BAD_LENGTH на пути, который CreateFileW открывает без вопросов), поэ-
// тому эталон работает с той же файловой системой в обычной форме пути —
// toPlainPath ниже. Это не потеря независимости, а требование Win32: две формы
// записи одного и того же каталога.
//
// Если обход потеряет элемент, посчитает его дважды или возьмёт логический
// размер не оттуда, откуда надо, расхождение всплывёт на сумме и на множестве
// путей, а не в невнятном «кажется, что-то не то». Аллоцированный размер
// дополнительно сверяется с прямым вызовом GetCompressedFileSizeW — буквально
// той функцией, которую называет спека.
//
// Прав администратора не требуется: фикстура живёт в %TEMP% пользователя, а
// создание файлов и каталогов — операции без повышения привилегий. Отдельные
// случаи §11.2 (sparse-файлы, длинные пути, reparse-петля, заблокированный
// файл) — это другие задачи набора, здесь их нет намеренно.
//
// Конвенция набора: main() определён ровно один раз — в этом файле. Остальные
// файлы набора только регистрируют TEST(...) через harness.hpp (как в
// tests/unit): второй main даст LNK2005, и это правильный способ об этом
// узнать.

#include "harness.hpp"

#include <windows.h>  // NOLINT(bugprone-suspicious-include) — набор Win32-тестов, ему положено

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.hpp"
#include "platform/vfs_size.hpp"
#include "platform/vfs_walk.hpp"
#include "win_handle.hpp"

using mrproper::platform::vfs::EntryKind;
using mrproper::platform::vfs::WalkEntry;
using mrproper::platform::vfs::WalkOptions;
using mrproper::platform::vfs::WalkResult;
using mrproper::platform::vfs::WalkStep;

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---------------------------------------------------------------------------
// Фикстура: дерево каталогов под %TEMP%, удаляется в деструкторе. Имя уникально
// на процесс и на счётчик: два параллельных прогона набора (он идёт в общий
// ctest) иначе столкнулись бы на одном пути.
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxTempPath = 260;

[[nodiscard]] std::wstring toExtended(std::wstring_view path) {
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' && (path[2] == L'?' || path[2] == L'.') &&
        path[3] == L'\\') {
        return std::wstring(path);
    }
    if (path.size() < 2 || path[1] != L':') {
        return std::wstring(path);
    }
    return std::wstring(L"\\\\?\\") + std::wstring(path);
}

// Та же запись пути без префикса \\?\: форма, в которой Win32 разбирает шаблоны
// FindFirstFileW. С префиксом шаблоны выключены вместе с разбором пути, и
// FindFirstFileW(L"\\\\?\\C:\\каталог\\*") отвечает ERROR_BAD_LENGTH (24) на
// пути, который CreateFileW с тем же префиксом открывает без вопросов. Поэтому
// всё, что в этом файле ходит по шаблону (уборка фикстуры, эталонный обход),
// работает в обычной форме. Фикстура живёт в %TEMP%, так что пути короткие и
// MAX_PATH тут не мешает; длинные пути — отдельный случай набора
// (vfsEdge_longPath_*).
[[nodiscard]] std::wstring toPlainPath(std::wstring_view path) {
    constexpr std::wstring_view kExtended = L"\\\\?\\";
    if (path.size() >= kExtended.size() && path.compare(0, kExtended.size(), kExtended) == 0) {
        return std::wstring(path.substr(kExtended.size()));
    }
    return std::wstring(path);
}

[[nodiscard]] bool makeDirectory(const std::wstring& path) {
    if (::CreateDirectoryW(toExtended(path).c_str(), nullptr) != FALSE) {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

// Создать каталог, которого быть НЕ должно. Отличается от makeDirectory
// именно этим: «уже существует» — отказ, а не успех. Фикстура проверок обязана
// быть пустой в момент проверки, и каталог, оставшийся от прошлого прогона с
// тем же pid (Windows регулярно переиспользует идентификаторы процессов), тихо
// выдавался бы за «пустой каталог», который на самом деле полон.
[[nodiscard]] bool createDirectoryFresh(const std::wstring& path) {
    return ::CreateDirectoryW(toExtended(path).c_str(), nullptr) != FALSE;
}

[[nodiscard]] bool removeTree(const std::wstring& path) {
    // Обычная форма пути: FindFirstFileW с шаблоном на пути с префиксом \\?\
    // не работает (см. toPlainPath), а с префиксом фикстура просто осталась бы
    // лежать в %TEMP% после каждого прогона.
    const std::wstring plain = toPlainPath(path);
    WIN32_FIND_DATAW data{};
    const auto find = mrproper::platform::adopt<mrproper::platform::FindHandlePolicy>(
        ::FindFirstFileW((plain + L"\\*").c_str(), &data));
    if (!find) {
        return ::DeleteFileW(plain.c_str()) != FALSE || ::RemoveDirectoryW(plain.c_str()) != FALSE;
    }
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring child = plain + L"\\" + name;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            (void)removeTree(child);
        } else {
            // Read-only у элемента фикстуры не бывает, но снять атрибут
            // дешевле, чем потом гадать, почему каталог не удалился.
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
            }
            ::DeleteFileW(child.c_str());
        }
    } while (::FindNextFileW(find.get(), &data) != FALSE);
    return ::RemoveDirectoryW(plain.c_str()) != FALSE;
}

class TempDir {
public:
    TempDir() {
        std::wstring base(kMaxTempPath, L'\0');
        const DWORD copied = ::GetTempPathW(static_cast<DWORD>(base.size()), base.data());
        if (copied == 0 || copied >= base.size()) {
            return;
        }
        base.resize(copied);
        while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
            base.pop_back();
        }
        static std::atomic<LONG> counter{0};
        // Префикс «mrproper-walk-it-» выбран не случайно: соседние наборы
        // интеграционных тестов называют свои фикстуры «MrProper-it-<pid>-<n>»,
        // а NTFS не различает регистр, и «mrproper-it-…» — это ТОТ ЖЕ
        // каталог. С общими нумераторами фикстур два набора попадали в один
        // каталог: обход показывал чужое дерево (16 элементов вместо 9), а
        // проверка пустого каталога видела непустой. Имя набора в префиксе —
        // дешёвый и окончательный развод.
        const std::wstring name = L"mrproper-walk-it-" +
                                  std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) + L"-" +
                                  std::to_wstring(static_cast<unsigned long>(counter.fetch_add(1)));
        for (int attempt = 0; attempt < 16 && root_.empty(); ++attempt) {
            const std::wstring candidate = base + L"\\" + name + (attempt == 0 ? L"" : L"-" + std::to_wstring(attempt));
            if (createDirectoryFresh(candidate)) {
                root_ = candidate;
            }
        }
    }

    ~TempDir() {
        if (!root_.empty()) {
            (void)removeTree(root_);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] bool ready() const noexcept { return !root_.empty(); }
    [[nodiscard]] const std::wstring& path() const noexcept { return root_; }

    // Путь элемента относительно корня: toExtended уже дал форму \\?\, а
    // разделитель между корнем и относительной частью добавляется здесь.
    // Слепой конкатенацией путь собирать нельзя: без разделителя файл
    // фикстуры становится соседом каталога, и проверка проходит на пустом
    // дереве, ни разу не увидев ни одного элемента.
    [[nodiscard]] std::wstring at(std::wstring_view relative) const {
        std::wstring result = toExtended(root_);
        if (relative.empty()) {
            return result;
        }
        if (result.back() != L'\\') {
            result.push_back(L'\\');
        }
        result.append(relative);
        return result;
    }

private:
    std::wstring root_;
};

// Записать файл ровно заданного размера. Содержимое детерминированное, но для
// проверки размера это не важно — важно, чтобы длина была точной: именно её
// потом и сверяют.
[[nodiscard]] bool writeFile(const std::wstring& path, std::size_t bytes) {
    const auto file = mrproper::platform::adopt(::CreateFileW(toExtended(path).c_str(), GENERIC_WRITE,
                                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }
    constexpr std::size_t kChunk = 512;
    const std::vector<unsigned char> chunk(kChunk, 0xA5u);
    std::size_t left = bytes;
    while (left > 0) {
        const std::size_t part = left < kChunk ? left : kChunk;
        DWORD written = 0;
        if (::WriteFile(file.get(), chunk.data(), static_cast<DWORD>(part), &written, nullptr) == FALSE ||
            written != part) {
            return false;
        }
        left -= part;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Описание фикстуры. Одно место и для создания, и для ожидаемых чисел: если
// «что создали» и «что ждём увидеть» разойдутся, это ошибка теста, а не обхода.
// ---------------------------------------------------------------------------

struct FileSpec {
    const wchar_t* relative;  // путь от корня фикстуры
    std::size_t bytes;
};

constexpr FileSpec kTree[] = {
    {L"alpha.txt", 1000},
    {L"beta.bin", 4096},
    {L"empty.txt", 0},
    {L"sub\\gamma.txt", 2000},
    {L"sub\\deep\\delta.txt", 500},
    {L"sub\\deep\\deeper.txt", 1},
};

constexpr const wchar_t* kTreeDirs[] = {L"sub", L"sub\\deep", L"empty_dir"};

constexpr std::size_t kFileCount = sizeof(kTree) / sizeof(kTree[0]);
constexpr std::size_t kDirCount = sizeof(kTreeDirs) / sizeof(kTreeDirs[0]);
constexpr std::size_t kEntryCount = kFileCount + kDirCount;
constexpr u32 kDeepestLevel = 3;  // sub -> deep -> delta.txt
constexpr u64 kLogicalSum = 1000ull + 4096ull + 0ull + 2000ull + 500ull + 1ull;

// Создать дерево. false — фикстуру создать не удалось: молча проверять пустое
// дерево нельзя, «ноль элементов» прошёл бы как успех.
[[nodiscard]] bool buildTree(const TempDir& root) {
    for (const auto& dir : kTreeDirs) {
        if (!makeDirectory(root.at(dir))) {
            return false;
        }
    }
    for (const auto& file : kTree) {
        if (!writeFile(root.at(file.relative), file.bytes)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Независимый обход на «голом» Win32 — эталон для сверки. Модуль обхода в
// перечислении не участвует: сумма должна совпасть с тем, что файловая система
// показывает сама. Единственное, что у него взято, — канонический корень:
// иначе относительные пути не сопоставить, если %TEMP% задан не в той форме,
// которую вернул бы GetFinalPathNameByHandleW.
// ---------------------------------------------------------------------------

struct RawEntry {
    std::wstring path;
    u64 logicalBytes{};
    u32 attributes{};
};

void rawEnumerateInto(const std::wstring& path, std::vector<RawEntry>& out) {
    WIN32_FIND_DATAW data{};
    const auto find = mrproper::platform::adopt<mrproper::platform::FindHandlePolicy>(
        ::FindFirstFileW((path + L"\\*").c_str(), &data));
    if (!find) {
        return;
    }
    do {
        const std::wstring name(data.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring child = path + L"\\" + name;
        RawEntry entry;
        entry.path = child;
        entry.attributes = data.dwFileAttributes;
        entry.logicalBytes = (static_cast<u64>(data.nFileSizeHigh) << 32) | static_cast<u64>(data.nFileSizeLow);
        out.push_back(std::move(entry));
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            rawEnumerateInto(child, out);
        }
    } while (::FindNextFileW(find.get(), &data) != FALSE);
}

// Канонический корень фикстуры В ОБЫЧНОЙ ФОРМЕ — та, в которой работает
// эталонный обход (см. toPlainPath). Сравнивать его с путями модуля нельзя:
// те в форме \\?\, это две записи одного пути.
[[nodiscard]] std::wstring rawRoot(const TempDir& root) {
    return toPlainPath(mrproper::platform::vfs::normalizePath(root.path()).path);
}

[[nodiscard]] std::vector<RawEntry> rawEnumerate(const TempDir& root) {
    std::vector<RawEntry> out;
    rawEnumerateInto(rawRoot(root), out);
    return out;
}

// Относительный путь от канонического корня, которым пользовался rawEnumerate:
// тот же разбор, что у путей обхода, иначе ключи не совпадут.
[[nodiscard]] std::wstring relativeFromCanonicalRoot(const TempDir& root, const std::wstring& path) {
    const std::wstring base = rawRoot(root);
    if (path.size() <= base.size()) {
        return path;
    }
    std::wstring relative = path.substr(base.size());
    if (!relative.empty() && relative.front() == L'\\') {
        relative.erase(0, 1);
    }
    return relative;
}

// Аллоцированный размер прямым вызовом — буквально та функция, которую называет
// спека §11.2. found == false, если файловой системе ответить нечем (драйвер не
// отдаёт размер): тогда сверять нечего, и тест обязан остаться зелёным.
[[nodiscard]] u64 rawAllocatedBytes(const std::wstring& path, bool& found) {
    DWORD high = 0;
    ::SetLastError(NO_ERROR);
    const DWORD low = ::GetCompressedFileSizeW(path.c_str(), &high);
    if (low == INVALID_FILE_SIZE && ::GetLastError() != NO_ERROR) {
        found = false;
        return 0;
    }
    found = true;
    return (static_cast<u64>(high) << 32) | static_cast<u64>(low);
}

// ---------------------------------------------------------------------------
// Обход с посетителем, который копит всё нужное для сверки.
// ---------------------------------------------------------------------------

struct Scan {
    WalkResult result;
    std::vector<WalkEntry> entries;
    std::map<std::wstring, u64> fileLogical;  // относительный путь -> логический размер
    u64 logicalSum{};
    u64 files{};
    u64 directories{};
    u64 allocatedSum{};
    bool allocatedKnownForAll{true};
};

// Путь элемента минус корень обхода. Корень известен только после возврата из
// walk(), поэтому относительные пути строятся вторым проходом по собранным
// элементам, а не внутри посетителя.
[[nodiscard]] std::wstring relativeTo(const WalkResult& result, const std::wstring& path) {
    if (path.size() <= result.root.size()) {
        return path;
    }
    std::wstring relative = path.substr(result.root.size());
    if (!relative.empty() && relative.front() == L'\\') {
        relative.erase(0, 1);
    }
    return relative;
}

// measureAllocated = true дополнительно меряет каждый файл через measurePath —
// так проверяется связка «обход → размеры» (SPEC §11.2: сверить с суммой
// GetCompressedFileSize).
[[nodiscard]] Scan scanTree(std::wstring_view root, const WalkOptions& options = {}, bool measureAllocated = false) {
    Scan scan;
    std::map<std::wstring, u64> allocated;  // полный путь -> аллоцированный размер
    scan.result = mrproper::platform::vfs::walk(
        root, options, [&scan, &allocated, measureAllocated](const WalkEntry& entry) {
            scan.entries.push_back(entry);
            if (entry.kind == EntryKind::File) {
                ++scan.files;
                scan.logicalSum += entry.logicalBytes;
                if (measureAllocated) {
                    const mrproper::platform::vfs::FileSize size = mrproper::platform::vfs::measurePath(entry.path);
                    if (size.allocatedKnown) {
                        allocated[entry.path] = size.allocatedBytes;
                    } else {
                        scan.allocatedKnownForAll = false;
                    }
                }
            } else if (entry.kind == EntryKind::Directory) {
                ++scan.directories;
            }
            return WalkStep::Continue;
        });
    for (const auto& entry : scan.entries) {
        if (entry.kind != EntryKind::File) {
            continue;
        }
        scan.fileLogical[relativeTo(scan.result, entry.path)] = entry.logicalBytes;
        const auto measured = allocated.find(entry.path);
        if (measured != allocated.end()) {
            scan.allocatedSum += measured->second;
        }
    }
    return scan;
}

// Диагностика: обычный CHECK печатает только текст условия, а «ошибок
// непустой список» без кодов и путей бесполезен — смотреть придётся в отладчик.
// Здесь сообщение собирается из самого результата обхода.
[[noreturn]] void failWithWalkErrors(const char* expression, const char* file, int line, const WalkResult& result) {
    std::string message = std::string(file) + ":" + std::to_string(line) + " " + expression + "; ошибок " +
                          std::to_string(result.errors.size()) + " из " + std::to_string(result.stats.errors) +
                          ", корень: " + mrproper::core::toUtf8(result.root);
    for (const auto& error : result.errors) {
        message += "; [" + std::to_string(error.win32Code) + "] " + mrproper::core::toUtf8(error.path) + " — " +
                   mrproper::core::toUtf8(error.message);
    }
    throw mrp::Failure{std::move(message)};
}

#define CHECK_WALK(cond, result)                                        \
    do {                                                                \
        if (!(cond)) {                                                  \
            ::failWithWalkErrors(#cond, __FILE__, __LINE__, (result));  \
        }                                                               \
    } while (false)

}  // namespace

// ---------------------------------------------------------------------------
// Проверки
// ---------------------------------------------------------------------------

TEST(walk_visits_created_tree) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    const Scan scan = scanTree(root.path());

    CHECK(scan.result.completed);
    CHECK(!scan.result.canceled);
    CHECK(!scan.result.stoppedByVisitor);
    CHECK_WALK(scan.result.errors.empty(), scan.result);
    CHECK_EQ(scan.result.stats.errors, u64{0});

    // Счётчики обхода совпадают с тем, что создано.
    CHECK_EQ(scan.result.stats.entries, u64{kEntryCount});
    CHECK_EQ(scan.result.stats.files, u64{kFileCount});
    CHECK_EQ(scan.result.stats.directories, u64{kDirCount});
    CHECK_EQ(scan.result.stats.otherEntries, u64{0});
    CHECK_EQ(scan.result.stats.reparseSkipped, u64{0});
    CHECK_EQ(scan.result.stats.loopsDetected, u64{0});
    CHECK_EQ(scan.result.stats.depthLimitHits, u64{0});
    CHECK_EQ(scan.result.stats.maxDepthReached, kDeepestLevel);

    // Сумма логических размеров — ровно та, что записана в фикстуре.
    CHECK_EQ(scan.logicalSum, kLogicalSum);
    CHECK_EQ(scan.files, u64{kFileCount});
    CHECK_EQ(scan.directories, u64{kDirCount});
    CHECK_EQ(scan.fileLogical.size(), kFileCount);

    // «.», «..» и сам корень посетителю не предъявляются: «сам каталог посещён»
    // и «в каталоге что-то есть» — одно и то же событие (шапка vfs_walk.hpp).
    for (const auto& entry : scan.entries) {
        CHECK(entry.name != L".");
        CHECK(entry.name != L"..");
        CHECK(entry.depth >= 1u);
        CHECK(entry.path != scan.result.root);
    }

    // Файлы — поимённо и по размеру.
    for (const auto& file : kTree) {
        const auto found = scan.fileLogical.find(file.relative);
        CHECK(found != scan.fileLogical.end());
        if (found != scan.fileLogical.end()) {
            CHECK_EQ(found->second, static_cast<u64>(file.bytes));
        }
    }

    // Каталоги — поимённо, с ожидаемой глубиной. Глубина задана явно для каждого
    // каталога, а не выведена из его номера в kTreeDirs: «empty_dir» стоит в
    // фикстуре третьим, но лежит в корне, то есть на глубине 1. Таблица вида
    // «index + 1» тихо требовала от обхода неверной глубины для любого
    // каталога, который не лежит на одной линии с предыдущими, — проверка
    // была написана под форму дерева, а не под его содержимое.
    // Пустой каталог виден, но его содержимого в дереве нет.
    struct DirAtDepth {
        const wchar_t* relative;
        u32 depth;
    };
    constexpr DirAtDepth kDirs[] = {
        {L"sub", 1u},
        {L"sub\\deep", 2u},
        {L"empty_dir", 1u},
    };
    static_assert(std::size(kDirs) == kDirCount, "каждый каталог фикстуры должен быть проверен");
    for (const auto& expected : kDirs) {
        bool seen = false;
        for (const auto& entry : scan.entries) {
            if (entry.kind == EntryKind::Directory && relativeTo(scan.result, entry.path) == expected.relative) {
                seen = true;
                CHECK_EQ(entry.depth, expected.depth);
            }
        }
        CHECK(seen);
    }
    for (const auto& entry : scan.entries) {
        if (relativeTo(scan.result, entry.path) == L"sub\\deep\\delta.txt") {
            CHECK_EQ(entry.depth, kDeepestLevel);
        }
    }
}

// Регрессия на дефект, который стоил обхода целиком. GetFileInformationByHandleEx
// в ответе на FileIdBothDirectoryInfo ОТДАЁТ «.» и «..» (проверено прямым
// вызовом: первая запись каталога — «.», FileNameLength == 2), хотя обход
// считает их несуществующими. Пропущенные фильтром они дают: элемент с именем
// «.», спуск в «..» (выход за пределы корня, SPEC §4 FR-6) и спуск в «.»
// (второй визит в тот же каталог, то есть петля и лишние счётчики).
TEST(walk_dot_and_dotdot_never_reach_the_visitor) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    const Scan scan = scanTree(root.path());
    CHECK(scan.result.completed);
    CHECK_WALK(scan.result.errors.empty(), scan.result);
    CHECK_EQ(scan.result.stats.entries, u64{kEntryCount});

    for (const auto& entry : scan.entries) {
        CHECK(entry.name != L".");
        CHECK(entry.name != L"..");
        CHECK(entry.path != scan.result.root + L"\\.");
        CHECK(entry.path != scan.result.root + L"\\..");
        CHECK(entry.path.find(L"\\..\\") == std::wstring::npos);
        CHECK(mrproper::platform::vfs::pathIsInsideRoot(entry.path, scan.result.root));
    }
    CHECK_EQ(scan.result.stats.loopsDetected, u64{0});
}

TEST(walk_logical_sum_matches_raw_listing) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    const Scan scan = scanTree(root.path());
    CHECK(scan.result.completed);

    // Эталон: то, что перечисляет сама файловая система.
    std::map<std::wstring, u64> rawFiles;
    u64 rawSum = 0;
    for (const auto& entry : rawEnumerate(root)) {
        if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        rawFiles[relativeFromCanonicalRoot(root, entry.path)] = entry.logicalBytes;
        rawSum += entry.logicalBytes;
    }

    CHECK_EQ(rawFiles.size(), kFileCount);
    CHECK_EQ(rawSum, kLogicalSum);
    CHECK_EQ(scan.logicalSum, rawSum);

    // Ни одного лишнего элемента и ни одного потерянного: множества путей
    // совпадают целиком, а не только по сумме.
    CHECK_EQ(scan.fileLogical.size(), rawFiles.size());
    for (const auto& pair : rawFiles) {
        const auto found = scan.fileLogical.find(pair.first);
        CHECK(found != scan.fileLogical.end());
        if (found != scan.fileLogical.end()) {
            CHECK_EQ(found->second, pair.second);
        }
    }
    for (const auto& pair : scan.fileLogical) {
        const auto found = rawFiles.find(pair.first);
        CHECK(found != rawFiles.end());
        if (found != rawFiles.end()) {
            CHECK_EQ(found->second, pair.second);
        }
    }
}

TEST(walk_allocated_sum_matches_get_compressed_file_size) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    const Scan scan = scanTree(root.path(), WalkOptions{}, true);
    CHECK(scan.result.completed);
    CHECK_EQ(scan.fileLogical.size(), kFileCount);

    u64 rawSum = 0;
    u64 moduleSum = 0;
    std::size_t compared = 0;
    for (const auto& entry : rawEnumerate(root)) {
        if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        bool found = false;
        const u64 rawBytes = rawAllocatedBytes(entry.path, found);
        if (!found) {
            continue;  // файловая система не ответила — сверять нечего
        }
        ++compared;
        rawSum += rawBytes;

        const mrproper::platform::vfs::FileSize size = mrproper::platform::vfs::measurePath(entry.path);
        CHECK(size.ok());
        if (size.allocatedKnown) {
            moduleSum += size.allocatedBytes;
            // measurePath обязан вернуть ровно то, что вернул прямой вызов
            // GetCompressedFileSizeW по тому же пути.
            CHECK_EQ(size.allocatedBytes, rawBytes);
        }
        // Инвариант для несжатых и неразреженных файлов: аллоцированный не
        // меньше логического (округление до кластера). У сжатого и
        // разреженного — наоборот, поэтому для них не проверяем.
        if ((entry.attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) == 0) {
            CHECK(rawBytes >= entry.logicalBytes);
        }
    }

    CHECK(compared == kFileCount);
    CHECK_EQ(moduleSum, rawSum);
    // Освобождаемое по аллоцированному не меньше суммы данных: иначе оценка
    // освобождения уехала бы в минус на любом дереве с файлами.
    CHECK(rawSum >= kLogicalSum);
    CHECK(scan.allocatedSum == rawSum || !scan.allocatedKnownForAll);
}

TEST(walk_root_is_normalized_and_every_path_inside_it) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    const auto normalized = mrproper::platform::vfs::normalizePath(root.path());
    CHECK(normalized.resolved);
    CHECK(normalized.isDirectory);
    CHECK(!normalized.reparsePoint);
    CHECK_EQ(normalized.win32Code, u32{0});
    CHECK(normalized.path.rfind(L"\\\\?\\", 0) == 0);
    CHECK(normalized.path.back() != L'\\');

    const Scan scan = scanTree(root.path());
    CHECK_EQ(scan.result.root, normalized.path);

    // Путь, который отдаёт правило из JSON, приходит в форме \\?\: корень
    // обхода обязан лежать внутри него (SPEC §4 FR-6). Обычная форма без
    // префикса с расширенной не сравнивается и проверкой быть не может — это
    // не ошибка, а две формы записи одного пути.
    CHECK(mrproper::platform::vfs::pathIsInsideRoot(scan.result.root, L"\\\\?\\" + root.path()));
    CHECK(mrproper::platform::vfs::pathIsInsideRoot(scan.result.root, scan.result.root));

    for (const auto& entry : scan.entries) {
        CHECK(mrproper::platform::vfs::pathIsInsideRoot(entry.path, scan.result.root));
        CHECK(entry.path.rfind(scan.result.root + L"\\", 0) == 0);
    }

    // Проверка обязана быть граничной: соседний каталог с тем же префиксом —
    // не «внутри», иначе выход за пределы корня проходит.
    CHECK(!mrproper::platform::vfs::pathIsInsideRoot(scan.result.root + L"x", scan.result.root));
    CHECK(!mrproper::platform::vfs::pathIsInsideRoot(L"\\\\?\\C:\\Windows", scan.result.root));

    // Сравнение регистронезависимое — так же, как у самой файловой системы.
    std::wstring upper = scan.result.root;
    for (auto& symbol : upper) {
        if (symbol >= L'a' && symbol <= L'z') {
            symbol = static_cast<wchar_t>(symbol - L'a' + L'A');
        }
    }
    CHECK(mrproper::platform::vfs::pathIsInsideRoot(upper + L"\\ALPHA.TXT", scan.result.root));
}

TEST(walk_empty_directory_reports_nothing) {
    const TempDir root;
    CHECK(root.ready());

    const Scan scan = scanTree(root.path());
    CHECK(scan.result.completed);
    CHECK(scan.entries.empty());
    CHECK_EQ(scan.result.stats.entries, u64{0});
    CHECK_EQ(scan.logicalSum, u64{0});
    CHECK_WALK(scan.result.errors.empty(), scan.result);
    // Пустое дерево и неоткрывшийся корень — разные результаты: первый
    // сообщает «смотреть было не на что», второй — «смотреть не смогли».
    CHECK(!scan.result.root.empty());
}

TEST(walk_missing_root_is_error_not_crash) {
    const TempDir root;
    CHECK(root.ready());

    // Ошибка не фатальна (SPEC §4 FR-6): результат возвращён, счётчики целы,
    // причина названа.
    const WalkResult missing = mrproper::platform::vfs::walk(root.at(L"нет-такого"));
    CHECK(missing.completed);
    CHECK_EQ(missing.stats.entries, u64{0});
    CHECK(!missing.errors.empty());
    CHECK(missing.errors.front().win32Code != 0u);
    CHECK(!missing.errors.front().message.empty());
    CHECK_EQ(missing.stats.errors, static_cast<u64>(missing.errors.size()));

    const WalkResult empty = mrproper::platform::vfs::walk(L"");
    CHECK(!empty.errors.empty());
}

TEST(walk_file_as_root_is_reported_as_directory_error) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(writeFile(root.at(L"file.bin"), 32));

    // Файл передан как корень. Молчаливый «пустой обход» был бы худшим
    // ответом: сканер сказал бы «в каталоге ничего нет».
    const Scan scan = scanTree(root.at(L"file.bin"));
    CHECK(scan.entries.empty());
    CHECK(!scan.result.errors.empty());
    CHECK_EQ(scan.result.errors.front().win32Code, static_cast<u32>(ERROR_DIRECTORY));
}

TEST(walk_max_depth_stops_descent) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    WalkOptions options;
    options.maxDepth = 1;
    const Scan scan = scanTree(root.path(), options);

    CHECK(scan.result.completed);
    // Только содержимое корня: два каталога предъявлены, но не раскрыты.
    CHECK_EQ(scan.result.stats.entries, u64{5});
    CHECK_EQ(scan.result.stats.files, u64{3});
    CHECK_EQ(scan.result.stats.directories, u64{2});
    CHECK_EQ(scan.result.stats.depthLimitHits, u64{2});
    CHECK_EQ(scan.result.stats.maxDepthReached, 1u);
    for (const auto& entry : scan.entries) {
        CHECK_EQ(entry.depth, 1u);
    }
    u32 limited = 0;
    for (const auto& entry : scan.entries) {
        if (entry.depthLimitReached) {
            ++limited;
            CHECK(entry.kind == EntryKind::Directory);
        }
    }
    CHECK_EQ(limited, u32{2});
}

TEST(walk_visitor_skip_directory) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    u64 seen = 0;
    const WalkResult partial = mrproper::platform::vfs::walk(root.path(), {}, [&seen](const WalkEntry& entry) {
        ++seen;
        if (entry.kind == EntryKind::Directory && entry.name == L"sub") {
            return WalkStep::SkipDirectory;
        }
        return WalkStep::Continue;
    });

    CHECK(partial.completed);
    CHECK_EQ(seen, u64{5});
    CHECK_EQ(partial.stats.entries, u64{5});
    // Исключение поддерева посетителем — это не ошибка и не предел глубины.
    CHECK_EQ(partial.stats.depthLimitHits, u64{0});
    CHECK(partial.errors.empty());

    // Контрольный обход целиком видит поддерево: пропущено оно было решением
    // посетителя, а не обходом.
    const Scan full = scanTree(root.path());
    CHECK_EQ(full.logicalSum, kLogicalSum);
    CHECK(full.fileLogical.count(L"sub\\deep\\delta.txt") == 1u);
}

TEST(walk_visitor_stop_returns_partial_result) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    const WalkResult stopped =
        mrproper::platform::vfs::walk(root.path(), {}, [](const WalkEntry&) { return WalkStep::Stop; });

    CHECK(!stopped.completed);
    CHECK(!stopped.canceled);
    CHECK(stopped.stoppedByVisitor);
    CHECK_EQ(stopped.stats.entries, u64{1});
}

TEST(walk_pre_canceled_stop_token_stops_immediately) {
    const TempDir root;
    CHECK(root.ready());
    CHECK(buildTree(root));

    std::stop_source source;
    source.request_stop();
    u64 seen = 0;
    const WalkResult canceled = mrproper::platform::vfs::walk(
        root.path(), {}, [&seen](const WalkEntry&) {
            ++seen;
            return WalkStep::Continue;
        },
        source.get_token());

    CHECK(!canceled.completed);
    CHECK(canceled.canceled);
    CHECK(!canceled.stoppedByVisitor);
    CHECK_EQ(seen, u64{0});
    CHECK_EQ(canceled.stats.entries, u64{0});
}

TEST(walk_unicode_names_survive_unchanged) {
    const TempDir root;
    CHECK(root.ready());

    // Кириллица, греческая буква и эмодзи вне BMP. Символ задан сурогатной
    // парой кодом: литерал с настоящим эмодзи зависит от кодировки исходника,
    // а код — нет.
    const wchar_t* kUnicodeDirs[] = {L"Данные", L"Данные\\вложение"};
    const FileSpec kUnicodeFiles[] = {
        {L"Данные\\отчёт.txt", 111},
        {L"Данные\\вложение\\\xD83D\xDDC1-мусор.bin", 222},
        {L"Данные\\χ-ray.dat", 3},
    };
    for (const auto& dir : kUnicodeDirs) {
        CHECK(makeDirectory(root.at(dir)));
    }
    for (const auto& file : kUnicodeFiles) {
        CHECK(writeFile(root.at(file.relative), file.bytes));
    }

    const Scan scan = scanTree(root.path());
    CHECK(scan.result.completed);
    CHECK_WALK(scan.result.errors.empty(), scan.result);
    CHECK_EQ(scan.result.stats.files, u64{sizeof(kUnicodeFiles) / sizeof(kUnicodeFiles[0])});
    CHECK_EQ(scan.result.stats.directories, u64{sizeof(kUnicodeDirs) / sizeof(kUnicodeDirs[0])});

    u64 expectedSum = 0;
    for (const auto& file : kUnicodeFiles) {
        const auto found = scan.fileLogical.find(file.relative);
        CHECK(found != scan.fileLogical.end());
        if (found != scan.fileLogical.end()) {
            CHECK_EQ(found->second, static_cast<u64>(file.bytes));
        }
        expectedSum += static_cast<u64>(file.bytes);
    }
    CHECK_EQ(scan.logicalSum, expectedSum);

    // Имена приходят байт в байт, без потери и перекодировки: Win32-W-функции
    // на входе и выходе, ни одной конвертации кодировок.
    bool astralSeen = false;
    for (const auto& entry : scan.entries) {
        CHECK(!entry.name.empty());
        CHECK(mrproper::platform::vfs::pathIsInsideRoot(entry.path, scan.result.root));
        if (entry.name.find(L"\xD83D\xDDC1") != std::wstring::npos) {
            astralSeen = true;
        }
    }
    CHECK(astralSeen);

    // Сумма по юникодному дереву совпадает и с эталонным перечислением.
    u64 rawSum = 0;
    for (const auto& entry : rawEnumerate(root)) {
        if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            rawSum += entry.logicalBytes;
        }
    }
    CHECK_EQ(scan.logicalSum, rawSum);
}

// Единственный main() набора — см. соглашение в шапке файла.
int main() {
    return mrp::runAll("MrProper integration");
}
