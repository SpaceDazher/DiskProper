// Реализация нормализации путей и проверки принадлежности корню правила.
// Спека: §4 FR-6, §10, §5 (пути, ADS), §12 (инвариант «путь кандидата внутри
// корня правила»). Контракт, границы и список «чего модуль не делает» — в
// vfs_paths.hpp; здесь только код.
//
// Порядок чтения: форма пути (toExtendedWide, isExtendedFileSystemPath) →
// нормализация (finalPathWide, resolve) → сравнение (canonicalWide,
// isInsideRoot) → белый список (protectedRoots, isProtectedWide) → сводный
// вердикт (checkRuleRoot).
//
// Слой Win32 (SPEC §6.1, ADR-004): единственное место проекта, где допустим
// windows.h. Наружу выходят только std::string в UTF-8, целые числа и
// перечисления — ни одного типа Win32 в объявлениях vfs_paths.hpp, поэтому
// проверки формы пути и текстового сравнения не зависят от Windows.
#include "vfs_paths.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::vfs_paths {
namespace {

// ---------------------------------------------------------------------------
// Пределы
//
// Буферы Win32 в этой части API растут по требованию: длинный путь
// возвращает ERROR_INSUFFICIENT_BUFFER вместе с числом, которое нужно. Потолок
// нужен, чтобы «растущий» буфер на неисправном драйвере не превратился в
// бесконечное выделение. 32768 — потолок Windows для одного пути, и
// GetFinalPathNameByHandleW длиннее не возвращает.
// ---------------------------------------------------------------------------

constexpr std::size_t kInitialPathChars = 512;  // типовой путь Windows
constexpr std::size_t kMaxPathChars = 32768;     // потолок роста буфера пути
constexpr std::size_t kEnvironmentChars = 260;   // MAX_PATH, типовой предел переменной окружения
constexpr std::size_t kGrowAttempts = 6u;       // удвоений буфера, потолок по числу попыток

// Префиксы форм пути. Объявлены массивами, а не через auto, чтобы длина была
// видна рядом с содержимым. В комментариях префиксы записаны без завершающего
// обратного слэша намеренно: строка комментария, кончающаяся на «\», склеивается
// со следующей строкой ещё до разбора комментариев, и объявление после неё
// исчезает вместе с текстом пояснения.
constexpr wchar_t kExtendedPrefix[] = L"\\\\?\\";         // префикс «\\?\»
constexpr wchar_t kExtendedUncPrefix[] = L"\\\\?\\UNC\\";  // префикс «\\?\UNC»
constexpr wchar_t kUncPrefix[] = L"\\\\";                 // префикс «\\»
constexpr wchar_t kDevicePrefix[] = L"\\\\.\\";           // префикс «\\.»

// «\\?\» — четыре символа, они же длина ASCII-литерала для сравнений строк.
constexpr std::size_t kExtendedPrefixChars = 4u;

// Права и флаги открытия. FILE_READ_ATTRIBUTES достаточно, чтобы спросить имя
// каталога, и не требует повышения прав (SPEC §5: приложение стартует как
// invoker, повышение запрашивается только под операцию). FILE_FLAG_BACKUP_SEMANTICS
// — единственный способ открыть каталог вообще. FILE_FLAG_OPEN_REPARSE_POINT
// намеренно НЕ задан: разворачивать ссылку нужно, чтобы узнать настоящее имя.
// Признак того, что ссылка есть, берётся отдельно, из атрибутов исходного пути.
constexpr DWORD kQueryAccess = FILE_READ_ATTRIBUTES;
constexpr DWORD kShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD kOpenOptions = FILE_FLAG_BACKUP_SEMANTICS;

// Флаги GetFinalPathNameByHandleW. Основная пара — нормализация: 8.3 развёрнут,
// reparse пройден, том приведён к букве диска. Запасная — имя «как открыли»:
// оно тоже полезно (в отчёте видно, куда смотреть), но доказательством тождества
// не является, поэтому вызывающий знает о нём по флагу strong.
constexpr DWORD kFinalPathFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
constexpr DWORD kFinalPathFallbackFlags = FILE_NAME_OPENED | VOLUME_NAME_DOS;

// ---------------------------------------------------------------------------
// Мелкие предикаты по символам
// ---------------------------------------------------------------------------

constexpr bool isSeparatorW(wchar_t ch) noexcept {
    return ch == L'\\' || ch == L'/';
}

constexpr bool isSeparatorA(char ch) noexcept {
    return ch == '\\' || ch == '/';
}

constexpr bool isAsciiLetter(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

constexpr bool isDriveLetterW(wchar_t ch) noexcept {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

constexpr bool isDriveLetterA(char ch) noexcept {
    return isAsciiLetter(ch);
}

[[nodiscard]] bool startsWithAscii(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] bool startsWithWide(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// Регистронезависимое сравнение по правилам ordinal — тем же, что применяет
// файловая система. Именно поэтому здесь CompareStringOrdinal, а не towlower:
// в турецкой локали towlower(L'I') даёт «ı», и «C:\ınner» сравнился бы с
// «C:\INNER» как с одинаковыми буквами — а это уже путаница «внутри/снаружи».
[[nodiscard]] bool equalsOrdinalCI(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.empty() || right.empty() || left.size() != right.size()) return false;
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()),
                                  TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool startsWithOrdinalCI(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    return ::CompareStringOrdinal(text.data(), static_cast<int>(prefix.size()), prefix.data(),
                                  static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

// ---------------------------------------------------------------------------
// Форма пути
// ---------------------------------------------------------------------------

// Привести путь к расширенной форме на месте. false — путь не приведён:
// относительный, привязанный к диску («C:x» — это текущий каталог диска C, а
// не корень) либо начинается с «//», что для UNC разделителем не является.
[[nodiscard]] bool toExtendedWide(std::wstring& path) noexcept {
    if (startsWithWide(path, kExtendedPrefix) || startsWithWide(path, kDevicePrefix)) return true;
    if (startsWithWide(path, kUncPrefix)) {
        path = std::wstring(kExtendedUncPrefix) + path.substr(2u);
        return true;
    }
    // «C:\…» и «C:\» одинаково приводятся: третьим символом у корня тома
    // стоит разделитель, а не буква, и проверять его на букву нельзя.
    if (path.size() < 2u || !isDriveLetterW(path[0]) || !isSeparatorW(path[1])) return false;
    path[1] = L'\\';
    path.insert(0, kExtendedPrefix);
    return true;
}

// Привести UTF-16 путь к сравнимому виду на месте: разделители — к «\»
// (Win32 принимает оба, а текст правила может содержать любой), завершающие
// разделители сняты. «\\?\C:\» от этого превращается в «\\?\C:», а
// «\\?\C:\Users\» — в «\\?\C:\Users». Границу задаёт проверка символа после
// корня, а не завершающий разделитель, поэтому снятие делает обе стороны
// сравнимыми независимо от того, как корень был записан в правиле.
void canonicalizeWide(std::wstring& path) noexcept {
    for (wchar_t& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    while (!path.empty() && path.back() == L'\\') path.pop_back();
}

[[nodiscard]] std::wstring canonicalWide(std::string_view extendedUtf8) noexcept {
    std::wstring wide;
    if (!platform::tryToUtf16(extendedUtf8, wide)) return {};
    canonicalizeWide(wide);
    return wide;
}

// Корневая ли форма у расширенного пути. Пространство устройств («\\.\» и всё,
// что не начинается с «\\?\») — не путь файловой системы: через него в правило
// попал бы диск, а не каталог.
[[nodiscard]] bool isExtendedFileSystemPath(std::string_view extended) noexcept {
    if (extended.size() <= kExtendedPrefixChars || !startsWithAscii(extended, "\\\\?\\")) return false;
    if (isDriveLetterA(extended[kExtendedPrefixChars])) {
        // «\\?\C:\…»
        return extended.size() >= kExtendedPrefixChars + 3u && extended[kExtendedPrefixChars + 1u] == ':' &&
               isSeparatorA(extended[kExtendedPrefixChars + 2u]);
    }
    const std::string_view tail = extended.substr(kExtendedPrefixChars);
    if (startsWithAscii(tail, "UNC\\")) {
        // «\\?\UNC\server\share\…»: сервер и шара обязательны, иначе это не том.
        const std::string_view share = tail.substr(4u);
        const std::size_t sep = share.find_first_of("\\/");
        return sep != std::string_view::npos && sep > 0u && sep + 1u < share.size() &&
               share.find_first_of("\\/", sep + 1u) == std::string_view::npos;
    }
    if (startsWithAscii(tail, "Volume{")) {
        // «\\?\Volume{GUID}\…»: GUID без разделителя после закрывающей скобки —
        // это сам том, а не путь внутри него.
        const std::size_t close = extended.find('}', kExtendedPrefixChars);
        return close != std::string_view::npos && close + 1u < extended.size() &&
               isSeparatorA(extended[close + 1u]);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Нормализация
// ---------------------------------------------------------------------------

// Настоящее имя объекта. Возвращает код Windows: ERROR_SUCCESS при успехе,
// иначе код отказа (0 на входе означает «вызова не было», и наружу он не
// выходит — см. вызывающий код).
[[nodiscard]] DWORD finalPathWide(HANDLE handle, DWORD flags, std::wstring& out) noexcept {
    std::vector<wchar_t> buffer(kInitialPathChars);
    for (std::size_t attempt = 0; attempt < kGrowAttempts; ++attempt) {
        const DWORD written =
            ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
        if (written == 0u) return ::GetLastError();
        // written < size — имя поместилось вместе с завершающим NUL.
        if (written < buffer.size()) {
            out.assign(buffer.data(), written);
            return ERROR_SUCCESS;
        }
        // written >= size — нужен буфер на written + 1 символ (NUL входит в это
        // число по документации). Потолок отсекает неисправный драйвер.
        if (written >= kMaxPathChars) return ERROR_FILENAME_EXCED_RANGE;
        buffer.resize(static_cast<std::size_t>(written) + 1u);
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

// ---------------------------------------------------------------------------
// Защищённые каталоги
// ---------------------------------------------------------------------------

// Канонические расширенные формы защищённых корней и признак «список собран».
// Состояние общее на процесс, инициализируется под std::call_once: обход
// каталогов многопоточный (SPEC §5), и гонка за std::vector была бы не просто
// ошибкой, а порчей данных.
struct ProtectedRoots {
    std::vector<std::wstring> forms;
    bool ready{false};
};

[[nodiscard]] std::wstring environmentValue(const wchar_t* name) noexcept {
    if (name == nullptr || *name == L'\0') return {};
    std::vector<wchar_t> buffer(kEnvironmentChars);
    for (std::size_t attempt = 0; attempt < kGrowAttempts; ++attempt) {
        const DWORD written = ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0u) return {};
        if (written < buffer.size()) return std::wstring(buffer.data(), written);
        buffer.resize(static_cast<std::size_t>(written) + 1u);
    }
    return {};
}

// Добавить корень в список, если его там ещё нет. Повторы нужны: на 64-битной
// Windows ProgramFiles и ProgramW6432 указывают на один каталог, и без
// дедупликации список содержал бы два одинаковых элемента.
void addProtectedRoot(std::vector<std::wstring>& forms, std::wstring value) noexcept {
    if (value.empty() || !toExtendedWide(value)) return;
    canonicalizeWide(value);
    if (value.empty()) return;
    for (const std::wstring& existing : forms) {
        if (equalsOrdinalCI(existing, value)) return;
    }
    forms.push_back(std::move(value));
}

[[nodiscard]] std::vector<std::wstring> collectProtectedRoots() {
    std::vector<std::wstring> forms;
    // Переменная окружения — источник по умолчанию, литерал — запас на
    // окружение, где переменной нет. Список литералов совпадает с путями в
    // наборе правил (§9.2), чтобы развёрнутый и неразвёрнутый вид правила вели
    // себя одинаково.
    addProtectedRoot(forms, environmentValue(L"SystemRoot"));
    addProtectedRoot(forms, environmentValue(L"ProgramFiles"));
    addProtectedRoot(forms, environmentValue(L"ProgramFiles(x86)"));
    addProtectedRoot(forms, environmentValue(L"ProgramW6432"));
    addProtectedRoot(forms, L"C:\\Windows");
    addProtectedRoot(forms, L"C:\\Program Files");
    addProtectedRoot(forms, L"C:\\Program Files (x86)");
    return forms;
}

// Список собирается один раз за процесс. Единственный перехват исключения в
// модуле и не в горячем цикле, а при инициализации: без списка защищённых
// каталогов проверка обязана отказывать, а не пропускать.
[[nodiscard]] const ProtectedRoots& protectedRoots() noexcept {
    static ProtectedRoots roots;
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            roots.forms = collectProtectedRoots();
            roots.ready = true;
        } catch (const std::bad_alloc&) {
            roots.ready = false;
        }
    });
    return roots;
}

// Корень тома в канонической форме: «\\?\C:» либо «\\?\UNC\server\share».
// Нужен, чтобы отличить «C:\System Volume Information» (защищён) от
// «D:\Backup\System Volume Information» (обычный каталог пользователя).
[[nodiscard]] bool isVolumeRootWide(std::wstring_view form) noexcept {
    if (form.size() == kExtendedPrefixChars + 2u && isDriveLetterW(form[kExtendedPrefixChars]) &&
        form[kExtendedPrefixChars + 1u] == L':') {
        return true;
    }
    if (!startsWithWide(form, kExtendedUncPrefix)) return false;
    const std::wstring_view share = form.substr(std::wstring_view(kExtendedUncPrefix).size());
    const std::size_t sep = share.find(L'\\');
    return sep != std::wstring_view::npos && sep > 0u && sep + 1u < share.size() &&
           share.find(L'\\', sep + 1u) == std::wstring_view::npos;
}

// «System Volume Information» прямым потомком корня тома. Имя — ASCII, поэтому
// сравнение с ним своё, и регистр тут не при чём: имя каталога Windows хранит
// как есть, а путь к нему может прийти в любом регистре.
[[nodiscard]] bool isProtectedSystemFolderWide(std::wstring_view form) noexcept {
    constexpr std::wstring_view kSystemVolume = L"System Volume Information";
    const std::size_t sep = form.find_last_of(L'\\');
    if (sep == std::wstring_view::npos || sep == 0u) return false;
    const std::wstring_view leaf = form.substr(sep + 1u);
    if (leaf.size() != kSystemVolume.size()) return false;
    for (std::size_t i = 0; i < leaf.size(); ++i) {
        wchar_t ch = leaf[i];
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        if (ch != kSystemVolume[i]) return false;
    }
    return isVolumeRootWide(form.substr(0u, sep));
}

[[nodiscard]] bool isProtectedWide(std::wstring_view form) noexcept {
    if (form.empty()) return false;
    const ProtectedRoots& roots = protectedRoots();
    if (!roots.ready) return true;  // список не собрался: не знаем — не пропускаем
    for (const std::wstring& root : roots.forms) {
        if (root.empty() || !startsWithOrdinalCI(form, root)) continue;
        // Граница компонента: «C:\Windows.old» не защищён, «C:\Windows» — да.
        if (form.size() == root.size() || form[root.size()] == L'\\') return true;
    }
    return isProtectedSystemFolderWide(form);
}

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------

// Один отказ — одна запись с путём и кодом Windows (SPEC §12: «все ошибки в
// логе с путём и HRESULT»). Поля собираются явно, а не макросом MRP_LOG_*:
// список из двух и более пар logFieldList разворачивает в один вызов logField,
// то есть такой вызов не компилируется.
void logRejected(const char* message, Verdict verdict, std::string_view pathUtf8, std::uint32_t lastError) noexcept {
    core::LogFields fields;
    fields.reserve(3u);
    fields.push_back(core::logField("verdict", verdictName(verdict)));
    fields.push_back(core::logField("path", pathUtf8));
    if (lastError != 0u) fields.push_back(core::logField("hr", lastError));
    core::logWarn("vfs.guard", message, std::move(fields));
}

}  // namespace

// ---------------------------------------------------------------------------
// Публичный интерфейс
// ---------------------------------------------------------------------------

bool isRootedFileSystemPath(std::string_view pathUtf8) noexcept {
    return isExtendedFileSystemPath(toExtendedPath(pathUtf8));
}

bool hasStreamSuffix(std::string_view pathUtf8) noexcept {
    // Двоеточие в префиксе тома — не поток: «C:» и «\\?\C:» (у «\\?\UNC\…»
    // двоеточия нет вовсе). Всё остальное — именованный поток, §5: «ADS не
    // трогаем», и «не трогаем» здесь означает «не признаём путём для удаления».
    for (std::size_t i = 0; i < pathUtf8.size(); ++i) {
        if (pathUtf8[i] != ':') continue;
        if (i == 1u || i == 5u) continue;
        return true;
    }
    return false;
}

bool hasDotSegment(std::string_view pathUtf8) noexcept {
    std::size_t pos = 0;
    while (pos < pathUtf8.size()) {
        while (pos < pathUtf8.size() && isSeparatorA(pathUtf8[pos])) ++pos;
        const std::size_t begin = pos;
        while (pos < pathUtf8.size() && !isSeparatorA(pathUtf8[pos])) ++pos;
        const std::size_t size = pos - begin;
        if (size == 1u && pathUtf8[begin] == '.') return true;
        if (size == 2u && pathUtf8[begin] == '.' && pathUtf8[begin + 1u] == '.') return true;
    }
    return false;
}

std::string toExtendedPath(std::string_view pathUtf8) noexcept {
    if (pathUtf8.empty()) return {};
    std::wstring wide;
    if (!platform::tryToUtf16(pathUtf8, wide)) return {};
    if (!toExtendedWide(wide)) return {};
    std::string out;
    if (!platform::tryToUtf8(wide, out)) return {};
    return out;
}

bool isReparsePoint(std::string_view pathUtf8, std::uint32_t& lastError) noexcept {
    lastError = 0u;
    const std::string extended = toExtendedPath(pathUtf8);
    if (extended.empty()) {
        lastError = ERROR_INVALID_NAME;
        return false;
    }
    std::wstring wide;
    if (!platform::tryToUtf16(extended, wide)) {
        lastError = ERROR_NO_UNICODE_TRANSLATION;
        return false;
    }
    // GetFileAttributesW отдаёт атрибуты самой ссылки, а не её цели, — ровно то,
    // что нужно: открывать с OPEN_REPARSE_POINT и проверять цель было бы лишним
    // системным вызовом на каждый файл прохода. Форма вызова — ровно один
    // аргумент, а отказ читается как INVALID_FILE_ATTRIBUTES: двухаргументная
    // форма есть только у GetFileAttributesExW, и это другой вызов.
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        lastError = ::GetLastError();
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
}

Resolution resolve(std::string_view pathUtf8) noexcept {
    Resolution out;
    const std::string extended = toExtendedPath(pathUtf8);
    if (extended.empty()) {
        out.lastError = ERROR_INVALID_NAME;
        return out;
    }
    std::wstring wide;
    if (!platform::tryToUtf16(extended, wide)) {
        out.lastError = ERROR_NO_UNICODE_TRANSLATION;
        return out;
    }

    // Признак точки перехода читаем ДО открытия: после открытия хендл указывает
    // уже на цель, и её атрибуты ни о какой ссылке не говорят.
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    const bool attributesKnown = attributes != INVALID_FILE_ATTRIBUTES;
    std::uint32_t attributesError = 0u;
    if (!attributesKnown) attributesError = ::GetLastError();

    const HANDLE raw = ::CreateFileW(wide.c_str(), kQueryAccess, kShareAccess, nullptr, OPEN_EXISTING, kOpenOptions,
                                     nullptr);
    const std::uint32_t createError = ::GetLastError();
    // adopt, а не adoptChecked: причина отказа уже получена, а бросать здесь
    // нечего — модуль не бросает исключений ни в горячем цикле, ни вне его.
    const auto handle = platform::adopt(raw);
    if (!handle.valid()) {
        out.lastError = createError != ERROR_SUCCESS ? createError : ERROR_INVALID_HANDLE;
        return out;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle.get(), &information)) {
        out.directory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
    }

    std::wstring finalWide;
    DWORD finalError = finalPathWide(handle.get(), kFinalPathFlags, finalWide);
    if (finalError != ERROR_SUCCESS) {
        // Нормализация не вышла: сохраняем её отказ (он информативнее
        // запасного) и пробуем имя «как открыли». Оно годится для отчёта, но
        // доказательством тождества не является, поэтому strong остаётся false.
        const DWORD normalizedError = finalError;
        finalError = finalPathWide(handle.get(), kFinalPathFallbackFlags, finalWide);
        if (finalError != ERROR_SUCCESS) {
            out.lastError = normalizedError;
            return out;
        }
        out.strong = false;
    } else {
        // Имя нормализовано, но без атрибутов исходного пути неизвестно, была
        // ли ссылка по дороге, — доказательства тождества тоже нет.
        out.strong = attributesKnown;
        if (!attributesKnown) out.lastError = attributesError;
    }

    if (!platform::tryToUtf8(finalWide, out.path)) {
        out.path.clear();
        out.lastError = ERROR_NO_UNICODE_TRANSLATION;
        out.strong = false;
        return out;
    }
    out.resolved = true;
    out.reparsePoint = attributesKnown && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
    return out;
}

bool isInsideRoot(std::string_view pathUtf8, std::string_view rootUtf8) noexcept {
    const std::wstring path = canonicalWide(toExtendedPath(pathUtf8));
    const std::wstring root = canonicalWide(toExtendedPath(rootUtf8));
    if (path.empty() || root.empty()) return false;
    if (path.size() < root.size()) return false;
    if (!startsWithOrdinalCI(path, root)) return false;
    // Граница компонента. «C:\Users\Ev» против «C:\Users\Evil\x»: префикс
    // совпал, а следующий символ — буква, то есть другой каталог.
    return path.size() == root.size() || path[root.size()] == L'\\';
}

std::vector<std::string> protectedRootsUtf8() {
    std::vector<std::string> out;
    const ProtectedRoots& roots = protectedRoots();
    out.reserve(roots.forms.size());
    for (const std::wstring& form : roots.forms) {
        std::string text;
        if (platform::tryToUtf8(form, text)) out.push_back(std::move(text));
    }
    return out;
}

bool isProtectedPath(std::string_view pathUtf8) noexcept {
    return isProtectedWide(canonicalWide(toExtendedPath(pathUtf8)));
}

const char* verdictName(Verdict verdict) noexcept {
    switch (verdict) {
        case Verdict::Inside:
            return "inside";
        case Verdict::OutsideRoot:
            return "outside-root";
        case Verdict::ReparsePoint:
            return "reparse-point";
        case Verdict::ProtectedPath:
            return "protected-path";
        case Verdict::Unresolvable:
            break;
    }
    return "unresolvable";
}

RootGuard prepareRoot(std::string_view rootUtf8) noexcept {
    RootGuard out;
    if (!isRootedFileSystemPath(rootUtf8) || hasDotSegment(rootUtf8) || hasStreamSuffix(rootUtf8)) {
        out.rejected = true;
        out.lastError = ERROR_INVALID_NAME;
        return out;
    }
    const Resolution resolution = resolve(rootUtf8);
    out.lastError = resolution.lastError;
    if (!resolution.resolved) {
        out.rejected = true;
        return out;
    }
    if (resolution.reparsePoint) {
        // Правило, уводящее обход за пределы своего каталога, — это не правило,
        // а закладка на удаление мимо корня.
        out.rejected = true;
        out.lastError = 0u;
        return out;
    }
    if (!resolution.strong) {
        // Корень оставляем пригодным: он задан правилом, готовится один раз и
        // задаёт границу текстом. Ложный запрет лечится повторным prepareRoot с
        // путём, который нормализуется, а ложного разрешения здесь быть не
        // может — решение принимается по нормализованному имени файла.
        core::logWarn("vfs.root", "корень правила не нормализован, граница сравнивается текстом",
                      core::LogFields{core::logField("root", rootUtf8)});
    }
    out.root = resolution.path;
    out.resolved = true;
    return out;
}

Guard checkRuleRoot(std::string_view pathUtf8, const RootGuard& root) noexcept {
    Guard out;
    if (!root.resolved || root.root.empty()) {
        out.lastError = root.lastError;
        logRejected("корень правила непригоден", out.verdict, pathUtf8, out.lastError);
        return out;
    }
    if (!isRootedFileSystemPath(pathUtf8) || hasDotSegment(pathUtf8) || hasStreamSuffix(pathUtf8)) {
        out.lastError = ERROR_INVALID_NAME;
        logRejected("путь неприемлем для проверки корня правила", out.verdict, pathUtf8, out.lastError);
        return out;
    }

    const Resolution resolution = resolve(pathUtf8);
    out.normalized = resolution.path;
    out.lastError = resolution.lastError;
    if (!resolution.resolved) {
        logRejected("не удалось получить нормализованный путь", out.verdict, pathUtf8, out.lastError);
        return out;
    }
    if (!resolution.strong) {
        // Имя «как открыли» не отличает «C:\PROGRA~1\X» от «C:\Program Files\X»,
        // поэтому вердикт о попадании внутрь корня на нём не выносится: пусть
        // файл уцелеет и попадёт в отчёт, чем будет удалён не тот.
        logRejected("путь не нормализован, попадание внутрь корня не доказано", out.verdict, pathUtf8, out.lastError);
        return out;
    }
    if (resolution.reparsePoint) {
        out.verdict = Verdict::ReparsePoint;
        logRejected("объект является точкой перехода, FR-6 требует пропуска", out.verdict, pathUtf8, 0u);
        return out;
    }
    if (!isInsideRoot(resolution.path, root.root)) {
        out.verdict = Verdict::OutsideRoot;
        logRejected("путь вне корня правила", out.verdict, pathUtf8, 0u);
        return out;
    }
    if (isProtectedPath(resolution.path)) {
        out.verdict = Verdict::ProtectedPath;
        logRejected("путь в защищённом системном каталоге", out.verdict, pathUtf8, 0u);
        return out;
    }
    out.verdict = Verdict::Inside;
    return out;
}

Guard checkRuleRoot(std::string_view pathUtf8, std::string_view rootUtf8) noexcept {
    return checkRuleRoot(pathUtf8, prepareRoot(rootUtf8));
}

}  // namespace mrproper::platform::vfs_paths
