// Удаление файлов и каталогов — реализация. Спека и границы модуля описаны в
// vfs_delete.hpp; здесь комментарии к решениям, которые из сигнатуры не видны.
//
// Слой Win32 (SPEC §6.1, ADR-004): единственное место проекта, где допустим
// windows.h. Ядро (core::model) о DeleteFileW/RemoveDirectoryW не знает, и
// поэтому единица трансляции с WinAPI физически не может попасть в
// mrproper_core: файлы берутся каталогом, а граница проходит по границе
// каталогов.
//
// Порядок проверок перед удалением (важно: каждая следующая опирается на
// предыдущую, и обойти их нельзя):
//
//   canonicalizePath           → канон `\\?\`, разделители, `.`/`..`;
//                               непригодный путь отвергается целиком;
//   checkProtectedCanonical    → корень тома, жёсткий и мягкий списки,
//                               принадлежность корню правила;
//   GetFileAttributesW         → тип объекта и признак reparse point;
//   GetFinalPathNameByHandleW  → настоящий адрес объекта; защита проверяется
//                               ещё раз по нему;
//   tryFileStamp               → §10: «проверка Version/LastWrite перед
//                               удалением»;
//   DeleteFileW/RemoveDirectoryW с повтором на кодах блокировки.
//
// Почему канонический путь по дескриптору обязателен, а не «на всякий случай».
// Сравнение строк с корнем правила обходится junction'у, который лежит внутри
// разрешённого каталога: лексически «C:\Temp\link\file» внутри «C:\Temp», а
// реально файл лежит в «C:\Windows\System32». Лексическая проверка такой путь
// пропускает, а GetFinalPathNameByHandleW — нет: он возвращает адрес цели
// ссылки. Поэтому канонический путь не «дополняет» проверку, а заменяет её
// лексическую часть.
//
// Почему reparse point пропускается всегда, без настройки. FR-6 требует этого
// прямо («пропуск reparse points (symlink/junction) — защита от петель и выхода
// за пределы пути»), а настройка «следовать по ссылкам» была бы рычагом, который
// однажды включат ради скорости.
//
// Почему отказ по политике — это тоже HRESULT. §12 требует, чтобы «все ошибки
// в логе с путём и HRESULT». Отказ «путь защищён» — не системная ошибка, но в
// отчёте и логе он должен выглядеть так же, как остальные строки, поэтому
// ACCESS_DENIED/WRITE_PROTECT/INVALID_PARAMETER/CANCELLED проставляются явно, а в
// detail кладётся причина на человеческом языке.
#include "vfs_delete.hpp"

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::vfs {
namespace {

using ScopedHandle = platform::unique_handle<platform::KernelHandlePolicy>;
using ScopedFind = platform::unique_handle<platform::FindHandlePolicy>;

// Предел CompareStringOrdinal: длина передаётся в int. Всё длиннее 32767
// символов — это рекурсивный каталог, и сравнивать его строками всё равно
// незачем: ни один путь Windows не бывает длиннее.
constexpr std::size_t kMaxComparableChars = 0x7FFF0000u;

// Буфер для GetFinalPathNameByHandleW: предел расширенного пути — 32767 символов
// плюс завершающий ноль, поэтому одного буфера достаточно. Ветка с ростом
// оставлена на случай, если Windows вернёт больше запрошенного.
constexpr std::size_t kExtendedPathBufferChars = 32768;

// Как часто обход дерева смотрит на stop_token: §6.4 требует кооперативную
// отмену с проверкой «каждые 256 элементов».
constexpr std::uint32_t kDeleteCancelCheckPeriod = 256;

// Срез ожидания backoff: отмена не должна ждать конца паузы (пауза достигает
// двух секунд, а пользователь уже нажал «Отмена»).
constexpr std::chrono::milliseconds kBackoffSlice{25};

// ---------------------------------------------------------------------------
// Сравнение и разбор пути
// ---------------------------------------------------------------------------

[[nodiscard]] bool isDriveLetter(wchar_t symbol) noexcept {
    return (symbol >= L'A' && symbol <= L'Z') || (symbol >= L'a' && symbol <= L'z');
}

// Сравнение без учёта регистра через CompareStringOrdinal, а не через towlower:
// регистр файлов на Windows задаёт файловая система тома, а не язык системы, и
// _wcsicmp ориентируется как раз на язык (в turkic-локалях он ведёт себя
// неожиданно). Ordinal-сравнение регистр не трактует вовсе.
[[nodiscard]] bool equalNoCase(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    if (left.empty()) return true;
    if (left.size() > kMaxComparableChars) return false;
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool startsWithNoCase(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    return equalNoCase(text.substr(0, prefix.size()), prefix);
}

// pathInside(child, root) — «child лежит внутри root или совпадает с ним».
// Оба аргумента обязаны быть канонизированы (canonicalizePath): иначе сравнение
// «C:\Temp\x» с «\\?\C:\Temp» даст ложное «вне корня» и правило перестанет
// работать на длинных путях.
[[nodiscard]] bool pathInside(std::wstring_view child, std::wstring_view root) noexcept {
    if (root.empty() || child.size() < root.size()) return false;
    if (child.size() > kMaxComparableChars) return false;
    if (!equalNoCase(child.substr(0, root.size()), root)) return false;
    if (child.size() == root.size()) return true;
    // «C:\Temp2» не внутри «C:\Temp»: после корня обязан быть разделитель.
    return child[root.size()] == L'\\';
}

// Строгое вхождение: нужно для правила «мягкий запрет снимается только корнем
// правила, который строго глубже защищённого каталога». Корень правила, равный
// защищённому каталогу, снятия не даёт: иначе правило с locator «C:\Windows\**»
// удалило бы всю Windows.
[[nodiscard]] bool pathStrictlyInside(std::wstring_view child, std::wstring_view root) noexcept {
    return child.size() > root.size() && pathInside(child, root);
}

// UNC-корень: сервер и имя ресурса — часть корня, а не компоненты пути.
// Иначе «\\?\UNC\server\share» сравнивался бы с «\\?\C:\server\share» как с одним
// и тем же деревом. Возвращает начало «хвоста» после корня.
[[nodiscard]] bool appendUncRoot(const std::wstring& text, std::size_t start, std::wstring& prefix,
                                 std::size_t& tailStart) {
    const std::size_t serverEnd = text.find(L'\\', start);
    if (serverEnd == std::wstring::npos || serverEnd == start) return false;
    const std::size_t shareEnd = text.find(L'\\', serverEnd + 1);
    if (shareEnd == std::wstring::npos) {
        // «\\server\share» — уже корень, хвоста нет.
        prefix = L"\\\\?\\UNC\\" + text.substr(start);
        tailStart = text.size();
        return true;
    }
    if (shareEnd == serverEnd + 1) return false;  // пустое имя ресурса
    prefix = L"\\\\?\\UNC\\" + text.substr(start, shareEnd + 1 - start);
    tailStart = shareEnd + 1;
    return true;
}

// Лексический канон пути. Одно выделение памяти на вызов: склейка идёт в
// единственную строку, начало последней компоненты хранится индексом.
//
// Возвращает пустую строку, если путь непригоден как цель удаления:
//   * пустой или относительный («Temp», «\Temp», «C:Temp»);
//   * пространство имён устройств («\\.\PhysicalDrive0», «\\?\GLOBALROOT\…»);
//   * «..», уводящий за корень тома («C:\..\Windows»).
[[nodiscard]] std::wstring canonicalizePath(std::wstring_view path) {
    if (path.empty()) return {};

    std::wstring text;
    text.reserve(path.size() + 8);
    for (const wchar_t symbol : path) {
        // Разделители приводим сразу: Win32 принимает оба, а сравнение строк — нет.
        text.push_back(symbol == L'/' ? L'\\' : symbol);
    }

    if (startsWithNoCase(text, L"\\\\.\\")) return {};        // устройства: удалять нечего и незачем
    if (startsWithNoCase(text, L"\\\\?\\GLOBALROOT")) return {};

    std::wstring prefix;
    std::size_t tailStart = 0;

    if (startsWithNoCase(text, L"\\\\?\\")) {
        const std::size_t separator = text.find(L'\\', 4);
        if (separator == std::wstring::npos) {
            // «\\?\C:» без разделителя — корень тома, а «\\?\мусор» — нет.
            if (text.size() != 6 || !isDriveLetter(text[4]) || text[5] != L':') return {};
            prefix = text + L"\\";
            tailStart = text.size();
        } else {
            prefix = text.substr(0, separator + 1);
            tailStart = separator + 1;
        }
        if (startsWithNoCase(std::wstring_view(text).substr(4), L"UNC\\")) {
            if (!appendUncRoot(text, tailStart, prefix, tailStart)) return {};
        }
    } else if (startsWithNoCase(text, L"\\\\")) {
        if (!appendUncRoot(text, 2, prefix, tailStart)) return {};
    } else if (text.size() >= 2 && isDriveLetter(text[0]) && text[1] == L':') {
        if (text.size() == 2) {
            prefix = L"\\\\?\\" + text + L"\\";
            tailStart = text.size();
        } else if (text[2] == L'\\') {
            prefix = L"\\\\?\\" + text.substr(0, 2) + L"\\";
            tailStart = 3;
        } else {
            return {};  // «C:Temp» — путь относительно текущего каталога диска
        }
    } else {
        return {};
    }

    std::wstring out;
    out.reserve(text.size() + 8);
    out.append(prefix);

    // Начало последней записанной компоненты: нужно для «..», который убирает
    // именно её. out.size() — «компонент нет».
    std::size_t lastStart = out.size();
    std::size_t index = tailStart;
    while (index < text.size()) {
        std::size_t end = text.find(L'\\', index);
        if (end == std::wstring::npos) end = text.size();
        const std::size_t length = end - index;
        if (length == 0 || (length == 1 && text[index] == L'.')) {
            index = end + 1;  // пустая компонента (двойной разделитель) или «.»
            continue;
        }
        if (length == 2 && text[index] == L'.' && text[index + 1] == L'.') {
            if (lastStart == out.size()) return {};  // выход за корень тома
            const std::size_t previous = out.find_last_of(L'\\', lastStart - 1);
            if (previous == std::wstring::npos || previous < prefix.size() - 1) return {};
            out.resize(previous + 1);
            lastStart = previous + 1;
            index = end + 1;
            continue;
        }
        // Разделитель между компонентами: без него «C:\Users\Daniil\Temp» склеивался
        // в «C:\Users\Daniil\TempX» — deleteEntry отказывал собственному файлу как
        // лежащему вне корня, а deleteTree рапортовал об удалённом дереве, которое
        // оставалось на диске (находка ревью F2, интеграционные провалы vfsEdge).
        if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
        out.append(text, index, length);
        lastStart = out.size();
        index = end + 1;
    }

    // Висящий разделитель: «C:\Temp\» и «C:\Temp» — один и тот же объект, а
    // сравнение строк этого не знает.
    if (out.size() > prefix.size() && out.back() == L'\\') out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// Списки защищённых путей
// ---------------------------------------------------------------------------

struct RootLists {
    std::vector<std::wstring> volumes;  // корни томов: «\\?\C:\», «\\?\Volume{…}\»
    std::vector<std::wstring> never;    // жёсткий запрет, правилом не снимается
    // Мягкий запрет, снимается корнем правила. Имя не protected: это ключевое
    // слово C++, и «protected» как имя элемента структуры не компилируется.
    std::vector<std::wstring> guarded;
};

void appendChild(std::vector<std::wstring>& out, const std::wstring& parent, std::wstring_view child) {
    std::wstring joined = parent;
    while (!joined.empty() && joined.back() == L'\\') joined.pop_back();
    joined.push_back(L'\\');
    joined.append(child);
    const std::wstring canonical = canonicalizePath(joined);
    if (!canonical.empty()) out.push_back(canonical);
}

[[nodiscard]] std::wstring environmentValue(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed + 1);
    if (written == 0 || written > needed) return {};
    value.resize(written);
    return value;
}

// Список строится один раз за жизнь процесса: он зависит от установки (буквы
// дисков, %ProgramFiles%, %ProgramData%), а не от проверяемого пути, и в рантайме
// не меняется. Потокобезопасен (инициализация локальной статической переменной
// гарантирована стандартом), поэтому deleteTree из нескольких потоков движка
// (§4 FR-6: пул) не синхронизируется вручную.
[[nodiscard]] const RootLists& rootLists() {
    static const RootLists lists = [] {
        RootLists built;

        // Тома: буквы дисков плюс все тома, включая безбуквенные (Recovery, том
        // без буквы). Каталоги вроде System Volume Information перечисляются для
        // каждого тома, поэтому правило не обходит защиту через «\\?\Volume{GUID}».
        const DWORD mask = ::GetLogicalDrives();
        for (int index = 0; index < 26; ++index) {
            const DWORD bit = static_cast<DWORD>(1u << static_cast<unsigned>(index));
            if ((mask & bit) == 0) continue;
            const wchar_t letter = static_cast<wchar_t>(L'A' + index);
            built.volumes.push_back(std::wstring(L"\\\\?\\") + letter + L":\\");
        }
        wchar_t volumeName[MAX_PATH] = {};
        const ScopedFind finder(::FindFirstVolumeW(volumeName, MAX_PATH));
        if (finder) {
            built.volumes.emplace_back(volumeName);
            while (::FindNextVolumeW(finder.get(), volumeName, MAX_PATH) != FALSE) {
                built.volumes.emplace_back(volumeName);
            }
        }
        for (const std::wstring& volume : built.volumes) {
            appendChild(built.never, volume, L"System Volume Information");
            appendChild(built.never, volume, L"$Recycle.Bin");
            appendChild(built.never, volume, L"Recovery");
        }

        // Системный каталог. Жёсткий запрет — ровно то, что FR-3 называет
        // «только оценка» (WinSxS, Installer: удаление ломает component store и
        // uninstall/repair), плюс System32/SysWOW64/DriverStore. Мягый запрет — сам
        // каталог Windows: правила temp.system и logs чистят C:\Windows\Temp и
        // C:\Windows\Logs, а вот C:\Windows целиком снять запрет не может,
        // потому что корень правила должен быть строго глубже.
        wchar_t windows[MAX_PATH] = {};
        const UINT windowsLength = ::GetWindowsDirectoryW(windows, MAX_PATH);
        if (windowsLength > 0 && windowsLength < MAX_PATH) {
            const std::wstring systemRoot(windows, windowsLength);
            appendChild(built.never, systemRoot, L"System32");
            appendChild(built.never, systemRoot, L"SysWOW64");
            appendChild(built.never, systemRoot, L"WinSxS");
            appendChild(built.never, systemRoot, L"Installer");
            appendChild(built.never, systemRoot, L"DriverStore");
            appendChild(built.never, systemRoot, L"assembly");
            const std::wstring canonical = canonicalizePath(systemRoot);
            if (!canonical.empty()) built.guarded.push_back(canonical);
        }
        wchar_t system[MAX_PATH] = {};
        const UINT systemLength = ::GetSystemDirectoryW(system, MAX_PATH);
        if (systemLength > 0 && systemLength < MAX_PATH) {
            const std::wstring canonical = canonicalizePath(std::wstring(system, systemLength));
            if (!canonical.empty()) built.never.push_back(canonical);
        }

        // Program Files во всех вариантах: на 32-битном клиенте «ProgramFiles» —
        // x86-каталог, а настоящий 64-битный лежит в ProgramW6432.
        for (const wchar_t* name : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
            const std::wstring canonical = canonicalizePath(environmentValue(name));
            if (!canonical.empty()) built.never.push_back(canonical);
        }
        // ProgramData — мягкий запрет: правило wer чистит
        // C:\ProgramData\Microsoft\Windows\WER, но не сам ProgramData.
        const std::wstring programData = canonicalizePath(environmentValue(L"ProgramData"));
        if (!programData.empty()) built.guarded.push_back(programData);

        return built;
    }();
    return lists;
}

// ---------------------------------------------------------------------------
// Параметры операции, приведённые один раз
// ---------------------------------------------------------------------------

// allowedRoot канонизируется один раз на операцию, а не на каждый элемент
// дерева: в deleteTree это сотни тысяч вызовов, и канонизация корня в каждом
// была бы чистой потерей.
struct PreparedOptions {
    std::wstring root;
    bool rootValid{};
    std::uint32_t maxAttempts{kDefaultDeleteAttempts};
    std::chrono::milliseconds backoff{kDefaultDeleteBackoff};
    std::chrono::milliseconds maxBackoff{kDefaultMaxDeleteBackoff};
    std::size_t maxReportedProblems{kMaxReportedDeleteProblems};
};

[[nodiscard]] PreparedOptions prepareOptions(const DeleteOptions& options) {
    PreparedOptions prepared;
    prepared.root = canonicalizePath(options.allowedRoot);
    // Пустой корень — не «проверять нечего», а запрет: операция удаления без
    // корня правила и есть тот случай «удаление вне корня правила» из §10.
    prepared.rootValid = !prepared.root.empty();
    prepared.maxAttempts = options.maxAttempts;
    prepared.backoff = options.backoff;
    prepared.maxBackoff = options.maxBackoff;
    prepared.maxReportedProblems = options.maxReportedProblems;
    return prepared;
}

[[nodiscard]] DeleteStatus statusFor(ProtectionVerdict verdict) noexcept {
    switch (verdict) {
        case ProtectionVerdict::InvalidPath:
            return DeleteStatus::SkippedInvalid;
        case ProtectionVerdict::OutsideRuleRoot:
            return DeleteStatus::SkippedOutsideRoot;
        case ProtectionVerdict::NeverAllowed:
        case ProtectionVerdict::ProtectedRoot:
            return DeleteStatus::SkippedProtected;
        case ProtectionVerdict::Allowed:
            break;
    }
    return DeleteStatus::SkippedProtected;
}

[[nodiscard]] DWORD policyCode(ProtectionVerdict verdict) noexcept {
    return verdict == ProtectionVerdict::InvalidPath ? ERROR_INVALID_PARAMETER : ERROR_ACCESS_DENIED;
}

[[nodiscard]] std::wstring reasonFor(ProtectionVerdict verdict, std::wstring_view root) {
    switch (verdict) {
        case ProtectionVerdict::InvalidPath:
            return L"путь пуст, относительный или указывает в пространство имён устройств";
        case ProtectionVerdict::OutsideRuleRoot:
            return L"путь вне корня правила (allowedRoot не задан или не содержит путь)";
        case ProtectionVerdict::NeverAllowed:
            return std::wstring(L"жёсткий защищённый каталог: ") + std::wstring(root);
        case ProtectionVerdict::ProtectedRoot:
            return std::wstring(L"защищённый каталог, корень правила не задан внутри него: ") + std::wstring(root);
        case ProtectionVerdict::Allowed:
            break;
    }
    return {};
}

[[nodiscard]] ProtectionCheck checkProtectedCanonical(std::wstring_view canonical, const PreparedOptions& prepared) {
    ProtectionCheck check;
    if (canonical.empty() || !prepared.rootValid) {
        check.verdict = prepared.rootValid ? ProtectionVerdict::InvalidPath : ProtectionVerdict::OutsideRuleRoot;
        return check;
    }
    const RootLists& lists = rootLists();
    // Корень тома удалять нельзя: RemoveDirectoryW на «C:\» и не пройдёт, а
    // правило с таким корнем — это уже не утилита очистки.
    for (const std::wstring& volume : lists.volumes) {
        if (equalNoCase(canonical, volume)) {
            check.verdict = ProtectionVerdict::NeverAllowed;
            check.root = volume;
            return check;
        }
    }
    if (!pathInside(canonical, prepared.root)) {
        check.verdict = ProtectionVerdict::OutsideRuleRoot;
        return check;
    }
    for (const std::wstring& forbidden : lists.never) {
        if (pathInside(canonical, forbidden)) {
            check.verdict = ProtectionVerdict::NeverAllowed;
            check.root = forbidden;
            return check;
        }
    }
    for (const std::wstring& guarded : lists.guarded) {
        // Снять мягкий запрет можно только корнем правила, который строго
        // глубже защищённого каталога: C:\Windows\Temp проходит, C:\Windows — нет.
        if (pathInside(canonical, guarded) && !pathStrictlyInside(prepared.root, guarded)) {
            check.verdict = ProtectionVerdict::ProtectedRoot;
            check.root = guarded;
            return check;
        }
    }
    check.verdict = ProtectionVerdict::Allowed;
    return check;
}

// ---------------------------------------------------------------------------
// Файловая система
// ---------------------------------------------------------------------------

[[nodiscard]] bool tryAttributes(const std::wstring& canonical, DWORD& attributes, DWORD& win32Error) {
    const DWORD value = ::GetFileAttributesW(canonical.c_str());
    if (value == INVALID_FILE_ATTRIBUTES) {
        win32Error = ::GetLastError();
        return false;
    }
    attributes = value;
    win32Error = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] bool isNotFound(DWORD win32Error) noexcept {
    return win32Error == ERROR_FILE_NOT_FOUND || win32Error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] bool isReparseAttribute(DWORD attributes) noexcept {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Канонический путь по дескриптору. Открытие идёт без FILE_FLAG_OPEN_REPARSE_POINT
// намеренно: нужна цель ссылки, а не сама ссылка. FILE_FLAG_BACKUP_SEMANTICS
// обязателен для каталогов.
[[nodiscard]] bool tryFinalPath(const std::wstring& canonical, std::wstring& out, DWORD& win32Error) {
    constexpr DWORD access = FILE_READ_ATTRIBUTES;
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const ScopedHandle handle(::CreateFileW(canonical.c_str(), access, share, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) {
        win32Error = ::GetLastError();
        return false;
    }
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    std::wstring buffer(kExtendedPathBufferChars, L'\0');
    DWORD written = ::GetFinalPathNameByHandleW(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (written == 0) {
        win32Error = ::GetLastError();
        return false;
    }
    if (static_cast<std::size_t>(written) >= buffer.size()) {
        buffer.resize(static_cast<std::size_t>(written) + 1, L'\0');
        written = ::GetFinalPathNameByHandleW(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), flags);
        if (written == 0) {
            win32Error = ::GetLastError();
            return false;
        }
        if (static_cast<std::size_t>(written) >= buffer.size()) {
            win32Error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
    }
    buffer.resize(static_cast<std::size_t>(written));
    out = std::move(buffer);
    win32Error = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] std::optional<FileStamp> tryFileStamp(const std::wstring& canonical, DWORD& win32Error) {
    constexpr DWORD access = FILE_READ_ATTRIBUTES;
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const ScopedHandle handle(::CreateFileW(canonical.c_str(), access, share, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) {
        win32Error = ::GetLastError();
        return std::nullopt;
    }
    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
        win32Error = ::GetLastError();
        return std::nullopt;
    }
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        win32Error = ::GetLastError();
        return std::nullopt;
    }
    FileStamp stamp;
    stamp.sizeBytes = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    stamp.lastWriteTicks = static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
    stamp.changeTicks = static_cast<std::uint64_t>(basic.ChangeTime.QuadPart);
    win32Error = ERROR_SUCCESS;
    return stamp;
}

[[nodiscard]] std::chrono::milliseconds backoffFor(const PreparedOptions& prepared, std::uint32_t retryIndex) noexcept {
    if (prepared.backoff.count() <= 0) return std::chrono::milliseconds(0);
    std::chrono::milliseconds delay = prepared.backoff;
    for (std::uint32_t index = 0; index < retryIndex; ++index) {
        if (delay >= prepared.maxBackoff) return prepared.maxBackoff;
        delay *= 2;
    }
    if (delay > prepared.maxBackoff) return prepared.maxBackoff;
    return delay;
}

// Ожидание паузы с проверкой отмены. Списки sleep_for, а не один вызов: иначе
// отмена во время backoff ждала бы до двух секунд после нажатия «Отмена».
[[nodiscard]] bool waitBackoff(std::chrono::milliseconds delay, const std::stop_token& stop) {
    auto left = delay;
    while (left.count() > 0) {
        if (stop.stop_requested()) return false;
        const std::chrono::milliseconds slice = (left < kBackoffSlice) ? left : kBackoffSlice;
        std::this_thread::sleep_for(slice);
        left -= slice;
    }
    return !stop.stop_requested();
}

// ---------------------------------------------------------------------------
// Журнал
// ---------------------------------------------------------------------------

// Поля собираются явно, а не макросом MRP_LOG_*: core::detail::logFieldList
// разворачивает пакет в один вызов logField со всеми аргументами, и на MSVC
// такой вызов не разрешается (C2661) — то же, что зафиксировано в devices.cpp и
// storage_query.cpp. Чужий заголовок не правим.
[[nodiscard]] core::LogFields deleteFields(const DeleteResult& result) {
    core::LogFields fields;
    fields.push_back(core::logField("status", toString(result.status)));
    fields.push_back(core::logField("attempts", static_cast<long long>(result.attempts)));
    fields.push_back(core::logField("waitedMs", static_cast<long long>(result.waited.count())));
    if (!result.detail.empty()) fields.push_back(core::logField("detail", result.detail));
    return fields;
}

void logOutcome(const DeleteResult& result) {
    // Успех не пишем: на дереве в 500 тысяч файлов это пол-мегабайта лога на
    // ровно то, что §5 просит не делать. Отказ и пропуск — всегда (§12).
    if (result.ok()) return;
    if (isSkipped(result.status)) {
        core::LogFields fields = deleteFields(result);
        fields.push_back(core::logField("path", toUtf8Path(result.path)));
        core::logWarn("platform.vfs_delete.entry", "операция удаления пропущена", std::move(fields));
        return;
    }
    core::logFailure("platform.vfs_delete.entry", "удаление не выполнено", toUtf8Path(result.path),
                     static_cast<std::int64_t>(result.hr), deleteFields(result));
}

[[nodiscard]] DeleteResult refuse(DeleteResult result, DeleteStatus status, DWORD win32Code,
                                  std::wstring_view detail) {
    result.status = status;
    result.hr = static_cast<std::int32_t>(platform::hresultFromWin32(win32Code));
    result.detail.assign(detail.data(), detail.size());
    logOutcome(result);
    return result;
}

// ---------------------------------------------------------------------------
// Удаление
// ---------------------------------------------------------------------------

[[nodiscard]] DeleteResult deleteCanonical(const std::wstring& canonical, std::wstring_view reportPath,
                                          const std::optional<FileStamp>& expected, const PreparedOptions& prepared,
                                          const std::stop_token& stop) {
    DeleteResult result;
    result.path.assign(reportPath.data(), reportPath.size());

    const ProtectionCheck check = checkProtectedCanonical(canonical, prepared);
    if (!check.allowed()) {
        return refuse(std::move(result), statusFor(check.verdict), policyCode(check.verdict),
                      reasonFor(check.verdict, check.root));
    }

    if (stop.stop_requested()) {
        return refuse(std::move(result), DeleteStatus::SkippedCancelled, ERROR_CANCELLED,
                      L"отмена до попытки удаления");
    }

    DWORD attributes = 0;
    DWORD win32Error = ERROR_SUCCESS;
    if (!tryAttributes(canonical, attributes, win32Error)) {
        if (isNotFound(win32Error)) {
            return refuse(std::move(result), DeleteStatus::AlreadyGone, ERROR_SUCCESS, L"объект уже отсутствует");
        }
        return refuse(std::move(result), DeleteStatus::Failed, win32Error, L"не удалось прочитать атрибуты объекта");
    }
    if (isReparseAttribute(attributes)) {
        return refuse(std::move(result), DeleteStatus::SkippedReparse, ERROR_ACCESS_DENIED,
                      L"reparse point (symlink, junction, точка монтирования): FR-6 — не раскрываем и не удаляем");
    }
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // Настоящий адрес объекта. Без него лексическая проверка корня правила
    // обходится junction'ом внутри разрешённого каталога.
    std::wstring finalPath;
    if (!tryFinalPath(canonical, finalPath, win32Error)) {
        return refuse(std::move(result), DeleteStatus::Failed, win32Error,
                      L"не удалось получить канонический путь по дескриптору: удаление запрещено");
    }
    const ProtectionCheck finalCheck = checkProtectedCanonical(finalPath, prepared);
    if (!finalCheck.allowed()) {
        return refuse(std::move(result), statusFor(finalCheck.verdict), policyCode(finalCheck.verdict),
                      reasonFor(finalCheck.verdict, finalCheck.root));
    }

    const std::uint32_t attempts = prepared.maxAttempts == 0 ? 1U : prepared.maxAttempts;
    for (;;) {
        if (stop.stop_requested()) {
            return refuse(std::move(result), DeleteStatus::SkippedCancelled, ERROR_CANCELLED, L"отмена перед попыткой");
        }
        if (expected) {
            DWORD stampError = ERROR_SUCCESS;
            const std::optional<FileStamp> current = tryFileStamp(canonical, stampError);
            if (!current) {
                if (isNotFound(stampError)) {
                    return refuse(std::move(result), DeleteStatus::AlreadyGone, ERROR_SUCCESS,
                                  L"объект исчез до удаления");
                }
                return refuse(std::move(result), DeleteStatus::Failed, stampError,
                              L"не удалось прочитать состояние файла для проверки «не изменился»");
            }
            if (!current->sameAs(*expected)) {
                return refuse(std::move(result), DeleteStatus::SkippedChanged, ERROR_WRITE_PROTECT,
                              L"файл изменился после сканирования: §10 требует проверить Version/LastWrite");
            }
        }

        ++result.attempts;
        const BOOL removed = directory ? ::RemoveDirectoryW(canonical.c_str()) : ::DeleteFileW(canonical.c_str());
        if (removed != FALSE) {
            result.status = DeleteStatus::Deleted;
            return result;
        }

        const DWORD code = ::GetLastError();
        if (isNotFound(code)) {
            return refuse(std::move(result), DeleteStatus::AlreadyGone, ERROR_SUCCESS, L"объект уже отсутствует");
        }
        if (isRetryableWin32(static_cast<std::int32_t>(code))) {
            if (result.attempts < attempts) {
                const std::chrono::milliseconds delay = backoffFor(prepared, result.attempts - 1);
                if (!waitBackoff(delay, stop)) {
                    return refuse(std::move(result), DeleteStatus::SkippedCancelled, ERROR_CANCELLED,
                                  L"отмена во время паузы перед повтором");
                }
                result.waited += delay;
                continue;
            }
            return refuse(std::move(result), DeleteStatus::SkippedBusy, code,
                          L"объект занят другим процессом после всех попыток (FR-5: Skip (locked))");
        }
        return refuse(std::move(result), DeleteStatus::Failed, code, directory ? L"RemoveDirectoryW" : L"DeleteFileW");
    }
}

void account(TreeDeleteSummary& summary, const DeleteResult& result, bool directory, const PreparedOptions& prepared,
             const DeleteEntryCallback& onEntry) {
    switch (result.status) {
        case DeleteStatus::Deleted:
            if (directory) {
                ++summary.dirsDeleted;
            } else {
                ++summary.filesDeleted;
            }
            break;
        case DeleteStatus::AlreadyGone:
            ++summary.alreadyGone;
            break;
        default:
            if (isSkipped(result.status)) {
                if (directory) {
                    ++summary.dirsSkipped;
                } else {
                    ++summary.filesSkipped;
                }
            } else {
                ++summary.failed;
            }
            if (summary.problems.size() < prepared.maxReportedProblems) {
                summary.problems.push_back(result);
            } else {
                ++summary.problemsDropped;
            }
            break;
    }
    if (onEntry) onEntry(result);
}

}  // namespace

// ---------------------------------------------------------------------------
// Публичный контракт
// ---------------------------------------------------------------------------

const char* toString(DeleteKind kind) noexcept {
    switch (kind) {
        case DeleteKind::File:
            return "file";
        case DeleteKind::Directory:
            return "directory";
        case DeleteKind::Unknown:
            break;
    }
    return "unknown";
}

const char* toString(DeleteStatus status) noexcept {
    switch (status) {
        case DeleteStatus::Deleted:
            return "deleted";
        case DeleteStatus::AlreadyGone:
            return "already-gone";
        case DeleteStatus::SkippedBusy:
            return "skipped-busy";
        case DeleteStatus::SkippedProtected:
            return "skipped-protected";
        case DeleteStatus::SkippedOutsideRoot:
            return "skipped-outside-root";
        case DeleteStatus::SkippedReparse:
            return "skipped-reparse";
        case DeleteStatus::SkippedChanged:
            return "skipped-changed";
        case DeleteStatus::SkippedCancelled:
            return "skipped-cancelled";
        case DeleteStatus::SkippedInvalid:
            return "skipped-invalid";
        case DeleteStatus::Failed:
            break;
    }
    return "failed";
}

const char* toString(ProtectionVerdict verdict) noexcept {
    switch (verdict) {
        case ProtectionVerdict::Allowed:
            return "allowed";
        case ProtectionVerdict::InvalidPath:
            return "invalid-path";
        case ProtectionVerdict::OutsideRuleRoot:
            return "outside-rule-root";
        case ProtectionVerdict::NeverAllowed:
            return "never-allowed";
        case ProtectionVerdict::ProtectedRoot:
            break;
    }
    return "protected-root";
}

bool isRemoved(DeleteStatus status) noexcept {
    return status == DeleteStatus::Deleted || status == DeleteStatus::AlreadyGone;
}

bool isSkipped(DeleteStatus status) noexcept {
    switch (status) {
        case DeleteStatus::SkippedBusy:
        case DeleteStatus::SkippedProtected:
        case DeleteStatus::SkippedOutsideRoot:
        case DeleteStatus::SkippedReparse:
        case DeleteStatus::SkippedChanged:
        case DeleteStatus::SkippedCancelled:
        case DeleteStatus::SkippedInvalid:
            return true;
        case DeleteStatus::Deleted:
        case DeleteStatus::AlreadyGone:
        case DeleteStatus::Failed:
            break;
    }
    return false;
}

const std::vector<std::wstring>& neverDeleteRoots() {
    return rootLists().never;
}

const std::vector<std::wstring>& protectedRoots() {
    return rootLists().guarded;
}

ProtectionCheck checkProtected(std::wstring_view path, const DeleteOptions& options) {
    const PreparedOptions prepared = prepareOptions(options);
    const std::wstring canonical = canonicalizePath(path);
    if (canonical.empty()) {
        ProtectionCheck check;
        check.verdict = prepared.rootValid ? ProtectionVerdict::InvalidPath : ProtectionVerdict::OutsideRuleRoot;
        return check;
    }
    return checkProtectedCanonical(canonical, prepared);
}

bool isProtectedPath(std::wstring_view path) {
    // Корень правила здесь не нужен: вопрос «этот путь защищён сам по себе».
    // Непригодный путь тоже считается неразрешённым — иначе вызывающий получил бы
    // «можно удалять» для пустой строки.
    DeleteOptions options;
    options.allowedRoot = path;
    return !checkProtected(path, options).allowed();
}

std::optional<FileStamp> readFileStamp(std::wstring_view path) {
    const std::wstring canonical = canonicalizePath(path);
    if (canonical.empty()) return std::nullopt;
    DWORD win32Error = ERROR_SUCCESS;
    return tryFileStamp(canonical, win32Error);
}

bool isRetryableWin32(std::int32_t win32Error) noexcept {
    if (win32Error == 0) return false;
    DWORD code = ERROR_SUCCESS;
    // Принимаем и код Win32, и обёрнутый HRESULT: вызывающий может держать в
    // руках именно HRESULT из DeleteResult.
    if (platform::tryWin32Code(static_cast<HRESULT>(win32Error), code) == false) {
        code = static_cast<DWORD>(win32Error);
    }
    // Только блокировки. ERROR_ACCESS_DENIED сюда не входит намеренно: это вердикт
    // о правах, повтор его не изменит — он лишь отложит строку отчёта.
    return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
}

std::string toUtf8Path(std::wstring_view path) {
    return platform::toUtf8(path);
}

DeleteResult deleteEntry(const DeleteRequest& request, const DeleteOptions& options, std::stop_token stop) {
    const PreparedOptions prepared = prepareOptions(options);
    const std::wstring canonical = canonicalizePath(request.path);
    if (canonical.empty()) {
        DeleteResult result;
        result.path = request.path;
        return refuse(std::move(result), DeleteStatus::SkippedInvalid, ERROR_INVALID_PARAMETER,
                      L"путь пуст, относительный или указывает в пространство имён устройств");
    }
    // request.kind — подсказка, а не источник истины: тип берётся из атрибутов
    // объекта, потому что атрибуты относятся к тому, что действительно лежит по
    // пути. Подсказка нужна вызывающему, когда объект уже исчез и выбирать нечего.
    return deleteCanonical(canonical, request.path, request.expectedStamp, prepared, stop);
}

TreeDeleteSummary deleteTree(std::wstring_view path, const DeleteOptions& options, std::stop_token stop,
                             const DeleteEntryCallback& onEntry) {
    TreeDeleteSummary summary;
    const PreparedOptions prepared = prepareOptions(options);
    const std::wstring root = canonicalizePath(path);
    if (root.empty()) {
        DeleteResult result;
        result.path.assign(path.data(), path.size());
        account(summary,
                refuse(std::move(result), DeleteStatus::SkippedInvalid, ERROR_INVALID_PARAMETER,
                       L"путь пуст, относительный или указывает в пространство имён устройств"),
                false, prepared, onEntry);
        return summary;
    }
    const ProtectionCheck check = checkProtectedCanonical(root, prepared);
    if (!check.allowed()) {
        DeleteResult result;
        result.path.assign(path.data(), path.size());
        account(summary,
                refuse(std::move(result), statusFor(check.verdict), policyCode(check.verdict),
                       reasonFor(check.verdict, check.root)),
                false, prepared, onEntry);
        return summary;
    }

    DWORD attributes = 0;
    DWORD win32Error = ERROR_SUCCESS;
    if (!tryAttributes(root, attributes, win32Error)) {
        DeleteResult result;
        result.path.assign(path.data(), path.size());
        if (isNotFound(win32Error)) {
            account(summary,
                    refuse(std::move(result), DeleteStatus::AlreadyGone, ERROR_SUCCESS, L"объект уже отсутствует"),
                    false, prepared, onEntry);
        } else {
            account(summary,
                    refuse(std::move(result), DeleteStatus::Failed, win32Error,
                           L"не удалось прочитать атрибуты объекта"),
                    false, prepared, onEntry);
        }
        return summary;
    }
    if (isReparseAttribute(attributes)) {
        DeleteResult result;
        result.path.assign(path.data(), path.size());
        account(summary,
                refuse(std::move(result), DeleteStatus::SkippedReparse, ERROR_ACCESS_DENIED,
                       L"reparse point (symlink, junction, точка монтирования): FR-6 — не раскрываем и не удаляем"),
                false, prepared, onEntry);
        return summary;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        // Корень оказался файлом: дерева нет, удаляем сам файл.
        account(summary, deleteCanonical(root, path, std::nullopt, prepared, stop), false, prepared, onEntry);
        return summary;
    }

    // Стек обхода и порядок удаления каталогов. Оба вектора растут до числа
    // каталогов, а не файлов: 500 тысяч файлов в 2000 каталогах дают 2000 строк
    // пути, а не полмиллиона (§5: рабочая память в пределах 150 МБ).
    std::vector<std::wstring> pending{root};
    std::vector<std::wstring> stack{root};
    std::uint32_t seen = 0;
    while (!stack.empty()) {
        if (stop.stop_requested()) {
            summary.cancelled = true;
            break;
        }
        const std::wstring directory = std::move(stack.back());
        stack.pop_back();

        // Маска перечисления склеивается вручную: путь уже канонизирован, а
        // FindFirstFileW требует NUL-строку.
        std::wstring pattern;
        pattern.reserve(directory.size() + 2);
        pattern.append(directory);
        pattern.push_back(L'\\');
        pattern.push_back(L'*');

        WIN32_FIND_DATAW entry{};
        const ScopedFind finder(::FindFirstFileW(pattern.c_str(), &entry));
        if (!finder) {
            const DWORD code = ::GetLastError();
            if (isNotFound(code)) continue;  // пустой каталог — норма, а не отказ
            DeleteResult result;
            result.path = directory;
            account(summary,
                    refuse(std::move(result), DeleteStatus::Failed, code, L"не удалось перечислить каталог"), true,
                    prepared, onEntry);
            continue;
        }
        for (;;) {
            if (seen % kDeleteCancelCheckPeriod == 0 && stop.stop_requested()) {
                summary.cancelled = true;
                break;
            }
            ++seen;

            // Первая запись приходит из FindFirstFileW, дальше — из
            // FindNextFileW: цикл обрабатывает текущую и запрашивает следующую,
            // иначе первый элемент каталога молча пропускался бы.
            const std::wstring_view name(entry.cFileName);
            // «.» и «..» — не элементы дерева. Здесь не continue: следующая
            // запись берётся из FindNextFileW в конце итерации, и continue
            // зациклил бы перечисление на одной и той же записи.
            if (name != L"." && name != L"..") {
                // Имя из FindFirstFile не содержит разделителей, поэтому склейка с
                // канонизированным путём даёт канонизированный путь и повторная
                // нормализация не нужна.
                std::wstring child;
                child.reserve(directory.size() + name.size() + 1);
                child.append(directory);
                child.push_back(L'\\');
                child.append(name);
                if (isReparseAttribute(entry.dwFileAttributes)) {
                    DeleteResult result;
                    result.path = child;
                    account(summary,
                            refuse(std::move(result), DeleteStatus::SkippedReparse, ERROR_ACCESS_DENIED,
                                   L"reparse point внутри дерева: FR-6 — не раскрываем и не удаляем"),
                            (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, prepared, onEntry);
                } else if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    pending.push_back(child);
                    stack.push_back(std::move(child));
                } else {
                    account(summary, deleteCanonical(child, child, std::nullopt, prepared, stop), false, prepared,
                            onEntry);
                }
            }

            if (::FindNextFileW(finder.get(), &entry) == FALSE) break;
        }
        if (summary.cancelled) break;
    }

    // Каталоги снизу вверх: pending пополняется родителем раньше детей, поэтому
    // обратный порядок — это дети раньше родителей. Иначе RemoveDirectoryW
    // возвращал бы ERROR_DIR_NOT_EMPTY и каталог остался бы в дереве.
    if (!summary.cancelled) {
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            if (stop.stop_requested()) {
                summary.cancelled = true;
                break;
            }
            account(summary, deleteCanonical(*it, *it, std::nullopt, prepared, stop), true, prepared, onEntry);
        }
    }

    core::LogFields fields;
    fields.push_back(core::logField("path", toUtf8Path(path)));
    fields.push_back(core::logField("filesDeleted", static_cast<long long>(summary.filesDeleted)));
    fields.push_back(core::logField("dirsDeleted", static_cast<long long>(summary.dirsDeleted)));
    fields.push_back(core::logField("alreadyGone", static_cast<long long>(summary.alreadyGone)));
    fields.push_back(core::logField("skipped", static_cast<long long>(summary.skipped())));
    fields.push_back(core::logField("failed", static_cast<long long>(summary.failed)));
    fields.push_back(core::logField("problemsDropped", static_cast<long long>(summary.problemsDropped)));
    fields.push_back(core::logField("cancelled", summary.cancelled));
    fields.push_back(core::logField("complete", summary.complete()));
    core::logInfo("platform.vfs_delete.tree", "дерево обработано", std::move(fields));
    return summary;
}

}  // namespace mrproper::platform::vfs
