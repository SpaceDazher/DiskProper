// Реализация обхода дерева каталогов. Единственный файл модуля, где
// встречается windows.h: наружу (vfs_walk.hpp) уходят только переносимые
// типы, чтобы потребители не тащили Win32 в свои заголовки (SPEC §6.1).
//
// Почему перечисление идёт через дескриптор, а не через FindFirstFileW:
//
//   1) GetFileInformationByHandleEx(FileFullDirectoryInfo) перечисляет каталог по
//      уже открытому дескриптору и не требует собирать путь «родитель + имя»
//      заново для Win32 — длинные пути (> MAX_PATH) не требуют ничего
//      особенного, префикс \\?\ добавлен один раз, при открытии;
//   2) тот же дескриптор отдаёт идентификацию каталога
//      (GetFileInformationByHandle: dwVolumeSerialNumber + nFileIndex), то есть
//      защита от петель не стоит дополнительного открытия на каталог;
//   3) в ответе сразу лежат FILE_ATTRIBUTE_REPARSE_POINT, размер и времена —
//      ровно то, что нужно посетителю, без повторного открытия каждого файла;
//   4) «.», «..» в ответе не приходят вовсе, а GetFileInformationByHandleEx
//      не переходит по ссылкам сам — обе защиты от петель из SPEC §4 FR-6
//      получаются бесплатно.
//
// Чего этот обход принципиально не делает: не следует по ссылкам, не
// разворачивает «..», не выходит за пределы переданного корня. Последнее
// обеспечивается конструкцией: элементы собираются от нормализованного корня
// добавлением имён, поэтому «путь внутри корня» верно тождественно, а не по
// результату сравнения строк.
//
// Чего обход не ловит и ловить не должен: файл, исчезнувший после
// перечисления, и каталог, переименованный в момент спуска. Первый просто не
// попадёт в результат (размеры берутся из записи каталога, а не из открытого
// файла — см. комментарий к WalkEntry::logicalBytes), второй приводит к
// отказу раскрывать поддерево с записью в лог. Оба случая — гонка с другим
// процессом, а не ошибка обхода, и «лечить» их повторными попытками значило
// бы зациклиться.

#include "vfs_walk.hpp"

#include <windows.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::vfs {
namespace {

using ScopedHandle = unique_handle<KernelHandlePolicy>;

// Буфер перечисления каталога: 64 КиБ. Запись FILE_FULL_DIR_INFO — это 104
// байта заголовка плюс имя, а NTFS, ReFS и FAT ограничивают имя 255 символами,
// то есть запись меньше килобайта и один вызов возвращает десятки записей.
// Теоретическое «имя не поместилось» (ERROR_MORE_DATA) на поддерживаемых ФС
// невозможно, но обработка всё равно есть: пишем ошибку и выходим из каталога,
// а не крутим перечисление заново.
constexpr std::size_t kEnumBufferBytes = 64u * 1024u;

// Буфер для FSCTL_GET_REPARSE_POINT: ровно размер, который требует документация
// (MAXIMUM_REPARSE_DATA_BUFFER_SIZE). Нужен только для reparse point, поэтому
// выделяется лениво.
constexpr std::size_t kReparseBufferBytes = MAXIMUM_REPARSE_DATA_BUFFER_SIZE;

// Права на дескриптор перечисления: каталог нужно только прочитать, поэтому
// дескриптор делимый по всем правам (иначе не залезть в занятый каталог) и
// запрошен без GENERIC_READ: FILE_LIST_DIRECTORY достаточно для
// GetFileInformationByHandleEx и не даёт лишних требований к ACL.
constexpr DWORD kListAccess = FILE_LIST_DIRECTORY;
constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// FILE_FLAG_OPEN_REPARSE_POINT — ключевой флаг модуля: дескриптор описывает
// сам каталог, а не цель ссылки, поэтому перечисление не уходит за пределы
// корня, даже если каталог успели превратить в junction.
constexpr DWORD kDirectoryFlags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;

// FILE_ATTRIBUTE_PIPE (0x100) в winnt.h нет — константа объявлена только в
// ntifs.h, а подключать драйверные заголовки ради одного значения нельзя.
// Значение взято из MSDN и служит единственной цели: отличить «файл» от
// «иное» (устройство, канал) в элементе каталога.
constexpr DWORD kFileAttributePipe = 0x00000100u;

// FILETIME → целое число 100-нс интервалов с 1601-01-01 UTC. В заголовке тип
// FILETIME намеренно не светится наружу (SPEC §6.1), поэтому здесь
// преобразование в то, что наружу уходит.
[[nodiscard]] std::uint64_t toUInt64(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | static_cast<std::uint64_t>(time.dwLowDateTime);
}

// Тот же формат приходит из записи каталога, где время — LARGE_INTEGER со
// знаком (обе структуры совпадают по разметке, но подставлять одну в другую
// нельзя).
[[nodiscard]] std::uint64_t toUInt64(const LARGE_INTEGER& time) noexcept {
    return static_cast<std::uint64_t>(time.QuadPart);
}

// 64-битный индекс файла из BY_HANDLE_FILE_INFORMATION.
[[nodiscard]] std::uint64_t toFileIndex(const BY_HANDLE_FILE_INFORMATION& info) noexcept {
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | static_cast<std::uint64_t>(info.nFileIndexLow);
}

// Привести путь к форме, которую понимают W-функции Win32 при любой длине:
// префикс \\?\ отключает разбор 8.3 и нормализацию пути, поэтому «C:\PROGRA~1\…»
// и «C:\Program Files\…» больше не смешиваются (SPEC §4 FR-6).
//
// UNC-путь требует другой формы: «\\server\share» превращается в
// «\\?\UNC\server\share», потому что «\\?\» перед именем сервера система не
// понимает. Префиксы \\?\ и \\.\ идут как есть — нормализовывать «сырой» путь
// устройства нельзя.
//
// Не-ASCII не трогаем вообще: это UTF-16 от начала до конца, кириллица и
// иероглифы проходят насквозь без конвертации (§12, «тесты с юникодом»).
[[nodiscard]] std::wstring toExtendedPath(std::wstring_view path) {
    std::wstring result;
    result.reserve(path.size() + 8);
    for (const wchar_t symbol : path) {
        result.push_back(symbol == L'/' ? L'\\' : symbol);
    }
    constexpr std::wstring_view kExtended = L"\\\\?\\";
    constexpr std::wstring_view kDevice = L"\\\\.\\";
    constexpr std::wstring_view kUnc = L"\\\\";
    if (result.size() >= kExtended.size() && result.compare(0, kExtended.size(), kExtended) == 0) {
        return result;
    }
    if (result.size() >= kDevice.size() && result.compare(0, kDevice.size(), kDevice) == 0) {
        return result;
    }
    if (result.size() >= kUnc.size() && result.compare(0, kUnc.size(), kUnc) == 0) {
        // UNC: «\\server\share\…» → «\\?\UNC\server\share\…»
        std::wstring unc(kExtended);
        unc.append(L"UNC");
        unc.append(result, kUnc.size(), std::wstring::npos);
        return unc;
    }
    // Остальное считаем полностью определённым путём: относительный путь с
    // префиксом \\?\ система отвергнет (ERROR_INVALID_NAME), и это честнее, чем
    // молчаливый обход относительного пути от текущего каталога процесса.
    std::wstring extended(kExtended);
    extended.append(result);
    return extended;
}

// Присоединить имя к пути каталога. Разделитель не дублируется: корень тома
// («C:\») и нормализованный путь уже заканчиваются разделителем.
[[nodiscard]] std::wstring joinPath(const std::wstring& parent, const std::wstring& name) {
    std::wstring result = parent;
    if (result.empty() || (result.back() != L'\\' && result.back() != L'/')) {
        result.push_back(L'\\');
    }
    result.append(name);
    return result;
}

// ASCII-регистр: NTFS по умолчанию нечувствителен к регистру ASCII, и этого
// достаточно для проверки принадлежности пути корню. Не-ASCII сравнивается
// точно: лишний «отказ» (путь не признан внутри корня) безопасен — движок
// откажется работать с путём, а не удалит лишнее; ложное «внутри» здесь
// невозможно в принципе.
[[nodiscard]] constexpr wchar_t toUpperAscii(wchar_t symbol) noexcept {
    return (symbol >= L'a' && symbol <= L'z') ? static_cast<wchar_t>(symbol - L'a' + L'A') : symbol;
}

[[nodiscard]] constexpr bool isPathSeparator(wchar_t symbol) noexcept {
    return symbol == L'\\' || symbol == L'/';
}

// Идентификация каталога: серийный номер тома и индекс файла.
//
// Индекс берётся ровно в том виде, в каком он гарантированно совпадает в двух
// источниках: FILE_FULL_DIR_INFO::FileIndex — это ULONG (32 бита), а
// GetFileInformationByHandle отдаёт 64-битный nFileIndexHigh/nFileIndexLow.
// Верхние 32 бита на NTFS нулевые (идентификатор — это MFT-ссылка с
// 16-битным номером последовательности в старших битах), но полагаться на
// это нельзя: при полном томе сравнение 32 и 64 бит разошлось бы и обход
// отказался бы раскрывать каталоги. Поэтому сравниваем и храним младшие
// 32 бита — это и есть то, что обе стороны отдают.
struct DirectoryId {
    std::uint64_t volumeSerial{};
    std::uint32_t fileIndex{};

    [[nodiscard]] friend bool operator==(const DirectoryId& left, const DirectoryId& right) noexcept {
        return left.volumeSerial == right.volumeSerial && left.fileIndex == right.fileIndex;
    }
};

struct DirectoryIdHash {
    [[nodiscard]] std::size_t operator()(const DirectoryId& id) const noexcept {
        // Смешивание Фибоначчи: два числа в один ключ, иначе хеш-таблица
        // вырождается в линейный поиск при равных volumeSerial.
        std::uint64_t mixed = id.volumeSerial * 0x9E3779B97F4A7C15ull + id.fileIndex;
        mixed ^= mixed >> 33;
        mixed *= 0xFF51AFD7ED558CCDull;
        mixed ^= mixed >> 33;
        return static_cast<std::size_t>(mixed);
    }
};

// Копия записи каталога. Запись лежит в общем буфере, который перезаписывает
// вложенный обход, поэтому из буфера копируется всё, что нужно, ДО рекурсии —
// иначе после возврата из вложенного обхода указатель на запись указывал бы на
// чужие данные.
struct Record {
    std::wstring name;
    std::uint32_t attributes{};
    std::uint32_t fileIndex{};  // см. комментарий к DirectoryId: младшие 32 бита
    std::uint64_t logicalBytes{};
    std::uint64_t creationTime{};
    std::uint64_t lastAccessTime{};
    std::uint64_t lastWriteTime{};
};

// Период проверки отмены с учётом «0 = значение по умолчанию»: вызывающий не
// должен обязан знать, что kCancelCheckPeriod ненулевой.
[[nodiscard]] std::size_t cancelCheckPeriod(const WalkOptions& options) noexcept {
    return options.cancelCheckPeriod != 0 ? options.cancelCheckPeriod : kCancelCheckPeriod;
}

// Обход. Состояние целиком внутри объекта: рекурсия по каталогам не должна
// тащить состояние через параметры, а указатель на объект переживает всё.
class Walker {
public:
    Walker(const WalkOptions& options, const WalkVisitor& visitor, std::stop_token stop) noexcept
        : options_(options), visitor_(visitor), stop_(stop) {
    }

    [[nodiscard]] WalkResult run(std::wstring_view root);

private:
    // false — обход прекращён (отмена или Stop посетителя), иначе true, даже
    // если поддерево пропущено из-за ошибки.
    [[nodiscard]] bool walkDirectory(const std::wstring& dirPath, std::uint32_t depth, std::uint64_t volumeSerial,
                                     std::uint32_t expectedIndex);
    // false — прекратить весь обход.
    [[nodiscard]] bool visitChild(const Record& record, const std::wstring& dirPath, std::uint32_t depth,
                                  std::uint64_t volumeSerial);
    [[nodiscard]] bool cancelled();
    [[nodiscard]] std::uint32_t readReparseTag(const std::wstring& path);
    void noteError(std::wstring_view path, DWORD code);
    void noteError(std::wstring_view path, DWORD code, std::wstring message);
    void finish(const std::chrono::steady_clock::time_point& started);

    const WalkOptions& options_;
    const WalkVisitor& visitor_;
    std::stop_token stop_;

    WalkResult result_;
    std::unordered_set<DirectoryId, DirectoryIdHash> visited_;
    // Буфер перечисления: один на весь обход, потому что вложенные вызовы
    // перезаписывают его содержимое.
    std::vector<std::byte> enumBuffer_;
    std::vector<std::byte> reparseBuffer_;
    std::uint64_t sinceCancelCheck_{};
};

[[nodiscard]] bool Walker::cancelled() {
    sinceCancelCheck_ = 0;
    if (!stop_.stop_requested()) {
        return false;
    }
    result_.canceled = true;
    return true;
}

void Walker::noteError(std::wstring_view path, DWORD code) {
    noteError(path, code, formatSystemMessage(code));
}

void Walker::noteError(std::wstring_view path, DWORD code, std::wstring message) {
    result_.stats.errors += 1;
    // Список ошибок ограничен: дерево с тысячами недоступных каталогов не
    // должно превращать отчёт в источник нехватки памяти. Счётчик stats.errors
    // при этом остаётся полным (см. пункт 6 в шапке vfs_walk.hpp).
    if (result_.errors.size() < kMaxStoredErrors) {
        WalkError error;
        error.path.assign(path);
        error.win32Code = static_cast<std::uint32_t>(code);
        error.message = std::move(message);
        result_.errors.push_back(std::move(error));
    }
    mrproper::core::LogFields fields;
    fields.push_back(mrproper::core::logField("path", core::toUtf8(path)));
    fields.push_back(mrproper::core::logField("win32", static_cast<std::uint32_t>(code)));
    mrproper::core::logWarn("vfs.walk.error", "обход: элемент пропущен", fields);
}

[[nodiscard]] std::uint32_t Walker::readReparseTag(const std::wstring& path) {
    // Права 0: FSCTL_GET_REPARSE_POINT документирован именно так, а
    // FILE_FLAG_OPEN_REPARSE_POINT обязателен — иначе тег пришёл бы от цели
    // ссылки, а не от самой ссылки.
    const std::wstring extended = toExtendedPath(path);
    const ScopedHandle link(::CreateFileW(extended.c_str(), 0, kShareAll, nullptr, OPEN_EXISTING, kDirectoryFlags, nullptr));
    if (!link) {
        return 0;
    }
    if (reparseBuffer_.size() < kReparseBufferBytes) {
        reparseBuffer_.resize(kReparseBufferBytes);
    }
    DWORD returned = 0;
    if (!::DeviceIoControl(link.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, reparseBuffer_.data(),
                           static_cast<DWORD>(reparseBuffer_.size()), &returned, nullptr)) {
        // Тег — украшение для отчёта; отказ читать его не должен ломать обход.
        return 0;
    }
    if (returned < sizeof(std::uint32_t)) {
        return 0;
    }
    std::uint32_t tag = 0;
    std::memcpy(&tag, reparseBuffer_.data(), sizeof(tag));
    return tag;
}

[[nodiscard]] bool Walker::visitChild(const Record& record, const std::wstring& dirPath, std::uint32_t depth,
                                     std::uint64_t volumeSerial) {
    // Отмена проверяется на границе периода, а не на каждом элементе: stop_token
    // — это чтение, а дерево в 500 тысяч файлов иначе платит за отмену по
    // одному SystemFunction036 на элемент (§6.4: «проверка каждые 256
    // элементов»).
    if (++sinceCancelCheck_ >= cancelCheckPeriod(options_)) {
        if (cancelled()) {
            return false;
        }
    }

    WalkEntry entry;
    entry.name = record.name;
    entry.path = joinPath(dirPath, record.name);
    entry.depth = depth + 1;
    entry.fileAttributes = record.attributes;
    entry.creationTime = record.creationTime;
    entry.lastAccessTime = record.lastAccessTime;
    entry.lastWriteTime = record.lastWriteTime;
    entry.logicalBytes = record.logicalBytes;

    if ((record.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        entry.kind = EntryKind::Directory;
    } else if ((record.attributes & (FILE_ATTRIBUTE_DEVICE | kFileAttributePipe)) != 0) {
        entry.kind = EntryKind::Other;
    } else {
        entry.kind = EntryKind::File;
    }

    entry.reparsePoint = (record.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (entry.reparsePoint) {
        entry.reparseTag = readReparseTag(entry.path);
    }

    // --- счётчики ----------------------------------------------------------
    result_.stats.entries += 1;
    switch (entry.kind) {
        case EntryKind::File:
            result_.stats.files += 1;
            break;
        case EntryKind::Directory:
            result_.stats.directories += 1;
            break;
        case EntryKind::Other:
            result_.stats.otherEntries += 1;
            break;
    }
    if (entry.depth > result_.stats.maxDepthReached) {
        result_.stats.maxDepthReached = entry.depth;
    }

    // --- решения о спуске --------------------------------------------------
    // Порядок важен: ссылка не раскрывается даже на малой глубине, глубина не
    // отменяет запрет на ссылку, а петля не отменяет ничего из перечисленного.
    bool descend = entry.kind == EntryKind::Directory;
    if (entry.reparsePoint && !options_.followReparsePoint) {
        result_.stats.reparseSkipped += 1;
        descend = false;
    }
    if (descend && entry.depth >= options_.maxDepth) {
        result_.stats.depthLimitHits += 1;
        entry.depthLimitReached = true;
        descend = false;
    }
    if (descend) {
        // Каталог мог встретиться второй раз (смонтированный том, теневой
        // каталог). Идентичность проверяется по данным записи каталога и
        // серийному номеру тома родителя: репоинты мы не раскрываем, поэтому
        // все дети одного каталога лежат на томе родителя.
        const DirectoryId id{volumeSerial, record.fileIndex};
        if (visited_.find(id) != visited_.end()) {
            result_.stats.loopsDetected += 1;
            entry.loopDetected = true;
            descend = false;
        }
    }

    // --- посетитель --------------------------------------------------------
    if (visitor_) {
        const WalkStep step = visitor_(entry);
        if (step == WalkStep::Stop) {
            result_.stoppedByVisitor = true;
            return false;
        }
        if (step == WalkStep::SkipDirectory) {
            descend = false;
        }
    }

    if (descend && !walkDirectory(entry.path, entry.depth, volumeSerial, record.fileIndex)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool Walker::walkDirectory(const std::wstring& dirPath, std::uint32_t depth, std::uint64_t volumeSerial,
                                         std::uint32_t expectedIndex) {
    const std::wstring extended = toExtendedPath(dirPath);
    const ScopedHandle dir(::CreateFileW(extended.c_str(), kListAccess, kShareAll, nullptr, OPEN_EXISTING,
                                         kDirectoryFlags, nullptr));
    if (!dir) {
        const DWORD code = ::GetLastError();
        // Корень, который не открылся, — это результат обхода, а не «пустое
        // дерево»: иначе сканер сообщил бы «в каталоге ничего нет».
        noteError(dirPath, code);
        return true;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(dir.get(), &info)) {
        noteError(dirPath, ::GetLastError());
        return true;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        // Файл занял место каталога между перечислением и открытием.
        noteError(dirPath, ERROR_DIRECTORY);
        return true;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && depth > 0 && !options_.followReparsePoint) {
        // Каталог стал ссылкой уже после того, как родитель его перечислил. Счётчик
        // reparseSkipped на этом элеменке уже стоит в родителе, поэтому здесь
        // только запись в лог: поддерево пропускаем молча, но невидимо.
        mrproper::core::LogFields fields;
        fields.push_back(mrproper::core::logField("path", core::toUtf8(dirPath)));
        mrproper::core::logWarn("vfs.walk.changed", "каталог стал reparse point до раскрытия — поддерево пропущено", fields);
        return true;
    }

    const std::uint32_t shortIndex = static_cast<std::uint32_t>(toFileIndex(info));
    if (depth > 0 && (info.dwVolumeSerialNumber != volumeSerial || shortIndex != expectedIndex)) {
        // Тот каталог, который мы перечислили, уже не тот: переименование,
        // монтирование тома, гонка с другим процессом. Идти по такому каталогу
        // нельзя — это ровно тот «выход за пределы корня правила», который
        // SPEC §4 FR-6 запрещает. Событие в лог, поддерево пропускаем.
        mrproper::core::LogFields fields;
        fields.push_back(mrproper::core::logField("path", core::toUtf8(dirPath)));
        fields.push_back(mrproper::core::logField("expectedIndex", expectedIndex));
        fields.push_back(mrproper::core::logField("actualIndex", shortIndex));
        mrproper::core::logWarn("vfs.walk.changed", "каталог изменился до раскрытия — поддерево пропущено", fields);
        return true;
    }

    // Повторный визит в тот же каталог невозможен: родитель проверял
    // идентичность до спуска, а корень посещается один раз. insert() здесь
    // регистрирует каталог и заодно страхует от повторов, если проверка
    // родителя была ослаблена (followReparsePoint, гонка).
    if (!visited_.insert(DirectoryId{info.dwVolumeSerialNumber, shortIndex}).second) {
        result_.stats.loopsDetected += 1;
        return true;
    }

    if (enumBuffer_.size() < kEnumBufferBytes) {
        enumBuffer_.resize(kEnumBufferBytes);
    }

    bool restart = true;
    for (;;) {
        DWORD returned = 0;
        const BOOL listed = ::GetFileInformationByHandleEx(
            dir.get(), restart ? FileFullDirectoryRestartInfo : FileFullDirectoryInfo, enumBuffer_.data(),
            static_cast<DWORD>(enumBuffer_.size()));
        restart = false;
        if (!listed) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_NO_MORE_FILES) {
                return true;  // каталог закончился — обычное завершение
            }
            noteError(dirPath, code);
            return true;
        }

        std::size_t offset = 0;
        for (;;) {
            if (offset + sizeof(FILE_FULL_DIR_INFO) > returned) {
                noteError(dirPath, ERROR_BAD_LENGTH);
                return true;
            }
            const auto* raw = reinterpret_cast<const FILE_FULL_DIR_INFO*>(enumBuffer_.data() + offset);

            // Копия записи: после рекурсивного спуска буфер перечисления уже
            // перезаписан, и raw больше нельзя ни читать, ни использовать.
            Record record;
            record.attributes = raw->FileAttributes;
            record.fileIndex = raw->FileIndex;
            record.logicalBytes = static_cast<std::uint64_t>(raw->EndOfFile.QuadPart);
            record.creationTime = toUInt64(raw->CreationTime);
            record.lastAccessTime = toUInt64(raw->LastAccessTime);
            record.lastWriteTime = toUInt64(raw->LastWriteTime);
            const std::size_t nameOffset = offsetof(FILE_FULL_DIR_INFO, FileName);
            if (raw->FileNameLength > returned - offset - nameOffset) {
                noteError(dirPath, ERROR_BAD_LENGTH);
                return true;
            }
            record.name.assign(raw->FileName, static_cast<std::size_t>(raw->FileNameLength) / sizeof(wchar_t));
            const std::size_t nextOffset = raw->NextEntryOffset;

            if (!visitChild(record, dirPath, depth, info.dwVolumeSerialNumber)) {
                return false;
            }

            if (nextOffset == 0) {
                break;
            }
            const std::size_t advanced = offset + nextOffset;
            if (advanced <= offset || advanced >= returned) {
                // Смещение не сходится: буфер повреждён или вернулось не то, что
                // мы запросили. Дальше идти опасно — прекращаем каталог.
                noteError(dirPath, ERROR_BAD_LENGTH);
                return true;
            }
            offset = advanced;
        }
    }
}

void Walker::finish(const std::chrono::steady_clock::time_point& started) {
    result_.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    result_.completed = !result_.canceled && !result_.stoppedByVisitor;

    mrproper::core::LogFields fields;
    fields.push_back(mrproper::core::logField("root", core::toUtf8(result_.root)));
    fields.push_back(mrproper::core::logField("entries", result_.stats.entries));
    fields.push_back(mrproper::core::logField("files", result_.stats.files));
    fields.push_back(mrproper::core::logField("dirs", result_.stats.directories));
    fields.push_back(mrproper::core::logField("reparseSkipped", result_.stats.reparseSkipped));
    fields.push_back(mrproper::core::logField("loops", result_.stats.loopsDetected));
    fields.push_back(mrproper::core::logField("errors", result_.stats.errors));
    fields.push_back(mrproper::core::logField("completed", result_.completed));
    fields.push_back(mrproper::core::logField("elapsedMs", static_cast<std::uint64_t>(result_.elapsed.count())));
    mrproper::core::logTrace("vfs.walk.done", "обход дерева завершён", fields);
}

[[nodiscard]] WalkResult Walker::run(std::wstring_view root) {
    const auto started = std::chrono::steady_clock::now();

    // Нормализация корня до обхода (SPEC §4 FR-6: GetFinalPathNameByHandleW
    // снимает 8.3 и разрешает ссылки). Если каталог недоступен, результат
    // всё равно полезен: root вернётся в форме \\?\ и с кодом отказа в errors.
    // Этот кусок вне try: если память кончится именно здесь, ловить нечего —
    // возвращать нечем (см. оговорку про std::bad_alloc в vfs_walk.hpp).
    const NormalizedPath normalized = normalizePath(root);
    result_.root = normalized.path;
    if (!normalized.resolved) {
        // Нет доступа даже к атрибутам: обхода не будет. Результат всё равно
        // возвращаем — с root в форме \\?\ и кодом отказа в errors.
        noteError(root, static_cast<DWORD>(normalized.win32Code));
        finish(started);
        return std::move(result_);
    }
    if (!normalized.isDirectory) {
        // Файл передан как корень. Сообщаем именно это, а не ACCESS_DENIED от
        // неудачного открытия с правами на перечисление.
        noteError(root, ERROR_DIRECTORY);
        finish(started);
        return std::move(result_);
    }
    if (normalized.reparsePoint && !options_.followReparsePoint) {
        // Корень обхода сам оказался ссылкой. Раскрывать его нельзя: путь,
        // который пришёл от правила, указывает на ссылку, а обход ушёл бы туда,
        // куда она ведёт, — это ровно тот «выход за пределы корня правила»,
        // который SPEC §4 FR-6 запрещает. Результат честный и помеченный:
        // элементов ноль, rootSkippedReparse = true, в лог — предупреждение.
        result_.rootSkippedReparse = true;
        const std::wstring tag = describeReparseTag(readReparseTag(result_.root));
        mrproper::core::LogFields fields;
        fields.push_back(mrproper::core::logField("path", core::toUtf8(result_.root)));
        fields.push_back(mrproper::core::logField("tag", tag));
        mrproper::core::logWarn("vfs.walk.root", "корень обхода — reparse point; не раскрыт", fields);
        finish(started);
        return std::move(result_);
    }

    try {
        if (cancelled()) {
            finish(started);
            return std::move(result_);
        }
        const std::wstring rootPath = result_.root;
        // Корень — единственный каталог, чью идентичность не с чем сравнивать:
        // его проверяет нормализация, а повторного подсчёта не будет.
        (void)walkDirectory(rootPath, 0, 0, 0);
    } catch (const std::bad_alloc&) {
        // Собранное сохраняем: половина обхода полезнее нуля (SPEC §4 FR-6:
        // ошибки не фатальны). Сообщение не форматируем — на это и нет памяти.
        noteError(std::wstring_view{}, ERROR_NOT_ENOUGH_MEMORY, L"нехватка памяти: обход прерван");
    } catch (const std::exception&) {
        // Исключение из посетителя: обход останавливаем, частичный результат
        // отдаём. Текст what() намеренно не переносим в отчёт: он в любой
        // кодировке, а перевод в UTF-16 означал бы выделение памяти прямо в
        // обработчике исключения — единственное место, где выделение памяти
        // гарантированно может снова упасть.
        noteError(std::wstring_view{}, ERROR_UNHANDLED_EXCEPTION, L"исключение в посетителе обхода");
    }

    finish(started);
    return std::move(result_);
}

}  // namespace

// ---------------------------------------------------------------------------
// Публичный интерфейс
// ---------------------------------------------------------------------------

const wchar_t* toString(EntryKind kind) noexcept {
    switch (kind) {
        case EntryKind::File:
            return L"file";
        case EntryKind::Directory:
            return L"directory";
        case EntryKind::Other:
            return L"other";
    }
    return L"unknown";
}

[[nodiscard]] std::wstring describeReparseTag(std::uint32_t tag) {
    switch (tag) {
        case 0:
            return L"ссылка (тег не прочитан)";
        case IO_REPARSE_TAG_SYMLINK:
            return L"символьная ссылка";
        case IO_REPARSE_TAG_MOUNT_POINT:
            return L"точка монтирования (junction)";
        case IO_REPARSE_TAG_CLOUD:
            return L"облачная заглушка (OneDrive)";
        case IO_REPARSE_TAG_WCI:
            return L"заглушка индексатора (Windows Search)";
        default:
            break;
    }
    // Неизвестный тег печатаем числом: в отчёте «0x9000001A» полезнее, чем
    // «неизвестный тег» — по нему видно, что это не symlink и не junction.
    // Шестнадцатеричный разряд печатается вручную: вызов printf-подобной
    // функции ради одного числа в отчёте не нужен, а разделители тысяч в
    // wprintf на этой машине зависят от локали.
    constexpr std::wstring_view kHexDigits = L"0123456789ABCDEF";
    std::array<wchar_t, 10> text{};  // «0x» + 8 разрядов
    text[0] = L'0';
    text[1] = L'x';
    for (std::size_t index = 0; index < 8; ++index) {
        const std::size_t shift = (7 - index) * 4;
        text[2 + index] = kHexDigits[(tag >> shift) & 0xFu];
    }
    return std::wstring(text.data(), text.size());
}

[[nodiscard]] NormalizedPath normalizePath(std::wstring_view root) {
    NormalizedPath out;
    if (root.empty()) {
        out.win32Code = ERROR_INVALID_NAME;
        return out;
    }
    const std::wstring extended = toExtendedPath(root);
    // FILE_READ_ATTRIBUTES — минимум, который требует документация
    // GetFinalPathNameByHandleW; FILE_FLAG_BACKUP_SEMANTICS обязателен для
    // открытия каталога. Права администратора не нужны ни там, ни здесь.
    const ScopedHandle handle(::CreateFileW(extended.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                                             kDirectoryFlags, nullptr));
    if (!handle) {
        const DWORD code = ::GetLastError();
        out.path = extended;
        out.win32Code = static_cast<std::uint32_t>(code);
        return out;
    }

    // Два прохода: первый спрашивает нужную длину, второй забирает строку.
    // Запас на завершающий нуль и одна повторная попытка на случай, если
    // между проходами каталог успели переименовать и путь стал длиннее.
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle.get(), &info)) {
        out.isDirectory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.reparsePoint = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    const DWORD needed = ::GetFinalPathNameByHandleW(handle.get(), nullptr, 0, flags);
    if (needed == 0) {
        const DWORD code = ::GetLastError();
        out.path = extended;
        out.win32Code = static_cast<std::uint32_t>(code);
        return out;
    }

    std::wstring buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD written =
            ::GetFinalPathNameByHandleW(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), flags);
        if (written == 0) {
            const DWORD code = ::GetLastError();
            out.path = extended;
            out.win32Code = static_cast<std::uint32_t>(code);
            return out;
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            out.path = std::move(buffer);
            out.resolved = true;
            return out;
        }
        buffer.assign(static_cast<std::size_t>(written) + 1, L'\0');
    }

    out.path = std::move(buffer);
    out.win32Code = ERROR_INSUFFICIENT_BUFFER;
    return out;
}

[[nodiscard]] bool pathIsInsideRoot(std::wstring_view path, std::wstring_view root) noexcept {
    if (path.empty() || root.empty()) {
        return false;
    }
    const std::size_t shorter = path.size() < root.size() ? path.size() : root.size();
    std::size_t matched = 0;
    while (matched < shorter && toUpperAscii(path[matched]) == toUpperAscii(root[matched])) {
        ++matched;
    }
    if (matched == 0) {
        return false;  // разные корни: «C:\…» и «D:\…», или UNC против буквы
    }
    if (matched == root.size()) {
        // Корень совпал целиком. Дальше — либо конец пути (путь == корень,
        // корень внутри самого себя), либо разделитель: «C:\Users» + «\Данные».
        if (matched == path.size()) {
            return true;
        }
        return isPathSeparator(path[matched]);
    }
    if (matched == path.size()) {
        // Совпал весь путь, а корень длиннее: «C:\Users\» не лежит внутри
        // «C:\Users\Данные» — наоборот, это его родитель. Отказ.
        return false;
    }
    if (!isPathSeparator(path[matched])) {
        // «C:\Windows» не является корнем для «C:\Windows.old\…»: дальше общего
        // префикса идти нельзя.
        return false;
    }
    // Общий префикс кончился, обе стороны продолжаются именем каталога: корень
    // должен при этом заканчиваться разделителем — «C:\» + «Users\Данные»
    // внутри, «C:\Users» + «Данные» снаружи.
    return isPathSeparator(root[matched]);
}

[[nodiscard]] WalkResult walk(std::wstring_view root, const WalkOptions& options, const WalkVisitor& visitor,
                              std::stop_token stop) {
    Walker walker(options, visitor, stop);
    return walker.run(root);
}

}  // namespace mrproper::platform::vfs
