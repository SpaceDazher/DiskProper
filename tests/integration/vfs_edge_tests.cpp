// Интеграционные тесты краев файловой системы: длинные пути, юникод, петля
// reparse. Спека §11.2 (пункт 2 «Интеграционные (Windows) — обёртки WinAPI
// против реальных путей: … длинные пути; reparse-петля»), §5 («пути: длинные
// пути, пробелы, не-ASCII и кириллица»; «ADS не трогаем»), §4 FR-6 («пропуск
// reparse points (symlink/junction) — защита от петель и выхода за пределы
// пути», «нормализация путей через GetFinalPathNameByHandleW …, поддержка \\?\
// для путей > MAX_PATH»), §10 (риск «длинные пути, reparse-петли, не-ASCII»:
// митигация «GetFinalPathNameByHandleW, \\?\, пропуск reparse, тесты с
// юникодом и петлями»), §12 (интеграционные тесты на «длинные пути;
// reparse-петля»).
//
// Задача 82. Соседи по набору: vfs_walk_tests (обход дерева), guard_tests
// (защищённые пути), inventory_tests (инвентаризация), cancel_tests (отмена).
// Здесь ровно три темы, которые не берёт на себя никто из них: путь длиннее
// MAX_PATH, имя файла в юникоде и ссылка, замкнутая сама на себя.
//
// ---------------------------------------------------------------------------
// Почему эти тесты нельзя написать переносимо
// ---------------------------------------------------------------------------
//
// Всё проверяемое здесь не существует в переносимом ядре: MAX_PATH, \\?\, имя
// 8.3, суррогатные пары, точка монтирования NTFS. Юнит-тест на «логику
// длинного пути» проверял бы модель строки, а не файловую систему Windows, и
// зелёный результат ничего бы не говорил о том, что произойдёт на живом диске.
// Поэтому файл живёт в tests/integration и требует Windows (SPEC §9).
//
// ---------------------------------------------------------------------------
// Честность результата
// ---------------------------------------------------------------------------
//
// Три предпосылки могут не выполняться на конкретной машине, и молчать об этом
// нельзя: без NTFS (временный том на ReFS/FAT) не создаётся точка
// монтирования, на отключённом 8.3 его не выдаёт GetShortPathNameW, на томе
// без TEMP не создаётся фикстура. В этих случаях тест печатает строку
// «[skip] <имя>: <причина>» и выходит, а harness считает его пройденным.
// Строка видна в выводе — пропуск не выглядит как успех (SPEC §12 требует,
// чтобы по журналу было видно, что не отработало).
//
// Ни один тест здесь не требует прав администратора: точка монтирования
// (junction) создаётся обычным пользователем, SeCreateSymbolicLinkPrivilege не
// нужна. Символьные ссылки в тестах не используются намеренно — их создание
// без повышения прав на этой машине невозможно, и тест был бы вечно зелёным
// «пропуском» вместо проверки.
//
// ---------------------------------------------------------------------------
// Уборка
// ---------------------------------------------------------------------------
//
// Фикстуры создаются в %TEMP% и убираются деструктором TempTree через
// vfs::deleteTree — тем же кодом, который проверяется. Точки монтирования
// удаляются раньше дерева: deleteTree по FR-6 reparse point не раскрывает и не
// удаляет (SkippedReparse), поэтому junction, оставшийся внутри каталога, не
// дал бы каталогу опустеть и дерево ушло бы в «осталось не пусто».
#include "harness.hpp"

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "platform/vfs_delete.hpp"
#include "platform/vfs_paths.hpp"
#include "platform/vfs_size.hpp"
#include "platform/vfs_walk.hpp"
#include "platform/win_error.hpp"
#include "platform/win_handle.hpp"

namespace {

namespace pf = mrproper::platform::vfs_paths;
namespace vfs = mrproper::platform::vfs;

using ScopedHandle = mrproper::platform::unique_handle<mrproper::platform::KernelHandlePolicy>;

// Тег точки монтирования из winnt.h. Объявлен здесь, чтобы файл не зависел от
// того, в каком виде его отдаёт SDK: значение зафиксировано архитектурой NTFS.
constexpr std::uint32_t kIoReparseTagMountPoint = 0xA0000003u;

// Смещение PathBuffer в REPARSE_DATA_BUFFER: DWORD тега + пять WORD.
constexpr std::size_t kReparseHeaderBytes = 16u;

// Имя каталога, из которого гарантированно получается путь длиннее MAX_PATH:
// кириллица здесь не украшение, а часть проверки (§5).
constexpr std::wstring_view kLevelSegment = L"глубокий-уровень-очень-длинного-имени";

// Порядковый номер фикстуры: имя должно быть уникальным, иначе два прогона
// тестов, случившиеся одновременно, делили бы один каталог и портили друг
// другу счётчики.
std::atomic<unsigned> g_fixtureCounter{0u};

// ---------------------------------------------------------------------------
// Мелкие помощники путей и файловой системы
// ---------------------------------------------------------------------------

// Расширенная форма пути: без неё Win32 обрезает путь на MAX_PATH символах
// (ровно то, из-за чего существует vfs_paths::toExtendedPath). Здесь форма
// продублирована намеренно: фикстуру создаёт сам тест, а не проверяемый модуль —
// иначе поломка toExtendedPath сделала бы тест неразличимым с «тест не смог
// создать фикстуру».
[[nodiscard]] std::wstring extended(std::wstring_view path) {
    static const std::wstring prefix = L"\\\\?\\";
    if (path.rfind(prefix, 0) == 0) {
        return std::wstring(path);
    }
    if (path.size() >= 2u && path[1] == L':') {
        return prefix + std::wstring(path);
    }
    return std::wstring(path);
}

[[nodiscard]] bool makeDirectory(const std::wstring& path, std::uint32_t& win32Error) {
    const std::wstring ext = extended(path);
    if (::CreateDirectoryW(ext.c_str(), nullptr)) {
        win32Error = ERROR_SUCCESS;
        return true;
    }
    win32Error = ::GetLastError();
    return false;
}

[[nodiscard]] std::uint32_t attributesOf(const std::wstring& path) {
    const std::wstring ext = extended(path);
    const DWORD value = ::GetFileAttributesW(ext.c_str());
    if (value == INVALID_FILE_ATTRIBUTES) {
        return 0u;
    }
    return value;
}

[[nodiscard]] bool exists(const std::wstring& path) {
    return attributesOf(path) != 0u;
}

[[nodiscard]] std::string describeError(const char* what, std::uint32_t win32Error) {
    return std::string(what) + ": " + mrproper::platform::win32ErrorText(win32Error) + " (" +
           std::to_string(win32Error) + ")";
}

// Записать файл ровно заданным содержимым. Короткая запись означала бы, что тест
// меряет не то, что создал, поэтому проверяется весь буфер.
[[nodiscard]] bool writeFile(const std::wstring& path, std::string_view content, std::uint32_t& win32Error) {
    const std::wstring ext = extended(path);
    ScopedHandle file(::CreateFileW(ext.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        win32Error = ::GetLastError();
        return false;
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const std::size_t chunk = (content.size() - written) > 0x10000u ? 0x10000u : content.size() - written;
        DWORD done = 0;
        if (!::WriteFile(file.get(), content.data() + written, static_cast<DWORD>(chunk), &done, nullptr)) {
            win32Error = ::GetLastError();
            return false;
        }
        if (done == 0) {
            win32Error = ERROR_WRITE_FAULT;
            return false;
        }
        written += done;
    }
    win32Error = ERROR_SUCCESS;
    return true;
}

// Создать точку монтирования (junction) linkPath на каталог targetDir. Требует
// только права записи в каталог: в отличие от символьной ссылки junction
// доступен обычному пользователю, поэтому тест не молчит там, где
// SeCreateSymbolicLinkPrivilege нет.
[[nodiscard]] bool createJunction(const std::wstring& linkPath, const std::wstring& targetDir,
                                  std::uint32_t& win32Error) {
    win32Error = ERROR_SUCCESS;
    if (!makeDirectory(linkPath, win32Error)) {
        return false;
    }

    // SubstituteName — NT-путь («\??\C:\…»), PrintName — то, что видит
    // пользователь. Хвостовой разделитель у цели запрещён: целью монтирования
    // иначе становится каталог, который нельзя открыть.
    std::wstring substitute = L"\\??\\" + targetDir;
    while (!substitute.empty() && substitute.back() == L'\\') {
        substitute.pop_back();
    }
    std::wstring printable = targetDir;
    while (printable.size() > 3u && printable.back() == L'\\') {
        printable.pop_back();
    }

    const auto substituteBytes = static_cast<std::size_t>(substitute.size() * sizeof(wchar_t));
    const auto printableBytes = static_cast<std::size_t>(printable.size() * sizeof(wchar_t));
    // Две завершающие нули: конец SubstituteName и конец PrintName.
    const std::size_t payloadBytes = substituteBytes + sizeof(wchar_t) + printableBytes + sizeof(wchar_t);
    std::vector<char> buffer(kReparseHeaderBytes + payloadBytes, 0);

    const auto put16 = [&buffer](std::size_t offset, WORD value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    };
    const DWORD tag = static_cast<DWORD>(kIoReparseTagMountPoint);
    std::memcpy(buffer.data(), &tag, sizeof(tag));                                    // ReparseTag
    put16(4u, static_cast<WORD>(payloadBytes));                                        // ReparseDataLength
    put16(6u, static_cast<WORD>(0));                                                   // Reserved
    put16(8u, static_cast<WORD>(0));                                                   // SubstituteNameOffset
    put16(10u, static_cast<WORD>(substituteBytes));                                   // SubstituteNameLength
    put16(12u, static_cast<WORD>(substituteBytes + sizeof(wchar_t)));                 // PrintNameOffset
    put16(14u, static_cast<WORD>(printableBytes));                                    // PrintNameLength

    std::memcpy(buffer.data() + kReparseHeaderBytes, substitute.c_str(), substituteBytes + sizeof(wchar_t));
    std::memcpy(buffer.data() + kReparseHeaderBytes + substituteBytes + sizeof(wchar_t), printable.c_str(),
                printableBytes + sizeof(wchar_t));

    // FILE_FLAG_OPEN_REPARSE_POINT обязателен: без него CreateFileW идёт по ссылке
    // и DeviceIoControl меняет несуществующий объект.
    ScopedHandle link(::CreateFileW(extended(linkPath).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!link) {
        win32Error = ::GetLastError();
        return false;
    }

    DWORD returned = 0;
    if (!::DeviceIoControl(link.get(), FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                           nullptr, 0, &returned, nullptr)) {
        win32Error = ::GetLastError();
        return false;
    }
    return true;
}

// Снять точку монтирования. RemoveDirectoryW на junction удаляет именно ссылку,
// а не каталог, на который она указывает, — в этом и смысл проверки «петля не
// съедает цель».
[[nodiscard]] bool removeJunction(const std::wstring& linkPath, std::uint32_t& win32Error) {
    const std::wstring ext = extended(linkPath);
    if (::RemoveDirectoryW(ext.c_str())) {
        win32Error = ERROR_SUCCESS;
        return true;
    }
    win32Error = ::GetLastError();
    return false;
}

// Строка пропуска: тест не выполнил проверку по объективной причине, и читатель
// вывода должен это видеть.
void reportSkip(const char* testName, const std::string& reason) {
    std::printf("  [skip] %s: %s\n", testName, reason.c_str());
}

// ---------------------------------------------------------------------------
// Фикстура: каталог в %TEMP%, который убирает за собой
// ---------------------------------------------------------------------------
//
// Деструктор обязан отработать и при провале проверки: harness ловит исключение,
// и без RAII в %TEMP% осталась бы пачка «MrProper-it-…» после каждого красного
// прогона. Поэтому никаких CHECK в деструкторе — только best effort.
class TempTree {
public:
    TempTree() {
        std::wstring tempDir(4096u, L'\0');
        const DWORD needed = ::GetTempPathW(4096u, tempDir.data());
        if (needed == 0u || needed > 4096u) {
            reason_ = "GetTempPathW не дал пригодный путь";
            return;
        }
        tempDir.resize(needed);
        while (tempDir.size() > 3u && tempDir.back() == L'\\') {
            tempDir.pop_back();
        }

        const unsigned index = g_fixtureCounter.fetch_add(1u);
        root_ = tempDir + L"\\MrProper-it-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(index);

        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!makeDirectory(root_, win32Error) && win32Error != ERROR_ALREADY_EXISTS) {
            reason_ = describeError("создать фикстуру в %TEMP%", win32Error);
            root_.clear();
            return;
        }
        ready_ = true;
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        // Сначала ссылки: deleteTree по FR-6 их не трогает.
        for (const std::wstring& link : junctions_) {
            std::uint32_t win32Error = ERROR_SUCCESS;
            (void)removeJunction(link, win32Error);
        }
        junctions_.clear();
        if (root_.empty()) {
            return;
        }
        mrproper::platform::vfs::DeleteOptions options;
        options.allowedRoot = root_;
        // Несколько проходов: антивирус и индексатор Windows успевают взять
        // каталог на долю секунды после удаления содержимого, и один проход
        // оставил бы мусор в %TEMP%.
        for (int attempt = 0; attempt < 3 && exists(root_); ++attempt) {
            if (attempt > 0) {
                ::Sleep(200);
            }
            (void)mrproper::platform::vfs::deleteTree(root_, options);
        }
        if (exists(root_)) {
            std::printf("  [warn] не удалось убрать фикстуру %s\n", mrproper::platform::toUtf8(root_).c_str());
        }
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::wstring& root() const noexcept { return root_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    [[nodiscard]] std::wstring path(std::wstring_view relative) const {
        if (relative.empty()) {
            return root_;
        }
        std::wstring joined = root_;
        if (joined.back() != L'\\') {
            joined.push_back(L'\\');
        }
        joined.append(relative);
        return joined;
    }

    // Точка монтирования, которую снимет деструктор.
    [[nodiscard]] bool link(const std::wstring& linkPath, const std::wstring& targetDir, std::string& error) {
        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!createJunction(linkPath, targetDir, win32Error)) {
            error = describeError("создать точку монтирования", win32Error);
            return false;
        }
        junctions_.push_back(linkPath);
        return true;
    }

private:
    std::wstring root_;
    std::vector<std::wstring> junctions_;
    std::string reason_;
    bool ready_{false};
};

// Содержимое фиксированного размера: размер файла в тестах краёв задаётся точно,
// иначе округление до кластера прячет разницу между логическим и
// аллоцированным размером.
[[nodiscard]] std::string payload(std::size_t size, char seed) {
    std::string data(size, seed);
    for (std::size_t index = 0; index < size; ++index) {
        data[index] = static_cast<char>(seed + static_cast<char>(index % 17u));
    }
    return data;
}

// Собрать элементы обхода. Копия структуры, а не ссылка: посетитель зовётся в
// стеке обхода, и хранить ссылку после возврата нельзя.
[[nodiscard]] std::vector<vfs::WalkEntry> walkCollect(std::wstring_view root) {
    std::vector<vfs::WalkEntry> entries;
    (void)vfs::walk(root, {}, [&entries](const vfs::WalkEntry& entry) {
        entries.push_back(entry);
        return vfs::WalkStep::Continue;
    });
    return entries;
}

// Наращивать вложенность кириллическими каталогами, пока путь не перевалит
// MAX_PATH. Возвращает глубину; созданные каталоги остаются на диске.
[[nodiscard]] std::size_t growBeyondMaxPath(const std::wstring& root, std::wstring& deepest,
                                            std::string& error) {
    deepest = root;
    std::size_t levels = 0;
    while (deepest.size() <= static_cast<std::size_t>(MAX_PATH) && levels < 24u) {
        deepest += L"\\" + std::wstring(kLevelSegment) + std::to_wstring(levels);
        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!makeDirectory(deepest, win32Error)) {
            error = describeError("создать вложенный каталог", win32Error);
            return 0;
        }
        ++levels;
    }
    return levels;
}

}  // namespace

// ---------------------------------------------------------------------------
// Длинные пути
// ---------------------------------------------------------------------------

// Путь длиннее MAX_PATH обязан работать через \\?\, и нормализация обязана его
// увидеть. Без этого очистка «Temp» на машине с глубоким профилем молча
// пропускала бы всё, что не влезло в 260 символов.
TEST(vfsEdge_longPath_tree_beyond_max_path_is_visible) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_longPath_tree_beyond_max_path_is_visible", tree.reason());
        return;
    }

    std::wstring deep;
    std::string growError;
    const std::size_t levels = growBeyondMaxPath(tree.root(), deep, growError);
    if (levels == 0) {
        reportSkip("vfsEdge_longPath_tree_beyond_max_path_is_visible", growError);
        return;
    }
    // Предусловие всего теста: без него проверки ниже ничего не доказывают.
    CHECK(deep.size() > static_cast<std::size_t>(MAX_PATH));
    CHECK(exists(deep));

    // 1) toExtendedPath даёт \\?\ ровно для той формы пути, которая его требует.
    const std::string deepUtf8 = mrproper::platform::toUtf8(deep);
    const std::string extendedUtf8 = pf::toExtendedPath(deepUtf8);
    CHECK(extendedUtf8.rfind("\\\\?\\", 0) == 0);
    CHECK(pf::isRootedFileSystemPath(deepUtf8));
    // Уже расширенная форма повторно не префиксуется: «\\?\\?\\C:\…» — другая
    // цель в диспетчере объектов.
    CHECK(pf::toExtendedPath(extendedUtf8) == extendedUtf8);

    // 2) Нормализация доходит до объекта за пределами MAX_PATH.
    const vfs::NormalizedPath normalized = vfs::normalizePath(deep);
    CHECK(normalized.resolved);
    CHECK(normalized.isDirectory);
    CHECK(!normalized.reparsePoint);
    CHECK(normalized.path.rfind(L"\\\\?\\", 0) == 0);
    // Хвост совпадает побайтно: сам временный каталог может оказаться ссылкой
    // (OneDrive, перенесённый профиль), и тогда префикс отличается — сравниваем
    // только то, что создал тест.
    const std::wstring deepTail = deep.substr(2u);
    CHECK(normalized.path.size() >= deepTail.size());
    CHECK(normalized.path.compare(normalized.path.size() - deepTail.size(), deepTail.size(), deepTail) == 0);

    // 3) Размер каталога меряется, аллоцированный у каталога по контракту
    //    неизвестен (NotApplicable) и подменять его нулём нельзя.
    const vfs::FileSize dirSize = vfs::measurePath(deep);
    CHECK(dirSize.ok());
    CHECK(vfs::hasFlag(dirSize.flags, vfs::FileFlags::Directory));
    CHECK(!dirSize.allocatedKnown);
    CHECK(dirSize.allocatedStatus == vfs::SizeStatus::NotApplicable);

    // 4) Обход видит файл на дне длинного пути и не теряет его по дороге.
    const std::wstring deepFile = deep + L"\\длинный-путь-файл.dat";
    std::uint32_t win32Error = ERROR_SUCCESS;
    const std::string data = payload(5000u, 'L');
    if (!writeFile(deepFile, data, win32Error)) {
        reportSkip("vfsEdge_longPath_tree_beyond_max_path_is_visible",
                   describeError("создать файл на длинном пути", win32Error));
        return;
    }

    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK(!result.canceled);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    CHECK_EQ(result.stats.files, std::uint64_t{1});
    CHECK_EQ(result.stats.directories, static_cast<std::uint64_t>(levels));
    CHECK_EQ(result.stats.entries, static_cast<std::uint64_t>(levels) + 1u);
    // Ни одной ссылки в дереве нет — счётчик обязан быть нулём, иначе тест
    // перестал бы доказывать, что петли тут нет.
    CHECK_EQ(result.stats.reparseSkipped, std::uint64_t{0});
    CHECK_EQ(result.stats.loopsDetected, std::uint64_t{0});
    // Глубина настоящего дерева: levels каталогов и файл на последнем уровне.
    CHECK_EQ(result.stats.maxDepthReached, static_cast<std::uint32_t>(levels) + 1u);

    const std::vector<vfs::WalkEntry> entries = walkCollect(tree.root());
    CHECK_EQ(entries.size(), levels + 1u);
    std::size_t deepFileSeen = 0;
    for (const vfs::WalkEntry& entry : entries) {
        // Ни один предъявленный путь не выходит за пределы корня обхода.
        CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        if (entry.name == L"длинный-путь-файл.dat") {
            ++deepFileSeen;
            // Путь элемента тоже длиннее MAX_PATH — обход не «срезал» его.
            CHECK(entry.path.size() > static_cast<std::size_t>(MAX_PATH));
            CHECK_EQ(entry.kind, vfs::EntryKind::File);
            CHECK_EQ(entry.logicalBytes, static_cast<std::uint64_t>(data.size()));
        }
    }
    CHECK_EQ(deepFileSeen, static_cast<std::size_t>(1));

    // 5) Размер файла на длинном пути совпадает с тем, что записали.
    const vfs::FileSize fileSize = vfs::measurePath(deepFile);
    CHECK(fileSize.ok());
    CHECK_EQ(fileSize.logicalBytes, static_cast<std::uint64_t>(data.size()));
    CHECK(fileSize.allocatedKnown);
    CHECK(fileSize.allocatedBytes >= fileSize.logicalBytes);
    CHECK(!vfs::hasFlag(fileSize.flags, vfs::FileFlags::Directory));
    CHECK_EQ(fileSize.reclaimBytes(), fileSize.allocatedBytes);
}

// Длинный путь должен удаляться, и ровно внутри своего корня: удаление файла за
// пределами корня правила — тот отказ, который §10 называет критическим.
TEST(vfsEdge_longPath_delete_inside_root_and_refuses_outside) {
    TempTree tree;
    TempTree foreign;
    if (!tree.ready() || !foreign.ready()) {
        reportSkip("vfsEdge_longPath_delete_inside_root_and_refuses_outside",
                   tree.ready() ? foreign.reason() : tree.reason());
        return;
    }

    std::wstring deep;
    std::string growError;
    const std::size_t levels = growBeyondMaxPath(tree.root(), deep, growError);
    if (levels == 0) {
        reportSkip("vfsEdge_longPath_delete_inside_root_and_refuses_outside", growError);
        return;
    }

    const std::wstring mine = deep + L"\\мой-файл-для-удаления.bin";
    const std::wstring theirs = foreign.path(L"чужой-файл-не-трогать.bin");
    std::uint32_t win32Error = ERROR_SUCCESS;
    const std::string data = payload(9000u, 'D');
    if (!writeFile(mine, data, win32Error) || !writeFile(theirs, data, win32Error)) {
        reportSkip("vfsEdge_longPath_delete_inside_root_and_refuses_outside",
                   describeError("создать фикстуры удаления", win32Error));
        return;
    }

    // Корень правила задаётся один раз на всё дерево операций удаления.
    mrproper::platform::vfs::DeleteOptions options;
    options.allowedRoot = tree.root();

    mrproper::platform::vfs::DeleteRequest outside;
    outside.path = theirs;
    outside.kind = mrproper::platform::vfs::DeleteKind::File;
    mrproper::platform::vfs::DeleteRequest inside;
    inside.path = mine;
    inside.kind = mrproper::platform::vfs::DeleteKind::File;

    // 1) Чужой путь при моём корне — отказ, и файл на месте.
    const mrproper::platform::vfs::DeleteResult outsideResult =
        mrproper::platform::vfs::deleteEntry(outside, options);
    CHECK_EQ(static_cast<int>(outsideResult.status),
             static_cast<int>(mrproper::platform::vfs::DeleteStatus::SkippedOutsideRoot));
    CHECK(exists(theirs));

    // 2) Пустой корень — тоже отказ: операция удаления без корня правила
    //    означала бы «удалить что угодно».
    const mrproper::platform::vfs::DeleteOptions noRoot;
    const mrproper::platform::vfs::DeleteResult noRootResult = mrproper::platform::vfs::deleteEntry(inside, noRoot);
    CHECK_EQ(static_cast<int>(noRootResult.status),
             static_cast<int>(mrproper::platform::vfs::DeleteStatus::SkippedOutsideRoot));
    CHECK(exists(mine));

    // 3) Свой путь при своём корне удаляется, и по длинному пути тоже.
    const mrproper::platform::vfs::DeleteResult mineResult = mrproper::platform::vfs::deleteEntry(inside, options);
    CHECK_EQ(static_cast<int>(mineResult.status), static_cast<int>(mrproper::platform::vfs::DeleteStatus::Deleted));
    CHECK(!exists(mine));
    CHECK(exists(theirs));

    // 4) Повторное удаление — «уже нет», а не «ошибка»: цель достигнута.
    const mrproper::platform::vfs::DeleteResult again = mrproper::platform::vfs::deleteEntry(inside, options);
    CHECK(mrproper::platform::vfs::isRemoved(again.status));

    // 5) Дерево длинных каталогов убирается целиком.
    const mrproper::platform::vfs::TreeDeleteSummary summary = mrproper::platform::vfs::deleteTree(tree.root(), options);
    CHECK(summary.complete());
    CHECK_EQ(summary.dirsDeleted, levels);
    CHECK_EQ(summary.failed, static_cast<std::size_t>(0));
    CHECK(!exists(tree.root()));
    // Чужое дерево не тронуто ни одной операцией.
    CHECK(exists(theirs));
}

// ---------------------------------------------------------------------------
// Юникод
// ---------------------------------------------------------------------------

// Имена, которые обязаны выжить round-trip: кириллица, CJK, суррогатные пары
// (эмодзи — два UTF-16-символа на один кодовый пункт), пробелы, скобки и
// диакритика. Ошибка «имя потеряло последний символ» или «кириллица стала
// вопросами» проявилась бы в отчёте пользователю, поэтому проверяются обе
// стороны: UTF-16 (что вернул обход) и UTF-8 (что положит в модель, §6.3).
TEST(vfsEdge_unicode_names_survive_round_trip) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_unicode_names_survive_round_trip", tree.reason());
        return;
    }

    struct NameCase {
        const wchar_t* wide;
        const char* utf8;
    };
    const NameCase names[] = {
        {L"кириллица-Привет-ЖУРНАЛ.txt", "кириллица-Привет-ЖУРНАЛ.txt"},
        {L"日本語のファイル名.bin", "日本語のファイル名.bin"},
        // Эмодзи: суррогатная пара. Половина пары (например U+D83D) сама по себе
        // не существует, и обрезанная пара ломает и вывод в консоль, и JSON.
        {L"emoji-\xD83D\xDE00-\xD83C\xDF89.dat", "emoji-\xF0\x9F\x98\x80-\xF0\x9F\x8E\x89.dat"},
        {L"пробелы и (скобки) [1] & 'кавычки'.tmp", "пробелы и (скобки) [1] & 'кавычки'.tmp"},
        {L"с-диакритикой-ǅ.txt", "с-диакритикой-ǅ.txt"},
        {L"точка.и-две.точки.txt", "точка.и-две.точки.txt"},
    };
    constexpr std::size_t kNameCount = sizeof(names) / sizeof(names[0]);

    struct Created {
        std::wstring name;
        std::uint64_t bytes;
    };
    std::vector<Created> created;
    std::uint64_t expectedTotal = 0;
    for (std::size_t index = 0; index < kNameCount; ++index) {
        const std::wstring name(names[index].wide);
        const std::string data = payload(2048u + index * 512u, 'U');
        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!writeFile(tree.path(name), data, win32Error)) {
            reportSkip("vfsEdge_unicode_names_survive_round_trip", describeError("создать файл", win32Error));
            return;
        }
        created.push_back(Created{name, static_cast<std::uint64_t>(data.size())});
        expectedTotal += static_cast<std::uint64_t>(data.size());
    }
    CHECK_EQ(created.size(), kNameCount);

    // 1) Обход предъявляет те же UTF-16 имена, что были созданы, и без потерь.
    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    CHECK_EQ(result.stats.files, static_cast<std::uint64_t>(kNameCount));
    CHECK_EQ(result.stats.entries, static_cast<std::uint64_t>(kNameCount));

    const std::vector<vfs::WalkEntry> entries = walkCollect(tree.root());
    for (const Created& item : created) {
        bool found = false;
        for (const vfs::WalkEntry& entry : entries) {
            if (entry.name != item.name) {
                continue;
            }
            found = true;
            CHECK_EQ(entry.kind, vfs::EntryKind::File);
            CHECK_EQ(entry.logicalBytes, item.bytes);
            CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        }
        // Имя, созданное на диске, обязано встретиться обходу в том же виде.
        CHECK(found);
    }

    for (std::size_t index = 0; index < kNameCount; ++index) {
        const std::wstring expectedName(names[index].wide);
        const std::string utf8(names[index].utf8);

        // 2) UTF-8 → UTF-16 → UTF-8 без потерь: так путь попадает в модель
        //    (§6.3) и обратно в операцию.
        const std::wstring wide = mrproper::platform::toUtf16(utf8);
        CHECK(wide == expectedName);
        CHECK(mrproper::platform::toUtf8(wide) == utf8);

        // 3) Юникод не должен приниматься за запрещённую FR-6 форму: ни поток
        //    данных (ADS), ни «.»-компонент. Иначе имя в отчёте вдруг «уехало
        //    бы в поток», а путь с «..» считался бы внутри корня.
        CHECK(!pf::hasStreamSuffix(utf8));
        CHECK(!pf::hasDotSegment(utf8));
        CHECK(pf::isRootedFileSystemPath(mrproper::platform::toUtf8(tree.path(expectedName))));
    }

    // 4) Сумма размеров сходится с числом записанных байт: имена в юникоде
    //    меняют длину пути, но не объём.
    std::uint64_t logicalTotal = 0;
    std::uint64_t allocatedTotal = 0;
    bool allMeasured = true;
    for (const Created& item : created) {
        const vfs::FileSize size = vfs::measurePath(tree.path(item.name));
        allMeasured = allMeasured && size.ok() && size.allocatedKnown;
        CHECK_EQ(size.logicalBytes, item.bytes);
        logicalTotal += size.logicalBytes;
        if (size.allocatedKnown) {
            allocatedTotal += size.allocatedBytes;
        }
    }
    CHECK(allMeasured);
    CHECK_EQ(logicalTotal, expectedTotal);
    CHECK(allocatedTotal >= logicalTotal);

    // 5) Кластер тома известен: без него «занято на диске» остаётся гипотезой.
    const vfs::ClusterSize cluster = vfs::queryClusterSize(tree.root());
    CHECK(cluster.ok());
    CHECK(cluster.bytesPerCluster > 0u);
    for (const Created& item : created) {
        const vfs::AllocatedSizeResult allocated = vfs::queryAllocatedSize(tree.path(item.name));
        CHECK(allocated.ok());
        CHECK(allocated.bytes >= item.bytes);
    }

    // 6) Всё убирается одним проходом deleteTree, юникод не мешает.
    mrproper::platform::vfs::DeleteOptions options;
    options.allowedRoot = tree.root();
    const mrproper::platform::vfs::TreeDeleteSummary summary = mrproper::platform::vfs::deleteTree(tree.root(), options);
    CHECK(summary.complete());
    CHECK_EQ(summary.filesDeleted, kNameCount);
    CHECK(!exists(tree.root()));
}

// ---------------------------------------------------------------------------
// Петля reparse
// ---------------------------------------------------------------------------

// Главное из FR-6: ссылка предъявляется, но обход в неё не входит, и обход
// конечен. Два вида проблем проверяются разными деревьями:
//
//   * «петля» — junction внутри поддерева, указывающий на собственный родитель:
//     раскрытие дало бы бесконечную рекурсию;
//   * «выход наружу» — junction, указывающий на каталог ЗА пределами корня:
//     раскрытие удаляло бы чужое.
TEST(vfsEdge_reparse_junction_is_reported_but_not_followed) {
    TempTree tree;
    TempTree outside;
    if (!tree.ready() || !outside.ready()) {
        reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed",
                   tree.ready() ? outside.reason() : tree.reason());
        return;
    }

    std::uint32_t win32Error = ERROR_SUCCESS;
    if (!makeDirectory(tree.path(L"внешний-каталог"), win32Error) ||
        !makeDirectory(tree.path(L"внешний-каталог/внутрь"), win32Error) ||
        !writeFile(tree.path(L"внешний-каталог/свидетель.txt"), payload(700u, 'W'), win32Error) ||
        !writeFile(tree.path(L"внешний-каталог/внутрь/глубокий-свидетель.txt"), payload(300u, 'v'), win32Error) ||
        !writeFile(outside.path(L"чужое-не-трогать.txt"), payload(400u, 'X'), win32Error)) {
        reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed",
                   describeError("создать фикстуру", win32Error));
        return;
    }

    const std::wstring selfLoop = tree.path(L"внешний-каталог/петля");
    const std::wstring escape = tree.path(L"выход-наружу");
    std::string linkError;
    if (!tree.link(selfLoop, tree.path(L"внешний-каталог"), linkError) ||
        !tree.link(escape, outside.root(), linkError)) {
        reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed", linkError);
        return;
    }

    // --- 1) Обход по умолчанию: ссылки видны, но не раскрыты --------------------
    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK(!result.canceled);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    // ровно две ссылки предъявлены и обе пропущены
    CHECK_EQ(result.stats.reparseSkipped, std::uint64_t{2});
    // петля ловится структурно (ссылка не раскрыта), а не подсчётом повторов
    CHECK_EQ(result.stats.loopsDetected, std::uint64_t{0});
    // 2 файла, 2 обычных каталога и 2 каталога-ссылки
    CHECK_EQ(result.stats.files, std::uint64_t{2});
    CHECK_EQ(result.stats.directories, std::uint64_t{4});
    CHECK_EQ(result.stats.entries, std::uint64_t{6});
    // Глубина ограничена размером настоящего дерева: петля не добавила ни одного
    // уровня. Без этой проверки «прогон завершился» ничего не значило бы —
    // обход мог пройти 30 уровней вглубь и упереться в maxDepth.
    CHECK_EQ(result.stats.maxDepthReached, std::uint32_t{3});

    const std::vector<vfs::WalkEntry> entries = walkCollect(tree.root());
    CHECK_EQ(entries.size(), static_cast<std::size_t>(6));
    std::size_t linksSeen = 0;
    for (const vfs::WalkEntry& entry : entries) {
        // Ни один предъявленный путь не выходит за пределы корня обхода: обход
        // не только не раскрыл ссылки, но и не показал элементов извне.
        CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        if (entry.name.find(L"чужое") != std::wstring::npos) {
            reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed",
                       "обход показал элемент чужого дерева — тест непригоден");
            return;
        }
        if (!entry.reparsePoint) {
            continue;
        }
        ++linksSeen;
        CHECK_EQ(entry.kind, vfs::EntryKind::Directory);
        CHECK_EQ(entry.reparseTag, kIoReparseTagMountPoint);
        // Ссылка предъявлена, но не помечена как петля: её не раскрывали, и
        // защита сработала раньше, чем пришлось бы считать повторы.
        CHECK(!entry.loopDetected);
        CHECK(!entry.depthLimitReached);
        // Ссылка видна и в отчёте: молча пропустить её нельзя, иначе
        // «каталог пуст» неотличим от «каталог скрыт ссылкой».
        CHECK(!vfs::describeReparseTag(entry.reparseTag).empty());
    }
    CHECK_EQ(linksSeen, static_cast<std::size_t>(2));

    // --- 2) Режим диагностики: followReparsePoint = true ------------------------
    // Модуль прямо предупреждает, что в этом режиме от петель защищает только
    // loopDetected. Обязательное свойство — конечность обхода: он обязан
    // завершиться и не уйти за пределы корня. Точное число элементов здесь не
    // проверяется: оно зависит от того, спустится ли обход по ссылке, а это
    // деталь реализации, которую тест не имеет права фиксировать.
    vfs::WalkOptions follow;
    follow.followReparsePoint = true;
    const vfs::WalkResult followed = vfs::walk(tree.root(), follow);
    CHECK(followed.completed);
    CHECK(followed.stats.entries <= std::uint64_t{8});
    for (const vfs::WalkEntry& entry : walkCollect(tree.root())) {
        CHECK(vfs::pathIsInsideRoot(entry.path, followed.root));
    }

    // --- 3) Удаление не раскрывает ссылки ------------------------------------
    mrproper::platform::vfs::DeleteOptions options;
    options.allowedRoot = tree.root();
    const mrproper::platform::vfs::TreeDeleteSummary summary = mrproper::platform::vfs::deleteTree(tree.root(), options);
    // Ссылки по FR-6 пропускаются, поэтому дерево «не пусто» и complete() ложно:
    // это ожидаемое поведение, а не дефект — снимать ссылку должен вызывающий.
    CHECK(!summary.complete());
    CHECK_EQ(summary.filesDeleted, static_cast<std::size_t>(2));
    CHECK(exists(selfLoop));
    CHECK(exists(escape));
    // Ключевая проверка темы: содержимое каталога ЗА ссылкой уцелело. Если бы
    // deleteTree раскрыл ссылку, «чужое-не-трогать.txt» исчез бы.
    CHECK(exists(outside.path(L"чужое-не-трогать.txt")));

    // --- 4) Ссылки сняты — остаётся пустота -----------------------------------
    CHECK(removeJunction(selfLoop, win32Error));
    CHECK(removeJunction(escape, win32Error));
    const mrproper::platform::vfs::TreeDeleteSummary afterLinks =
        mrproper::platform::vfs::deleteTree(tree.root(), options);
    CHECK(afterLinks.complete());
    CHECK(!exists(tree.root()));
    CHECK(exists(outside.path(L"чужое-не-трогать.txt")));
}

// Корень обхода, сам оказавшийся ссылкой, не раскрывается: правило с таким
// корнем увело бы обход за пределы каталога, который пользователь назвал.
TEST(vfsEdge_walk_root_reparse_point_is_not_expanded) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_walk_root_reparse_point_is_not_expanded", tree.reason());
        return;
    }

    std::uint32_t win32Error = ERROR_SUCCESS;
    if (!writeFile(tree.path(L"внутри-корня.txt"), payload(123u, 'R'), win32Error)) {
        reportSkip("vfsEdge_walk_root_reparse_point_is_not_expanded", describeError("создать файл", win32Error));
        return;
    }

    const std::wstring link = tree.path(L"ссылка-на-корень");
    std::string linkError;
    if (!tree.link(link, tree.root(), linkError)) {
        reportSkip("vfsEdge_walk_root_reparse_point_is_not_expanded", linkError);
        return;
    }

    // Нормализация различает ссылку и каталог: иначе «каталог ничего не
    // содержит» и «каталог — ссылка» были бы одним и тем же.
    CHECK(vfs::normalizePath(link).reparsePoint);
    CHECK(!vfs::normalizePath(tree.root()).reparsePoint);
    CHECK(vfs::normalizePath(tree.root()).isDirectory);

    const vfs::WalkResult result = vfs::walk(link);
    CHECK(result.completed);
    CHECK(result.rootSkippedReparse);
    CHECK_EQ(result.stats.entries, std::uint64_t{0});
    CHECK_EQ(result.stats.files, std::uint64_t{0});
    CHECK_EQ(result.stats.errors, std::uint64_t{0});

    // И содержимое исходного каталога на месте: обход по ссылке его не съел.
    CHECK(exists(tree.path(L"внутри-корня.txt")));
}

// Имя 8.3 и длинное имя — один каталог. Проверяется ровно то, что требует
// FR-6: нормализация обязана снять короткое имя, иначе корень правила
// «C:\Program Files\X» не совпал бы с найденным «C:\PROGRA~1\X» и уборка
// молча ничего бы не сделала.
TEST(vfsEdge_short_name_resolves_to_the_same_directory) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_short_name_resolves_to_the_same_directory", tree.reason());
        return;
    }

    const std::wstring longNamed = tree.path(L"каталог-со-столь-длинным-именем-чтобы-получить-короткое");
    std::uint32_t win32Error = ERROR_SUCCESS;
    if (!makeDirectory(longNamed, win32Error)) {
        reportSkip("vfsEdge_short_name_resolves_to_the_same_directory",
                   describeError("создать каталог", win32Error));
        return;
    }

    const std::wstring extendedLong = extended(longNamed);
    std::vector<wchar_t> shortBuffer(extendedLong.size() + 1u, L'\0');
    const DWORD shortLength = ::GetShortPathNameW(extendedLong.c_str(), shortBuffer.data(),
                                                  static_cast<DWORD>(shortBuffer.size()));
    const std::wstring shortName(shortBuffer.data());
    // На томе с отключённым 8.3 (частая настройка рабочих машин) короткого имени
    // не будет — это не дефект теста и не дефект модуля.
    if (shortLength == 0u || shortName == longNamed || shortName.find(L'~') == std::wstring::npos) {
        reportSkip("vfsEdge_short_name_resolves_to_the_same_directory",
                   "том не выдаёт короткие имена 8.3 (GetShortPathNameW вернул длину " +
                       std::to_string(shortLength) + ")");
        return;
    }

    // Порядок «сначала нормализация, потом сравнение» задан контрактом
    // vfs_paths: сырой короткий путь против длинного корня даёт ложный запрет.
    const pf::Resolution fromLong = pf::resolve(mrproper::platform::toUtf8(longNamed));
    const pf::Resolution fromShort = pf::resolve(mrproper::platform::toUtf8(shortName));
    CHECK(fromLong.ok());
    CHECK(fromShort.ok());
    // strong == true — доказательство, что имя нормализовано (а не получено «как
    // открыли»): без него вердикт о принадлежности корню выносить нельзя.
    CHECK(fromLong.strong);
    CHECK(fromShort.strong);
    CHECK(fromLong.directory);
    CHECK(fromShort.directory);
    CHECK(fromLong.path == fromShort.path);
    CHECK(fromLong.path.find('~') == std::string::npos);
    CHECK(pf::isInsideRoot(fromShort.path, fromLong.path));
    // И обратное направление: корень-короткий, путь-длинный.
    CHECK(pf::isInsideRoot(fromLong.path, fromShort.path));

    // Подготовленный корень правила даёт тот же ответ, что и сравнение строк.
    const pf::RootGuard root = pf::prepareRoot(mrproper::platform::toUtf8(shortName));
    CHECK(root.resolved);
    CHECK(!root.rejected);
    CHECK(root.root == fromLong.path);
}

// Длинный путь с юникодом и ссылкой в одном дереве: три темы задачи
// одновременно, потому что в жизни они и встречаются вместе (кириллические
// профили, вложенные кэши, OneDrive-подобные каталоги).
TEST(vfsEdge_long_unicode_tree_with_reparse_backlink) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", tree.reason());
        return;
    }

    std::wstring deep;
    std::string growError;
    const std::size_t levels = growBeyondMaxPath(tree.root(), deep, growError);
    if (levels == 0) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", growError);
        return;
    }
    CHECK(deep.size() > static_cast<std::size_t>(MAX_PATH));

    const std::wstring leaf = deep + L"\\крайний-файл-😀.dat";
    std::uint32_t win32Error = ERROR_SUCCESS;
    const std::string data = payload(1500u, 'E');
    if (!writeFile(leaf, data, win32Error)) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", describeError("создать файл", win32Error));
        return;
    }

    // Ссылка с самого дна на корень обхода: раскрытие прогнало бы всё дерево
    // ещё раз, а на длинных путях это ещё и заметная потеря времени.
    std::string linkError;
    if (!tree.link(deep + L"\\назад-на-корень", tree.root(), linkError)) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", linkError);
        return;
    }

    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    CHECK_EQ(result.stats.files, std::uint64_t{1});
    CHECK_EQ(result.stats.reparseSkipped, std::uint64_t{1});
    CHECK_EQ(result.stats.loopsDetected, std::uint64_t{0});
    // Настоящие каталоги, одна ссылка и файл на дне
    CHECK_EQ(result.stats.entries, static_cast<std::uint64_t>(levels) + 2u);
    // Глубина настоящего дерева, а не «уровни × 2»: петлю не раскрывали.
    CHECK(result.stats.maxDepthReached <= static_cast<std::uint32_t>(levels) + 1u);

    std::size_t leafSeen = 0;
    for (const vfs::WalkEntry& entry : walkCollect(tree.root())) {
        CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        if (entry.name == L"крайний-файл-😀.dat") {
            ++leafSeen;
            CHECK_EQ(entry.logicalBytes, static_cast<std::uint64_t>(data.size()));
            CHECK(entry.path.size() > static_cast<std::size_t>(MAX_PATH));
        }
    }
    CHECK_EQ(leafSeen, static_cast<std::size_t>(1));

    // Размер длинного юникодного файла меряется теми же функциями, что ASCII.
    const vfs::FileSize size = vfs::measurePath(leaf);
    CHECK(size.ok());
    CHECK_EQ(size.logicalBytes, static_cast<std::uint64_t>(data.size()));
    CHECK(size.allocatedKnown);
    CHECK_EQ(size.reclaimBytes(), size.allocatedBytes);
}

// ==== ВРЕМЕННЫЙ ДИАГНОСТИЧЕСКИЙ ТЕСТ (будет удалён) ====
TEST(zz_diagnostics) {
    TempTree tree;
    std::printf("ACP=%u OEMCP=%u\n", ::GetACP(), ::GetOEMCP());
    std::wstring deep;
    std::string growError;
    const std::size_t levels = growBeyondMaxPath(tree.root(), deep, growError);
    std::printf("levels=%zu len=%zu\n", levels, deep.size());
    const std::string utf8 = mrproper::platform::toUtf8(deep);
    std::printf("utf8 len=%zu [%s]\n", utf8.size(), utf8.c_str());
    const std::string ext8 = pf::toExtendedPath(utf8);
    std::printf("toExtendedPath len=%zu [%s]\n", ext8.size(), ext8.c_str());
    std::printf("isRootedFileSystemPath=%d isReparsePoint-ok\n", (int)pf::isRootedFileSystemPath(utf8));
    const pf::Resolution res = pf::resolve(utf8);
    std::printf("resolve: ok=%d strong=%d err=%u [%s]\n", (int)res.ok(), (int)res.strong, res.lastError,
                res.path.c_str());
    const pf::Resolution rootRes = pf::resolve(mrproper::platform::toUtf8(tree.root()));
    std::printf("resolve(root): ok=%d strong=%d err=%u [%s]\n", (int)rootRes.ok(), (int)rootRes.strong,
                rootRes.lastError, rootRes.path.c_str());

    const std::wstring file = tree.path(L"проба.txt");
    std::uint32_t we = 0;
    std::printf("writeFile=%d\n", (int)writeFile(file, payload(100u, 'p'), we));
    const vfs::WalkResult wr = vfs::walk(tree.root());
    std::printf("walk: completed=%d entries=%llu errors=%llu root=[%s]\n", (int)wr.completed,
                (unsigned long long)wr.stats.entries, (unsigned long long)wr.stats.errors,
                mrproper::platform::toUtf8(wr.root).c_str());
    for (const auto& e : wr.errors) {
        std::printf("  err: %u [%s] [%s]\n", e.win32Code, mrproper::platform::toUtf8(e.message).c_str(),
                    mrproper::platform::toUtf8(e.path).c_str());
    }

    mrproper::platform::vfs::DeleteOptions opts;
    opts.allowedRoot = tree.root();
    mrproper::platform::vfs::DeleteRequest req;
    req.path = file;
    req.kind = mrproper::platform::vfs::DeleteKind::File;
    const auto dr = mrproper::platform::vfs::deleteEntry(req, opts);
    std::printf("deleteEntry status=%s hr=%d detail=[%s]\n", mrproper::platform::vfs::toString(dr.status),
                (int)dr.hr, mrproper::platform::toUtf8(dr.detail).c_str());
    const auto ts = mrproper::platform::vfs::deleteTree(tree.root(), opts);
    std::printf("deleteTree files=%zu dirs=%zu failed=%zu problems=%zu\n", ts.filesDeleted, ts.dirsDeleted,
                ts.failed, ts.problems.size());
    for (const auto& p : ts.problems) {
        std::printf("  prob: %s hr=%d [%s]\n", mrproper::platform::vfs::toString(p.status), (int)p.hr,
                    mrproper::platform::toUtf8(p.path).c_str());
    }
    std::printf("exists(root)=%d\n", (int)exists(tree.root()));

    // Прямой вызов Win32: где именно ломается перечисление.
    {
        ScopedHandle h(::CreateFileW(extended(tree.root()).c_str(), FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr));
        std::printf("enum handle valid=%d\n", (int)(bool)h);
        std::vector<char> buf(64u * 1024u, 0);
        const BOOL okDir = ::GetFileInformationByHandleEx(h.get(), FileFullDirectoryRestartInfo, buf.data(),
                                                          (DWORD)buf.size());
        // У API четыре аргумента: отдельного «сколько записано» нет. Признак того,
        // что запись получена, — успех вызова плюс последующая проверка буфера.
        const DWORD ret = okDir ? 1u : 0u;
        std::printf("GetFileInformationByHandleEx ok=%d err=%u ret=%u\n", (int)okDir, ::GetLastError(), ret);
        if (okDir && ret > 0) {
            const auto* raw = reinterpret_cast<const FILE_FULL_DIR_INFO*>(buf.data());
            std::printf("  first entry: nameLen=%u name=[%ls] next=%u sizeof=%u\n", raw->FileNameLength,
                        (const wchar_t*)(buf.data() + offsetof(FILE_FULL_DIR_INFO, FileName)),
                        (unsigned)raw->NextEntryOffset, (unsigned)sizeof(FILE_FULL_DIR_INFO));
        }
    }
    // Короткое имя 8.3 — отдельная проверка, resolve() падает на toExtendedPath.
    {
        const std::wstring longNamed = tree.path(L"каталог-со-столь-длинным-именем-чтобы-получить-короткое");
        std::uint32_t weLong = 0;  // не we: внешний we уже объявлен, а /WX превращает C4456 в ошибку
        std::printf("mkdir(long)=%d\n", (int)makeDirectory(longNamed, weLong));
        std::vector<wchar_t> sb(extended(longNamed).size() + 2u, 0);
        const DWORD n = ::GetShortPathNameW(extended(longNamed).c_str(), sb.data(), (DWORD)sb.size());
        std::printf("GetShortPathNameW n=%u err=%u [%ls]\n", n, ::GetLastError(), n ? sb.data() : L"");
    }
}
