// Реализация клиента обновления набора правил: WinHTTP-транспорт, конвейер
// «скачать → проверить → применить атомарно», журнал применения и откат.
// Спека: §9.2 (поток обновления, офлайн-режим и сброс), §9.1 ADR-008, §5, §6.1–6.4,
// §12. Контракт, раскладка каталогов, границы модуля и политика подписи описаны
// в rulesync_client.hpp; здесь только код.
//
// Порядок чтения файла: утилиты пути и файлов (ниже) → транспорт (WinHTTP) →
// применение и восстановление (журнал) → чтение состояния и набора с диска →
// конвейер Client.
//
// Слой Win32 (SPEC §6.1, ADR-004): единственное место проекта, где допустим
// windows.h. Наружу выходят только std::string в UTF-8, целые числа, перечисления
// и core::RuleSet — ни одного типа Win32 в объявлениях rulesync_client.hpp,
// поэтому транспорт можно подменить и прогнать конвейер без сети.
#include "rulesync_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/log.hpp"
#include "win_error.hpp"

namespace mrproper::platform::rulesync {
namespace {

// ---------------------------------------------------------------------------
// Пределы и мелочи формата
// ---------------------------------------------------------------------------

// Буфер чтения ответа: 64 КиБ — крупный блок, при котором счёт вызовов WinHTTP
// на файл правил не растёт, но память остаётся предсказуемой.
constexpr DWORD kReadChunkBytes = 64u * 1024u;

// Типовой путь Windows плюс запас на длинные профили. Значения длиннее этого
// бюджета считаем «переменной окружения нет»: %LOCALAPPDATA% — это путь внутри
// профиля пользователя, и на 4 КиБ он укладывается с большим запасом.
constexpr DWORD kEnvironmentPathChars = 4096;

// Границы URL, который этот клиент вообще готов обрабатывать. Без них
// WinHttpCrackUrl получает строку произвольной длины, а ответ — тем более.
constexpr std::size_t kMaxUrlChars = 2048;

// Часы: FILETIME в 100-наносекундных интервалах с 1601-01-01, до Unix-эпохи
// 11644473600 секунд. system clock, а не QPC: границы интервала в 24 часа
// (§9.2 п.1) и метки установки должны совпадать с показаниями часов пользователя
// и переживать перевод времени.
constexpr unsigned long long kFileTimeTicksPerSecond = 10000000ull;
constexpr unsigned long long kFileTimeToUnixSeconds = 11644473600ull;

[[nodiscard]] bool isAsciiSpace(char symbol) noexcept {
    return symbol == ' ' || symbol == '\t' || symbol == '\r' || symbol == '\n' || symbol == '\f' || symbol == '\v';
}

[[nodiscard]] std::string_view trimmedView(std::string_view text) noexcept {
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && isAsciiSpace(text[first])) ++first;
    while (last > first && isAsciiSpace(text[last - 1])) --last;
    return text.substr(first, last - first);
}

// Регистронезависимое сравнение ASCII: пути манифеста и файлы на диске живут в
// файловой системе, где регистр не учитывается, а в манифесте написан строчными.
[[nodiscard]] bool equalsIgnoreCaseAscii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const char a = left[i];
        const char b = right[i];
        const char lowerA = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
        const char lowerB = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
        if (lowerA != lowerB) return false;
    }
    return true;
}

// Монотонные миллисекунды для замера «сколько заняла проверка». QPC, а не часы:
// пользователь вправе перевести время посреди проверки, и тогда отрицательная
// длительность в журнале была бы нелепостью.
[[nodiscard]] std::int64_t monotonicMs() noexcept {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (::QueryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0) return 0;
    if (::QueryPerformanceCounter(&counter) == FALSE) return 0;
    return (counter.QuadPart * 1000LL) / frequency.QuadPart;
}

[[nodiscard]] bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    return equalsIgnoreCaseAscii(text.substr(0, prefix.size()), prefix);
}

// ---------------------------------------------------------------------------
// Файловая система
// ---------------------------------------------------------------------------

// Склейка пути: разделитель выбирается по виду основы, чтобы «C:\\a» + «b»
// стало «C:\\a\\b», а не «C:\\a/b». Своя функция вместо core::joinPath по
// существенной причине: joinPath живёт в core/trash.hpp, а в этом же
// namespace core::rulesync объявлен parseManifest для МАНИФЕСТА НАБОРА — и
// включать корзину ради одной склейки значит получить два разных parseManifest
// в одном пространстве имён.
[[nodiscard]] std::string joinPath(std::string_view base, std::string_view leaf) {
    if (base.empty()) return std::string(leaf);
    if (leaf.empty()) return std::string(base);
    std::string head(base);
    const char last = head.back();
    if (last == '/' || last == '\\') return head + std::string(leaf);
    // Windows-путь узнаём по обратному слэшу или по диску («C:»): разделитель в
    // нём обратный, иначе пришлось бы угадывать по умолчанию.
    const bool windowsStyle = head.find('\\') != std::string::npos || head.find(':') != std::string::npos;
    if (last == ':') return head + "\\" + std::string(leaf);
    return head + (windowsStyle ? "\\" : "/") + std::string(leaf);
}

[[nodiscard]] std::wstring wide(std::string_view path) {
    return toUtf16(path);
}

[[nodiscard]] bool fileAttributes(const std::wstring& path, DWORD& attributes) {
    const DWORD value = ::GetFileAttributesW(path.c_str());
    if (value == INVALID_FILE_ATTRIBUTES) return false;
    attributes = value;
    return true;
}

[[nodiscard]] bool pathExists(const std::wstring& path) {
    DWORD attributes = 0;
    return fileAttributes(path, attributes);
}

[[nodiscard]] bool isDirectory(const std::wstring& path) {
    DWORD attributes = 0;
    if (!fileAttributes(path, attributes)) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Снять «только чтение»: файл правил может прийти с этим атрибутом, иначе
// DeleteFileW вернёт ACCESS_DENIED и откат «не сможет удалить старый набор».
[[nodiscard]] bool clearReadOnly(const std::wstring& path) {
    DWORD attributes = 0;
    if (!fileAttributes(path, attributes)) return false;
    if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) return true;
    return ::SetFileAttributesW(path.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) != FALSE;
}

// Создать каталог и всех его предков. Отсутствие родителя — обычное дело для
// %LOCALAPPDATA%\MrProper\rules: каталога может не быть вообще, и его создание
// — часть работы клиента, а не ошибка.
[[nodiscard]] bool ensureDirectoryTree(const std::wstring& path, std::string& problem) {
    if (path.empty()) {
        problem = "путь каталога пуст";
        return false;
    }
    std::wstring partial;
    partial.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        const wchar_t symbol = path[i];
        if (symbol == L'\\' || symbol == L'/') {
            partial.push_back(symbol);
            // Корень диска («C:\») создавать нельзя, он уже есть.
            if (partial.size() >= 3u && partial[1] == L':') {
                if (!isDirectory(partial)) {
                    problem = "не найден каталог " + pathToUtf8(partial);
                    return false;
                }
                continue;
            }
            if (partial.size() <= 2u) continue;  // UNC-префикс и корень
            if (::CreateDirectoryW(partial.c_str(), nullptr) == FALSE) {
                // HRESULT снимается ДО следующего обращения к WinAPI: проверка
                // isDirectory зовёт GetFileAttributesW и затрёт GetLastError, а в
                // журнале нужен код именно от CreateDirectoryW (§12).
                const HRESULT hr = lastErrorHresult();
                if (hr != hresultFromWin32(ERROR_ALREADY_EXISTS) || !isDirectory(partial)) {
                    problem = hresultInfo(hr, "CreateDirectoryW", pathToUtf8(partial)).toString();
                    return false;
                }
            }
            continue;
        }
        partial.push_back(symbol);
    }
    if (::CreateDirectoryW(partial.c_str(), nullptr) == FALSE) {
        const HRESULT hr = lastErrorHresult();
        if (hr != hresultFromWin32(ERROR_ALREADY_EXISTS) || !isDirectory(partial)) {
            problem = hresultInfo(hr, "CreateDirectoryW", pathToUtf8(partial)).toString();
            return false;
        }
    }
    return true;
}

// Удаление дерева: содержимое снизу вверх, затем сам каталог. Каталог с
// атрибутом reparse point НЕ раскрывается (SPEC §5): он удаляется DeleteFileW
// как объект, а его цель остаётся нетронутой.
[[nodiscard]] bool removeTree(const std::wstring& path, std::string& problem) {
    if (!pathExists(path)) return true;
    if (!isDirectory(path)) {
        (void)clearReadOnly(path);
        if (::DeleteFileW(path.c_str()) == FALSE && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
            problem = lastErrorInfo("DeleteFileW", pathToUtf8(path)).toString();
            return false;
        }
        return true;
    }

    const std::wstring pattern = path + L"\\*";
    WIN32_FIND_DATAW entry{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name(entry.cFileName);
            if (name == L"." || name == L"..") continue;
            const std::wstring child = path + L"\\" + name;
            const bool childIsDir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool childIsReparse = (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (childIsDir && !childIsReparse) {
                if (!removeTree(child, problem)) {
                    ::FindClose(find);
                    return false;
                }
                continue;
            }
            (void)clearReadOnly(child);
            if (::DeleteFileW(child.c_str()) == FALSE && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
                problem = lastErrorInfo("DeleteFileW", pathToUtf8(child)).toString();
                ::FindClose(find);
                return false;
            }
        } while (::FindNextFileW(find, &entry) != FALSE);
        ::FindClose(find);
    }

    (void)clearReadOnly(path);
    if (::RemoveDirectoryW(path.c_str()) == FALSE) {
        problem = lastErrorInfo("RemoveDirectoryW", pathToUtf8(path)).toString();
        return false;
    }
    return true;
}

// Переименование каталога на месте. MOVEFILE_WRITE_THROUGH: переименование
// должно пережить падение питания, иначе журнал применения мог бы остаться с
// «current», которого уже нет. MOVEFILE_REPLACE_EXISTING здесь не нужен и
// вреден: он не умеет заменять непустой каталог, поэтому отказ выглядел бы
// зависанием. Целевой каталог вызывающий удаляет сам.
[[nodiscard]] bool renameInPlace(const std::wstring& from, const std::wstring& to, std::string& problem) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
        problem = lastErrorInfo("MoveFileExW", pathToUtf8(from)).toString();
        return false;
    }
    return true;
}

// Запись файла атомарно: временный файл рядом, FlushFileBuffers, затем
// переименование с REPLACE_EXISTING|WRITE_THROUGH. Прямая запись на месте после
// сбоя оставила бы обрезанный state.json, который на следующем запуске читается
// как «состояние есть, а какое — неизвестно».
[[nodiscard]] bool writeFileAtomic(const std::wstring& path, std::string_view data, std::string& problem) {
    const std::wstring temp = path + L".tmp";
    HANDLE handle = ::CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        problem = lastErrorInfo("CreateFileW", pathToUtf8(temp)).toString();
        return false;
    }

    std::size_t writtenTotal = 0;
    while (writtenTotal < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - writtenTotal, 64u * 1024u));
        DWORD written = 0;
        if (::WriteFile(handle, data.data() + writtenTotal, chunk, &written, nullptr) == FALSE || written == 0) {
            problem = lastErrorInfo("WriteFile", pathToUtf8(temp)).toString();
            (void)::CloseHandle(handle);
            (void)::DeleteFileW(temp.c_str());
            return false;
        }
        writtenTotal += written;
    }

    // Данные обязаны оказаться на диске до переименования.
    const BOOL flushed = ::FlushFileBuffers(handle);
    const DWORD flushError = ::GetLastError();
    (void)::CloseHandle(handle);
    if (flushed == FALSE) {
        problem = hresultInfo(hresultFromWin32(flushError), "FlushFileBuffers", pathToUtf8(temp)).toString();
        (void)::DeleteFileW(temp.c_str());
        return false;
    }

    if (::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        problem = lastErrorInfo("MoveFileExW", pathToUtf8(temp)).toString();
        (void)::DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] bool readFileLimited(const std::wstring& path, std::size_t maxBytes, std::string& out,
                                    std::string& problem) {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        problem = lastErrorInfo("CreateFileW", pathToUtf8(path)).toString();
        return false;
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle, &size) == FALSE) {
        problem = lastErrorInfo("GetFileSizeEx", pathToUtf8(path)).toString();
        (void)::CloseHandle(handle);
        return false;
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > static_cast<std::uint64_t>(maxBytes)) {
        problem = "файл " + pathToUtf8(path) + " больше потолка " + std::to_string(maxBytes) + " байт";
        (void)::CloseHandle(handle);
        return false;
    }
    out.clear();
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t readTotal = 0;
    while (readTotal < out.size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - readTotal, kReadChunkBytes));
        if (::ReadFile(handle, out.data() + readTotal, want, &read, nullptr) == FALSE) {
            problem = lastErrorInfo("ReadFile", pathToUtf8(path)).toString();
            (void)::CloseHandle(handle);
            return false;
        }
        if (read == 0) break;  // файл оказался короче, чем сообщил GetFileSizeEx
        readTotal += read;
    }
    (void)::CloseHandle(handle);
    out.resize(readTotal);
    return true;
}

[[nodiscard]] std::int64_t modifiedEpochSeconds(const std::wstring& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) return 0;
    ULARGE_INTEGER ticks{};
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
    if (ticks.QuadPart < kFileTimeToUnixSeconds * kFileTimeTicksPerSecond) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart / kFileTimeTicksPerSecond) - kFileTimeToUnixSeconds);
}

// Относительные пути всех файлов дерева, с '/' вместо '\' — ровно в том виде, в
// каком пути перечислены в манифесте. Нужно для проверки «файл вне манифеста»:
// скачиваются только перечисленные, но остаться в staging может что-то, чего
// там быть не должно, и молчаливый остаток — это набор, который движок прочтёт
// целиком и который подпись не покрывает. Префикс prefix накапливает подкаталоги:
// без него «sub\a.json» сравнился бы с манифестным «a.json» и дал бы ложный отказ.
[[nodiscard]] bool listFilesRelativeIn(const std::wstring& root, std::string_view prefix,
                                       std::vector<std::string>& out, std::size_t limit) {
    const std::wstring pattern = root + L"\\*";
    WIN32_FIND_DATAW entry{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return true;  // пустое дерево — не ошибка
    do {
        const std::wstring name(entry.cFileName);
        if (name == L"." || name == L"..") continue;
        const std::wstring child = root + L"\\" + name;
        const std::string relative = std::string(prefix) + pathToUtf8(name);
        const bool childIsDir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (childIsDir && (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            if (!listFilesRelativeIn(child, relative + "/", out, limit)) {
                ::FindClose(find);
                return false;
            }
            continue;
        }
        if (out.size() >= limit) {
            ::FindClose(find);
            return false;
        }
        std::string normalized = relative;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        out.push_back(std::move(normalized));
    } while (::FindNextFileW(find, &entry) != FALSE);
    ::FindClose(find);
    return true;
}

[[nodiscard]] bool listFilesRelative(const std::wstring& root, std::vector<std::string>& out, std::size_t limit) {
    return listFilesRelativeIn(root, std::string_view{}, out, limit);
}

// Относительный путь из манифеста: та же проверка, что и в ядре
// (core::parseManifest), повторённая здесь. Ядро её делает при разборе, а путь
// к файлу собирает уже платформа — второй раз проверить дешевле, чем объяснять
// потом, откуда взялся «C:\Windows\System32\evil.json».
[[nodiscard]] bool isSafeManifestPath(std::string_view path) {
    if (path.empty() || path.size() > 200u) return false;
    if (path.front() == '/' || path.front() == '\\') return false;
    if (path.find_first_of("<>:\"|?*") != std::string_view::npos) return false;
    for (const char symbol : path) {
        const auto byte = static_cast<unsigned char>(symbol);
        if (byte < 0x20u || byte == 0x7fu) return false;
    }
    std::string_view rest = path;
    while (true) {
        const std::size_t slash = rest.find_first_of("/\\");
        const std::string_view component = rest.substr(0, slash);
        if (component.empty()) return false;
        if (component == "." || component == "..") return false;
        if (component.back() == ' ' || component.back() == '.') return false;
        if (slash == std::string_view::npos) break;
        rest = rest.substr(slash + 1);
    }
    return true;
}

// Относительный путь набора → путь на диске. Разделитель '/' печатается в
// Win-пути как '\': манифест использует прямой слэш по всем платформам, а
// WinAPI ждёт привычный вид. Компоненты приводятся из UTF-8 в UTF-16 целиком:
// правила пишутся по-русски, и побайтовое расширение склеило бы негодную строку.
[[nodiscard]] std::wstring insideDirectory(const std::wstring& directory, std::string_view relative) {
    std::wstring path(directory);
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    std::string component;
    const auto flush = [&path, &component]() {
        if (component.empty()) return;
        path += toUtf16(component);
        component.clear();
    };
    for (const char symbol : relative) {
        if (symbol == '/' || symbol == '\\') {
            flush();
            path.push_back(L'\\');
            continue;
        }
        component.push_back(symbol);
    }
    flush();
    return path;
}

// ---------------------------------------------------------------------------
// URL
// ---------------------------------------------------------------------------

// Только HTTPS. Не «потом проверим ответ», а до запроса: правила удаления не
// должны ехать открытым текстом даже один раз (SPEC §9.2 п.1).
[[nodiscard]] bool isHttpsUrl(std::string_view url) {
    return startsWithIgnoreCase(url, "https://") && url.size() > 8u;
}

// Соседний адрес в каталоге манифеста: подпись лежит рядом с манифестом, файлы
// набора — там же (tools/rule_keys.md §1: набор версионируется одним
// каталогом). Относительный путь из манифеста подставляется как есть: прямой
// слэш в URL допустим, а манифест использует его и для подкаталогов.
[[nodiscard]] std::string urlSibling(std::string_view manifestUrl, std::string_view name) {
    const std::size_t slash = manifestUrl.find_last_of('/');
    if (slash == std::string_view::npos || slash + 1u >= manifestUrl.size()) return {};
    return std::string(manifestUrl.substr(0, slash + 1u)) + std::string(name);
}

// ---------------------------------------------------------------------------
// base64: строгий разбор
// ---------------------------------------------------------------------------

// Строгий разбор base64. expectedBytes == 0 означает «любая длина» (этим
// пользуется публичная функция decodeBase64), иначе результат обязан быть
// ровно заданной длины: подпись — 64 байта, публичный ключ — 32, любая другая
// длина означает «подписи нет».
[[nodiscard]] bool decodeInto(std::string_view text, std::size_t expectedBytes, std::vector<std::uint8_t>& out) {
    const std::string_view body = trimmedView(text);
    if (body.empty()) return false;

    auto valueOf = [](char symbol) -> int {
        if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
        if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
        if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
        if (symbol == '+') return 62;
        if (symbol == '/') return 63;
        return -1;
    };

    out.clear();
    out.reserve(expectedBytes);
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t padding = 0;
    for (const char symbol : body) {
        if (symbol == '=') {
            ++padding;
            if (padding > 2u) return false;
            continue;
        }
        if (padding > 0u) return false;  // выравнивание не в конце
        const int value = valueOf(symbol);
        if (value < 0) return false;  // пробел внутри строки подписи — не подпись
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (expectedBytes != 0u && out.size() >= expectedBytes) return false;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    if (bits >= 6) return false;                 // хвост из «лишней» sextet-группы
    if (bits > 0 && ((buffer & ((1u << bits) - 1u)) != 0u)) return false;  // небитые биты хвоста
    if (expectedBytes == 0u) return true;
    return out.size() == expectedBytes;
}

}  // namespace

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------

bool decodeBase64(std::string_view text, std::vector<std::uint8_t>& out) {
    out.clear();
    return decodeInto(text, 0u, out);
}

std::optional<RawSignature> decodeSignature(std::string_view base64) {
    std::vector<std::uint8_t> bytes;
    if (!decodeInto(base64, kSignatureBytes, bytes)) return std::nullopt;
    RawSignature signature{};
    std::copy(bytes.begin(), bytes.end(), signature.begin());
    return signature;
}

std::optional<PublicKey> decodePublicKey(std::string_view base64) {
    std::vector<std::uint8_t> bytes;
    if (!decodeInto(base64, kPublicKeyBytes, bytes)) return std::nullopt;
    PublicKey key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::string toBase64(const std::uint8_t* data, std::size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2u) / 3u) * 4u);
    std::size_t i = 0;
    while (i + 2u < size) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1u]) << 8) |
            static_cast<std::uint32_t>(data[i + 2u]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3Fu]);
        out.push_back(kAlphabet[triple & 0x3Fu]);
        i += 3u;
    }
    if (i + 1u == size) {
        const std::uint32_t triple = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2u == size) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[i]) << 16) |
                                     (static_cast<std::uint32_t>(data[i + 1u]) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

// ---------------------------------------------------------------------------
// Раскладка
// ---------------------------------------------------------------------------

bool Layout::valid() const noexcept {
    return !root.empty() && !current.empty() && !previous.empty() && !staging.empty() && !stateFile.empty() &&
           !journalFile.empty();
}

Layout Layout::under(std::string_view rulesRoot) {
    Layout layout;
    if (rulesRoot.empty()) return layout;
    layout.root = std::string(rulesRoot);
    layout.current = joinPath(layout.root, kActiveDirName);
    layout.previous = joinPath(layout.root, kPreviousDirName);
    layout.staging = joinPath(layout.root, kStagingDirName);
    layout.stateFile = joinPath(layout.root, kStateFileName);
    layout.journalFile = joinPath(layout.root, kJournalFileName);
    return layout;
}

std::string defaultRuleSetRoot() {
    // GetEnvironmentVariableW, а не SHGetKnownFolderPath: последний живёт в
    // shell32, которой в списке библиотек слоя нет, а тянуть новую библиотеку
    // ради одного вызова — значит править CMakeLists чужого слоя. Тот же
    // довод, что в vfs_trash.
    std::wstring buffer(kEnvironmentPathChars, L'\0');
    const DWORD length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), kEnvironmentPathChars);
    if (length == 0 || length >= kEnvironmentPathChars) return {};
    buffer.resize(length);
    const std::string localAppData = toUtf8(buffer);
    if (localAppData.empty()) return {};
    return joinPath(joinPath(localAppData, kAppDirName), kRuleSetDirName);
}

bool defaultLayout(Layout& out) {
    const std::string root = defaultRuleSetRoot();
    if (root.empty()) return false;
    out = Layout::under(root);
    return out.valid();
}

// ---------------------------------------------------------------------------
// Проверка подписи
// ---------------------------------------------------------------------------

bool SignaturePolicy::isZeroKey() const noexcept {
    for (const std::uint8_t byte : publicKey) {
        if (byte != 0) return false;
    }
    return true;
}

core::SignatureVerifier SignaturePolicy::toCoreVerifier() const {
    if (!available()) return {};
    const SignatureVerify function = verify;
    // Ядро передаёт байты подписи как есть, декодировать — дело платформы
    // (tools/rule_keys.md §1: в файле подпись лежит в base64).
    return [function](std::string_view manifestBytes, std::string_view signatureBytes) {
        const std::optional<RawSignature> signature = decodeSignature(signatureBytes);
        if (!signature) return false;
        return function(manifestBytes, *signature);
    };
}

// ---------------------------------------------------------------------------
// Время и окружение
// ---------------------------------------------------------------------------

std::int64_t nowEpochSeconds() noexcept {
    FILETIME stamp{};
    ::GetSystemTimeAsFileTime(&stamp);
    const unsigned long long ticks = (static_cast<unsigned long long>(stamp.dwHighDateTime) << 32) |
                                     static_cast<unsigned long long>(stamp.dwLowDateTime);
    if (ticks < kFileTimeToUnixSeconds * kFileTimeTicksPerSecond) return 0;
    return static_cast<std::int64_t>((ticks / kFileTimeTicksPerSecond) - kFileTimeToUnixSeconds);
}

std::string environmentDump() {
    LPWCH block = ::GetEnvironmentStringsW();
    if (block == nullptr) return {};
    std::string out;
    for (const wchar_t* cursor = block; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1u) {
        const std::wstring_view entry(cursor);
        if (entry.empty() || entry.front() == L'=') continue;  // скрытые переменные вида «=C:=C:\»
        const std::size_t equals = entry.find(L'=');
        if (equals == std::wstring_view::npos || equals == 0) continue;
        out += toUtf8(entry.substr(0, equals));
        out += '=';
        out += toUtf8(entry.substr(equals + 1u));
        out += '\n';
    }
    ::FreeEnvironmentStringsW(block);
    return out;
}

// ---------------------------------------------------------------------------
// Транспорт: WinHTTP
// ---------------------------------------------------------------------------

namespace {

// Владение HINTERNET. WinHttpCloseHandle — это CloseHandle, но называть его
// WinAPI-именем честнее, чем кастовать дескриптор в HANDLE ради чужой RAII.
class WinHttpCloser {
public:
    WinHttpCloser() = default;
    explicit WinHttpCloser(HINTERNET handle)
        : handle_(handle) {
    }
    ~WinHttpCloser() {
        if (handle_ != nullptr) ::WinHttpCloseHandle(handle_);
    }
    WinHttpCloser(const WinHttpCloser&) = delete;
    WinHttpCloser& operator=(const WinHttpCloser&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept {
        return handle_;
    }

private:
    HINTERNET handle_{};
};

class WinHttpTransport final : public Transport {
public:
    WinHttpTransport(std::wstring userAgent, bool useSystemProxy)
        : userAgent_(std::move(userAgent)) {
        const DWORD accessType = useSystemProxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
        session_ = ::WinHttpOpen(userAgent_.c_str(), accessType, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                 WINHTTP_FLAG_SECURE);
        if (session_ == nullptr) return;
        // Редиректы не следуются и являются отказом. Пин тега (SPEC §9.2 п.1)
        // держится на адресе: ответ «302 на другой хост» — это ровно тот вектор,
        // от которого пин и защищает, а тихо следовать ему нельзя.
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        (void)::WinHttpSetOption(session_, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    }

    [[nodiscard]] HttpResponse get(const HttpRequest& request) override {
        if (session_ == nullptr) return HttpResponse::failure("WinHttpOpen: сессия не создана");
        if (request.url.empty() || request.url.size() > kMaxUrlChars) {
            return HttpResponse::failure("URL пуст или длиннее " + std::to_string(kMaxUrlChars) + " символов");
        }
        if (!isHttpsUrl(request.url)) {
            return HttpResponse::failure("запрошен не https URL: " + request.url);
        }
        if (request.maxBytes == 0) return HttpResponse::failure("нулевой потолок размера ответа");

        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        wchar_t extra[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
        const std::wstring url = wide(request.url);
        if (::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) == FALSE) {
            return HttpResponse::failure(lastErrorInfo("WinHttpCrackUrl", request.url).toString());
        }
        const std::wstring hostName(host, parts.dwHostNameLength);
        const std::wstring urlPath(path, parts.dwUrlPathLength);
        const std::wstring extraInfo(extra, parts.dwExtraInfoLength);
        const std::wstring objectPath = urlPath.empty() ? std::wstring(L"/") : urlPath + extraInfo;
        if (hostName.empty() || objectPath.empty()) {
            return HttpResponse::failure("в URL нет узла или пути: " + request.url);
        }

        // Таймауты на все четыре фазы: без них «таймаут 10 с» из SPEC §9.2 п.1
        // означал бы только ожидание ответа, а не всю операцию. Миллисекунды
        // передаются четырьмя int, поэтому приведение с ограничением сверху.
        const int timeout = static_cast<int>(std::clamp<std::int64_t>(request.timeoutMs, 100, 60000));
        if (::WinHttpSetTimeouts(session_, timeout, timeout, timeout, timeout) == FALSE) {
            return HttpResponse::failure(lastErrorInfo("WinHttpSetTimeouts", request.url).toString());
        }

        WinHttpCloser connection(::WinHttpConnect(session_, hostName.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (connection.get() == nullptr) {
            return HttpResponse::failure(lastErrorInfo("WinHttpConnect", request.url).toString());
        }
        // Только GET (SPEC §9.2 п.1), без Referer и без WinHttpSetCredentials:
        // авторизации на сервере правил нет, и её не должно даже быть возможности.
        // Седьмой-параметрный pwszVersion между именем объекта и Referrer:
        // nullptr — HTTP/1.1 по умолчанию.
        WinHttpCloser httpRequest(::WinHttpOpenRequest(connection.get(), L"GET", objectPath.c_str(), nullptr,
                                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       WINHTTP_FLAG_SECURE));
        if (httpRequest.get() == nullptr) {
            return HttpResponse::failure(lastErrorInfo("WinHttpOpenRequest", request.url).toString());
        }
        if (::WinHttpSendRequest(httpRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                                 0) == FALSE) {
            return HttpResponse::failure(lastErrorInfo("WinHttpSendRequest", request.url).toString());
        }
        if (::WinHttpReceiveResponse(httpRequest.get(), nullptr) == FALSE) {
            return HttpResponse::failure(lastErrorInfo("WinHttpReceiveResponse", request.url).toString());
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (::WinHttpQueryHeaders(httpRequest.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                  WINHTTP_NO_HEADER_INDEX) == FALSE) {
            return HttpResponse::failure(lastErrorInfo("WinHttpQueryHeaders", request.url).toString());
        }
        if (statusCode != 200) {
            return HttpResponse::failure("HTTP " + std::to_string(statusCode) + " на " + request.url);
        }

        std::string body;
        body.reserve(4096u);
        std::array<std::uint8_t, kReadChunkBytes> buffer;  // заполняется WinHttpReadData
        while (true) {
            DWORD available = 0;
            if (::WinHttpQueryDataAvailable(httpRequest.get(), &available) == FALSE) {
                return HttpResponse::failure(lastErrorInfo("WinHttpQueryDataAvailable", request.url).toString());
            }
            if (available == 0) break;
            DWORD read = 0;
            const DWORD want = std::min(available, static_cast<DWORD>(std::size(buffer)));
            if (::WinHttpReadData(httpRequest.get(), buffer.data(), want, &read) == FALSE) {
                return HttpResponse::failure(lastErrorInfo("WinHttpReadData", request.url).toString());
            }
            if (read == 0) break;
            if (body.size() + read > request.maxBytes) {
                return HttpResponse::failure("ответ больше потолка " + std::to_string(request.maxBytes) + " байт на " +
                                            request.url);
            }
            body.append(reinterpret_cast<const char*>(buffer.data()), read);
        }
        return HttpResponse::success(static_cast<long>(statusCode), std::move(body));
    }

private:
    std::wstring userAgent_;
    HINTERNET session_{};
};

}  // namespace

std::unique_ptr<Transport> makeWinHttpTransport(std::wstring_view userAgent, bool useSystemProxy) {
    return std::make_unique<WinHttpTransport>(std::wstring(userAgent), useSystemProxy);
}

HttpResponse HttpResponse::failure(std::string reason) {
    HttpResponse response;
    response.ok = false;
    response.status = 0;
    response.error = std::move(reason);
    return response;
}

HttpResponse HttpResponse::success(long statusCode, std::string payload) {
    HttpResponse response;
    response.ok = true;
    response.status = statusCode;
    response.body = std::move(payload);
    return response;
}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

core::RuleSetStatus readStatus(const Layout& layout) {
    if (!layout.valid()) return core::RuleSetStatus{};
    std::string text;
    std::string problem;
    if (!readFileLimited(wide(layout.stateFile), core::kMaxManifestBytes, text, problem)) {
        // Отсутствующий или битый state.json — не ошибка: приложение обязано
        // стартовать на встроенном наборе (SPEC §9.2 п.4), а не падать.
        return core::RuleSetStatus{};
    }
    return core::parseRuleSetStatus(text);
}

bool writeStatus(const Layout& layout, const core::RuleSetStatus& status, std::string& problem) {
    if (!layout.valid()) {
        problem = "раскладка каталогов не задана";
        return false;
    }
    const std::wstring root = wide(layout.root);
    if (!ensureDirectoryTree(root, problem)) return false;
    return writeFileAtomic(wide(layout.stateFile), core::serializeRuleSetStatus(status), problem);
}

// ---------------------------------------------------------------------------
// Набор с диска
// ---------------------------------------------------------------------------

bool readInstalledSet(const Layout& layout, InstalledSet& out, std::string& problem) {
    out = InstalledSet{};
    if (!layout.valid()) {
        problem = "раскладка каталогов не задана";
        return false;
    }
    const std::wstring current = wide(layout.current);
    if (!isDirectory(current)) {
        problem = "рабочего набора нет: " + layout.current;
        return false;
    }

    std::string manifestBytes;
    if (!readFileLimited(insideDirectory(current, kManifestFileName), core::kMaxManifestBytes, manifestBytes,
                        problem)) {
        return false;
    }
    out.manifestBytes = std::move(manifestBytes);

    std::string signatureBytes;
    std::string signatureProblem;
    if (readFileLimited(insideDirectory(current, kSignatureFileName), kDefaultMaxSignatureBytes, signatureBytes,
                        signatureProblem)) {
        out.signatureBytes = std::move(signatureBytes);
        out.signaturePresent = true;
    } else {
        // Набор без подписи непригоден к применению, но прочитать его можно:
        // вердикт «подпись не проверена» выносит проверка, а не чтение.
        out.signatureBytes.clear();
        out.signaturePresent = false;
    }

    core::Manifest manifest;
    try {
        manifest = core::parseManifest(out.manifestBytes, kManifestFileName);
    } catch (const std::exception& error) {
        problem = error.what();
        return false;
    }
    out.version = manifest.version;

    std::size_t totalBytes = 0;
    for (const core::ManifestFile& entry : manifest.files) {
        if (!isSafeManifestPath(entry.path)) {
            problem = "недопустимый путь в манифесте: " + entry.path;
            return false;
        }
        std::string content;
        std::string readProblem;
        if (!readFileLimited(insideDirectory(current, entry.path), kDefaultMaxRuleFileBytes, content, readProblem)) {
            problem = "файл набора не прочитан: " + readProblem;
            return false;
        }
        totalBytes += content.size();
        if (totalBytes > kDefaultMaxSetBytes) {
            problem = "набор на диске больше потолка " + std::to_string(kDefaultMaxSetBytes) + " байт";
            return false;
        }
        out.files.push_back(core::makeVerifiedFile(entry.path, content));
        out.contents.emplace_back(entry.path, std::move(content));
    }
    out.modifiedEpochSeconds = modifiedEpochSeconds(insideDirectory(current, kManifestFileName));
    return true;
}

// ---------------------------------------------------------------------------
// Журнал применения, восстановление, откат, сброс
// ---------------------------------------------------------------------------

namespace {

// Журнал пишется ДО первого переименования и снимается ПОСЛЕ последнего. Его
// наличие означает «применение началось и не закончилось».
[[nodiscard]] std::string makeJournalText(std::string_view version, std::int64_t stamp) {
    const json::Value document = json::Value::object({
        {"version", json::Value(std::string(version))},
        {"started", json::Value(static_cast<double>(stamp))},
    });
    return document.dump(2);
}

[[nodiscard]] std::string readJournalVersion(const std::string& text) {
    try {
        const json::Value document = json::parse(text);
        if (!document.isObject()) return {};
        const json::Value* version = document.find("version");
        if (version == nullptr || !version->isString()) return {};
        return version->asString();
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace

RecoveryReport recoverInterruptedApply(const Layout& layout) {
    RecoveryReport report;
    if (!layout.valid()) return report;
    const std::wstring journal = wide(layout.journalFile);
    if (!pathExists(journal)) return report;
    report.journalFound = true;

    std::string text;
    std::string problem;
    if (readFileLimited(journal, core::kMaxManifestBytes, text, problem)) {
        report.version = readJournalVersion(text);
    }

    const std::wstring current = wide(layout.current);
    const std::wstring previous = wide(layout.previous);
    const std::wstring staging = wide(layout.staging);

    if (!isDirectory(current) && isDirectory(previous)) {
        // Самое частое: упали между «current → previous» и «staging → current».
        // Возвращаем на место последний рабочий набор — приложение снова имеет
        // правила, и статус при этом остаётся прежним.
        if (renameInPlace(previous, current, problem)) {
            report.restored = true;
            report.detail = "рабочий набор " + (report.version.empty() ? std::string("(без версии)") : report.version) +
                            " восстановлен из " + layout.previous;
        } else {
            report.detail = "восстановление не удалось: " + problem;
        }
    } else {
        report.detail = "прерванное применение найдено, рабочий набор на месте — откатываться нечего";
    }

    if (pathExists(staging)) {
        std::string stagingProblem;
        if (removeTree(staging, stagingProblem)) {
            report.stagingDropped = true;
        } else {
            report.detail += "; незавершённый кандидат не удалён: " + stagingProblem;
        }
    }
    (void)::DeleteFileW(journal.c_str());

    if (report.restored) {
        core::logWarn("rulesync.recovered", "восстановлено прерванное применение набора",
                      core::LogFields{core::logField("version", report.version)});
    } else {
        core::logWarn("rulesync.recovered", "разобран журнал прерванного применения",
                      core::LogFields{core::logField("version", report.version)});
    }
    return report;
}

bool rollbackToLastGood(const Layout& layout, const core::RuleSetStatus& current, std::string& problem) {
    if (!layout.valid()) {
        problem = "раскладка каталогов не задана";
        return false;
    }
    const std::wstring currentDir = wide(layout.current);
    const std::wstring previousDir = wide(layout.previous);
    if (!isDirectory(previousDir)) {
        problem = "откатываться не на что: предыдущего рабочего набора нет (" + layout.previous + ")";
        return false;
    }

    // Откат — это те же атомарные переименования, что и применение, только в
    // обратную сторону. Сначала освобождаем current, иначе MoveFileEx не сможет
    // переименовать в непустой каталог.
    std::string swapProblem;
    if (pathExists(currentDir) && !removeTree(currentDir, swapProblem)) {
        problem = "текущий набор не удалён перед откатом: " + swapProblem;
        return false;
    }
    if (!renameInPlace(previousDir, currentDir, problem)) return false;

    // Кандидат, который не удалось применить, больше не нужен.
    std::string stagingProblem;
    if (pathExists(wide(layout.staging))) (void)removeTree(wide(layout.staging), stagingProblem);

    core::RuleSetStatus next = current;
    next.embedded = false;
    // Набор в previous — по построению тот, который уже проверялся перед
    // применением (попасть в current иначе нельзя), поэтому он подтверждён.
    next.verified = true;
    next.version = current.verifiedVersion;
    next.lastResult = "откат на набор " + current.verifiedVersion;
    std::string statusProblem;
    (void)writeStatus(layout, next, statusProblem);
    core::logWarn("rulesync.rollback", "выполнен откат на предыдущий набор",
                  core::LogFields{core::logField("version", next.version)});
    return true;
}

bool resetToEmbeddedSet(const Layout& layout, std::string& problem) {
    if (!layout.valid()) {
        problem = "раскладка каталогов не задана";
        return false;
    }
    bool ok = true;
    for (const std::string& directory : {layout.current, layout.previous, layout.staging}) {
        std::string removeProblem;
        if (!removeTree(wide(directory), removeProblem)) {
            problem = "каталог не удалён: " + removeProblem;
            ok = false;
        }
    }
    if (pathExists(wide(layout.journalFile))) (void)::DeleteFileW(wide(layout.journalFile).c_str());

    const core::RuleSetStatus next = core::resetToEmbeddedSet(readStatus(layout), nowEpochSeconds());
    std::string statusProblem;
    if (!writeStatus(layout, next, statusProblem)) {
        if (problem.empty()) problem = statusProblem;
        ok = false;
    }
    MRP_LOG_INFO("rulesync.reset", "возвращён встроенный набор правил");
    return ok;
}

bool setAutoUpdateEnabled(const Layout& layout, bool enabled, std::string& problem) {
    core::RuleSetStatus status = readStatus(layout);
    status.autoUpdateEnabled = enabled;
    if (!writeStatus(layout, status, problem)) return false;
    core::logInfo("rulesync.autoupdate", "автообновление набора правил изменено",
                 core::LogFields{core::logField("enabled", enabled)});
    return true;
}

// ---------------------------------------------------------------------------
// Итог проверки
// ---------------------------------------------------------------------------

std::string UpdateReport::summary() const {
    if (!problems.empty()) {
        std::string text = problems.front();
        if (problems.size() > 1u) text += " (ещё " + std::to_string(problems.size() - 1u) + ")";
        return text;
    }
    if (applied) return "набор " + version + " применён, файлов " + std::to_string(filesApplied);
    if (skippedByInterval) return "проверка не выполнялась: с прошлой прошло меньше суток";
    if (skippedAutoUpdateOff) return "автообновление выключено, работаем на текущем наборе";
    if (recovered) return "восстановлено прерванное применение, проверка не выполнялась";
    return "набор не изменился";
}

// ---------------------------------------------------------------------------
// Конвейер
// ---------------------------------------------------------------------------

Client::Client(UpdateConfig config, Layout layout, std::unique_ptr<Transport> transport,
               std::function<std::int64_t()> clock)
    : config_(std::move(config)),
      layout_(std::move(layout)),
      transport_(std::move(transport)),
      clock_(std::move(clock)) {
    if (!clock_) clock_ = nowEpochSeconds;
}

Client::~Client() = default;

Client::Client(Client&&) noexcept = default;

Client& Client::operator=(Client&&) noexcept = default;

namespace {

// Один GET с учётом общего дедлайна проверки. Отдельная функция, потому что
// дедлайн применяется к каждому запросу одинаково, а проверка их сама не
// различает.
[[nodiscard]] bool fetchText(Transport& transport, const std::string& url, std::size_t maxBytes,
                             std::int64_t requestTimeoutMs, std::int64_t deadlineMs, std::string& out,
                             std::string& problem) {
    if (url.empty()) {
        problem = "не задан адрес набора";
        return false;
    }
    std::int64_t timeout = requestTimeoutMs;
    const std::int64_t remaining = deadlineMs - monotonicMs();
    if (remaining <= 0) {
        problem = "истекло общее время проверки обновления";
        return false;
    }
    if (remaining < timeout) timeout = remaining;

    HttpRequest request;
    request.url = url;
    request.maxBytes = maxBytes;
    request.timeoutMs = timeout;
    const HttpResponse response = transport.get(request);
    if (!response.ok) {
        problem = response.error;
        return false;
    }
    out = response.body;
    return true;
}

// Проверка подписи манифеста до скачивания файлов. SPEC §9.2 п.2 требует
// проверять подпись раньше хешей, а п.3 — не скачивать непроверенное: сначала
// 24 файла ради набора, у которого подпись не сойдётся, были бы и сетью, и
// диском заняты без пользы.
[[nodiscard]] bool checkSignature(const UpdateConfig& config, std::string_view manifestBytes,
                                  const std::string& signatureText, std::string& problem) {
    const std::optional<RawSignature> signature = decodeSignature(signatureText);
    if (!signature) {
        problem = "подпись не прочитана: ожидаются " + std::to_string(kSignatureBytes) + " байт в base64";
        return false;
    }
    if (!config.signature.available()) {
        problem = "проверка подписи недоступна: не задан публичный ключ или верификатор (ADR-008)";
        return false;
    }
    if (!config.signature.verify(manifestBytes, *signature)) {
        problem = "подпись манифеста не прошла проверку";
        return false;
    }
    return true;
}

// Проверка minAppVersion до сети. Не-числовая компонента — отказ, а не «ноль»:
// это осознанное решение автора правил, и тихо проигнорировать его нельзя.
[[nodiscard]] bool checkAppVersion(const UpdateConfig& config, const core::Manifest& manifest, std::string& problem) {
    if (config.appVersion.empty()) {
        problem = "не задана версия приложения — minAppVersion проверить не на что";
        return false;
    }
    try {
        if (core::isAppVersionSupported(manifest.minAppVersion, config.appVersion)) return true;
    } catch (const std::exception& error) {
        problem = std::string("minAppVersion не разобран: ") + error.what();
        return false;
    }
    problem = "набор требует приложение не ниже " + manifest.minAppVersion + ", текущая версия " + config.appVersion;
    return false;
}

// Записать итог проверки и вернуть его вызывающему. Отдельная функция, а не
// лямбда внутри runCheck: справедливость — политика ядра (что делать при отказе),
// а текст результата — наш.
void finishReport(UpdateReport& report, const Layout& layout, core::RuleSetStatus& status, std::int64_t now,
                  std::int64_t startedMs) {
    report.elapsedMs = monotonicMs() - startedMs;
    // Политика «что делаем при отказе» — ядро (SPEC §9.2 п.4: встроенный набор,
    // если не подтверждён ни один). Текст результата — наш: упавшая сеть и битый
    // манифест — разные причины, и «манифест не разобран» в поле «результат
    // последней проверки» только сбивает с толку.
    status = core::withFailedCheck(status, report.verification, now);
    status.lastResult = report.summary();
    std::string statusProblem;
    (void)writeStatus(layout, status, statusProblem);
    report.version = status.version;
}

}  // namespace

UpdateReport Client::checkAndApply(bool force) {
    return runCheck(force);
}

UpdateReport Client::runCheck(bool force) {
    UpdateReport report;
    const std::int64_t startedMs = monotonicMs();
    const std::int64_t now = clock_();

    // Шаг 0. Прерванное применение восстанавливается ДО всего остального: если
    // процесс упал между переименованиями, рабочего набора сейчас нет, и любая
    // другая работа с каталогами была бы поверх битого состояния.
    const RecoveryReport recovery = recoverInterruptedApply(layout_);
    report.recovered = recovery.restored;

    core::RuleSetStatus status = readStatus(layout_);
    report.version = status.version;

    if (!layout_.valid()) {
        report.problems.push_back("раскладка каталогов не задана — проверить нечего");
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    if (!transport_) {
        report.problems.push_back("транспорт не задан");
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    if (!force) {
        if (!status.autoUpdateEnabled) {
            report.skippedAutoUpdateOff = true;
            report.elapsedMs = monotonicMs() - startedMs;
            return report;
        }
        if (!status.shouldCheck(now)) {
            report.skippedByInterval = true;
            report.elapsedMs = monotonicMs() - startedMs;
            return report;
        }
    }
    if (config_.manifestUrl.empty()) {
        report.problems.push_back("не задан адрес манифеста набора");
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    if (!config_.signature.available()) {
        // Отказ ДО сети: незачем отдавать трафик набору, который всё равно не
        // будет применён, и незачем ждать таймаут на канале без причины.
        report.problems.push_back("проверка подписи недоступна: не задан публичный ключ или верификатор (ADR-008)");
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    report.checked = true;

    const std::int64_t deadline = startedMs + config_.overallTimeoutMs;
    const std::wstring root = wide(layout_.root);
    const std::wstring staging = wide(layout_.staging);
    std::string problem;

    // Шаг 1 §9.2: staging каждый раз с нуля. Остатки прошлой попытки — это
    // файлы, которых нет в новом манифесте, а «файл вне манифеста» в правилах
    // удаления означает удаление по неподписанному правилу.
    if (!ensureDirectoryTree(root, problem) || !removeTree(staging, problem) ||
        !ensureDirectoryTree(staging, problem)) {
        report.problems.push_back("каталог кандидата не подготовлен: " + problem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }

    // Шаг 2 §9.2 п.1: манифест и подпись.
    std::string manifestBytes;
    if (!fetchText(*transport_, config_.manifestUrl, config_.maxManifestBytes, config_.requestTimeoutMs, deadline,
                   manifestBytes, problem)) {
        report.problems.push_back("манифест не скачан: " + problem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    const std::string signatureUrl =
        config_.signatureUrl.empty() ? urlSibling(config_.manifestUrl, kSignatureFileName) : config_.signatureUrl;
    std::string signatureText;
    if (!fetchText(*transport_, signatureUrl, config_.maxSignatureBytes, config_.requestTimeoutMs, deadline,
                   signatureText, problem)) {
        report.problems.push_back("подпись не скачана: " + problem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }

    // Шаг 3 §9.2 п.2: схема манифеста, minAppVersion, подпись. В этом порядке
    // и не иначе: подпись доказывает происхождение манифеста, а хеши — только
    // целостность уже подписанного.
    core::Manifest manifest;
    try {
        manifest = core::parseManifest(manifestBytes, kManifestFileName);
    } catch (const std::exception& error) {
        report.problems.push_back(std::string("манифест не разобран: ") + error.what());
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    report.candidateVersion = manifest.version;
    if (!checkAppVersion(config_, manifest, problem)) {
        report.problems.push_back(problem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    if (!checkSignature(config_, manifestBytes, signatureText, problem)) {
        report.problems.push_back(problem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }

    // Шаг 4 §9.2 п.3: файлы набора в staging. Тело ответа Transport уже держит в
    // памяти (потолок maxRuleFileBytes на файл и maxSetBytes на набор), на диск
    // пишется кусками, хеш считается по ходу.
    std::vector<core::VerifiedFile> verified;
    std::vector<std::pair<std::string, std::string>> contents;
    verified.reserve(manifest.files.size());
    contents.reserve(manifest.files.size());
    std::size_t totalBytes = manifestBytes.size() + signatureText.size();
    for (const core::ManifestFile& entry : manifest.files) {
        if (!isSafeManifestPath(entry.path)) {
            report.problems.push_back("недопустимый путь в манифесте: " + entry.path);
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }
        const std::string fileUrl = urlSibling(config_.manifestUrl, entry.path);
        std::string body;
        if (fileUrl.empty() ||
            !fetchText(*transport_, fileUrl, config_.maxRuleFileBytes, config_.requestTimeoutMs, deadline, body,
                       problem)) {
            report.problems.push_back("файл \"" + entry.path + "\" не скачан: " + problem);
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }
        totalBytes += body.size();
        if (totalBytes > config_.maxSetBytes) {
            report.problems.push_back("набор больше потолка " + std::to_string(config_.maxSetBytes) + " байт");
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }
        if (entry.sizeDeclared && body.size() != entry.size) {
            report.problems.push_back("файл \"" + entry.path + "\": размер " + std::to_string(body.size()) +
                                      " байт, в манифесте " + std::to_string(entry.size));
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }

        const std::wstring destination = insideDirectory(staging, entry.path);
        const std::size_t slash = destination.find_last_of(L'\\');
        if (slash != std::wstring::npos && !ensureDirectoryTree(destination.substr(0, slash), problem)) {
            report.problems.push_back("каталог файла не создан: " + problem);
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }
        if (!writeFileAtomic(destination, body, problem)) {
            report.problems.push_back("файл \"" + entry.path + "\" не записан в кандидат: " + problem);
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }
        verified.push_back(core::makeVerifiedFile(entry.path, body));
        contents.emplace_back(entry.path, std::move(body));
    }

    // Манифест и подпись кладём рядом с файлами: без них рабочий набор не
    // перепроверяется на следующем запуске, а перепроверка на старте обязательна
    // (ADR-008: то, что лежит на диске, ничем не отличается от подменённого).
    if (!writeFileAtomic(insideDirectory(staging, kManifestFileName), manifestBytes, problem) ||
        !writeFileAtomic(insideDirectory(staging, kSignatureFileName), signatureText, problem)) {
        report.problems.push_back("манифест или подпись не сохранены в кандидат: " + problem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }

    // Файл вне манифеста — отказ: его никто не подписывал, а удалять по нему
    // будут. Исключение одно: манифест и подпись — они и есть подписанная пара.
    std::vector<std::string> onDisk;
    if (!listFilesRelative(staging, onDisk, core::kMaxManifestFiles + 2u)) {
        report.problems.push_back("в кандидате больше файлов, чем допустимо");
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    for (const std::string& relative : onDisk) {
        if (relative == kManifestFileName || relative == kSignatureFileName) continue;
        if (manifest.byPath(relative) == nullptr) {
            report.problems.push_back("файл \"" + relative + "\" не перечислен в манифесте — набор не применяется");
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }
    }

    // Шаг 5 §9.2 п.2-3: вердикт ядра (схема → версия → подпись → SHA-256 и
    // размер каждого файла) и разбор правил парсером.
    core::VerificationPolicy policy;
    policy.verifySignature = config_.signature.toCoreVerifier();
    policy.requireSignature = true;
    policy.requireKnownFilesOnly = true;
    report.verification = core::verifyRuleSet(manifestBytes, signatureText, verified, config_.appVersion, policy);
    report.filesApplied = verified.size();
    report.bytesApplied = static_cast<std::int64_t>(totalBytes);
    if (!report.verification.ok()) {
        for (const std::string& text : report.verification.errors()) report.problems.push_back(text);
        std::string dropProblem;
        (void)removeTree(staging, dropProblem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }

    bool unresolved = false;
    try {
        const core::RuleSet parsed = core::loadVerifiedRuleSet(report.verification, contents, environmentDump(),
                                                               &unresolved);
        if (parsed.rules.empty()) {
            report.problems.push_back("набор не содержит ни одного правила");
            finishReport(report, layout_, status, now, startedMs);
            return report;
        }
    } catch (const std::exception& error) {
        // Неизвестное поле и битый JSON становятся ошибкой именно здесь: после
        // проверки целостности это уже не «сеть», а содержимое набора.
        report.problems.push_back(std::string("набор не разбирается парсером правил: ") + error.what());
        std::string dropProblem;
        (void)removeTree(staging, dropProblem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    if (unresolved) {
        // Не блокирует: неизвестная переменная окружения означает, что правило
        // ничего не найдёт, а не то, что оно опасно. Путь, не совпавший ни с
        // одним правилом, по SPEC §10 не удаляется никогда.
        MRP_LOG_WARN("rulesync.unresolved", "в наборе есть локаторы с неразрешёнными переменными окружения");
    }

    // Шаг 6 §9.2 п.3: атомарная подмена. Журнал → previous ← current → current
    // ← staging → журнал снят. Любой сбой посередине оставляет приложение с
    // целым набором, а следующий запуск доводит откат до конца.
    const std::wstring journal = wide(layout_.journalFile);
    const std::wstring currentDir = wide(layout_.current);
    const std::wstring previousDir = wide(layout_.previous);
    std::string journalProblem;
    if (!writeFileAtomic(journal, makeJournalText(manifest.version, now), journalProblem)) {
        report.problems.push_back("журнал применения не записан: " + journalProblem);
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }

    std::string applyProblem;
    if (pathExists(previousDir) && !removeTree(previousDir, applyProblem)) {
        report.problems.push_back("предыдущий набор не удалён перед подменой: " + applyProblem);
        (void)::DeleteFileW(journal.c_str());
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    const bool hadCurrent = isDirectory(currentDir);
    if (hadCurrent && !renameInPlace(currentDir, previousDir, applyProblem)) {
        report.problems.push_back("рабочий набор не убран в откат: " + applyProblem);
        (void)::DeleteFileW(journal.c_str());
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    if (!renameInPlace(staging, currentDir, applyProblem)) {
        // Откат вручную: без него каталога current просто не было бы.
        std::string undoProblem;
        if (hadCurrent) (void)renameInPlace(previousDir, currentDir, undoProblem);
        (void)::DeleteFileW(journal.c_str());
        report.problems.push_back("кандидат не подменён рабочим набором: " + applyProblem +
                                  (undoProblem.empty() ? std::string() : "; откат: " + undoProblem));
        finishReport(report, layout_, status, now, startedMs);
        return report;
    }
    (void)::DeleteFileW(journal.c_str());

    const core::RuleSetStatus applied = core::withAppliedCandidate(status, report.verification, now);
    std::string statusProblem;
    (void)writeStatus(layout_, applied, statusProblem);
    status = applied;
    report.applied = true;
    report.version = status.version;
    report.elapsedMs = monotonicMs() - startedMs;
    core::logInfo("rulesync.applied", "набор правил применён",
                 core::LogFields{core::logField("version", report.version),
                                 core::logField("files", report.filesApplied),
                                 core::logField("bytes", report.bytesApplied),
                                 core::logField("ms", report.elapsedMs)});
    return report;
}

bool Client::loadActiveRuleSet(const std::vector<std::pair<std::string, std::string>>& builtInFiles, core::RuleSet& out,
                               std::string& origin, std::string& problem) const {
    origin.clear();
    const std::string env = environmentDump();
    const core::RuleSetStatus status = readStatus(layout_);
    const core::StartupChoice choice = core::chooseStartupRuleSet(status);

    auto useEmbedded = [&out, &origin, &problem, &env, &builtInFiles]() {
        origin = "встроенный набор";
        try {
            out = core::loadRuleFiles(builtInFiles, env, nullptr);
        } catch (const std::exception& error) {
            problem = std::string("встроенный набор не читается: ") + error.what();
            return false;
        }
        return true;
    };

    if (choice.useEmbedded) {
        core::logInfo("rulesync.startup", "работаем на встроенном наборе",
                      core::LogFields{core::logField("reason", choice.reason)});
        return useEmbedded();
    }

    // То, что лежит на диске, перепроверяется целиком: подпись, minAppVersion,
    // SHA-256 каждого файла. Каталог %LOCALAPPDATA% доступен и постороннему
    // процессу, поэтому «проверено при применении» — не то же самое, что
    // «проверено сейчас». Не подтвердился рабочий набор — пробуем откат на
    // предыдущий (одна попытка, иначе возможен цикл), иначе встроенный.
    core::VerificationPolicy policy;
    policy.verifySignature = config_.signature.toCoreVerifier();
    policy.requireSignature = true;
    policy.requireKnownFilesOnly = true;

    bool rolledBack = false;
    std::string lastProblem;
    for (int attempt = 0; attempt < 2; ++attempt) {
        InstalledSet installed;
        std::string readProblem;
        if (!readInstalledSet(layout_, installed, readProblem)) {
            lastProblem = readProblem;
            break;
        }

        const core::RuleSetVerification verification =
            core::verifyRuleSet(installed.manifestBytes, installed.signatureBytes, installed.files, config_.appVersion,
                                policy);
        if (!verification.ok()) {
            for (const std::string& text : verification.errors()) {
                core::logError("rulesync.startup", "рабочий набор не подтверждён",
                               core::LogFields{core::logField("problem", text)});
            }
            lastProblem = verification.summary();
            if (rolledBack) break;
            std::string rollbackProblem;
            if (rollbackToLastGood(layout_, readStatus(layout_), rollbackProblem)) {
                core::logWarn("rulesync.startup", "выполнен откат на предыдущий набор",
                              core::LogFields{core::logField("version", installed.version)});
                rolledBack = true;
                continue;
            }
            break;
        }

        try {
            out = core::loadVerifiedRuleSet(verification, installed.contents, env, nullptr);
        } catch (const std::exception& error) {
            lastProblem = std::string("рабочий набор не разбирается: ") + error.what();
            core::logError("rulesync.startup", "рабочий набор не разобран",
                           core::LogFields{core::logField("problem", lastProblem)});
            break;
        }
        origin = rolledBack ? ("откат на набор " + installed.version) : ("набор " + installed.version);
        return true;
    }

    // Ни подтверждённого набора, ни отката: работаем на встроенном — приложение
    // никогда не остаётся без правил (SPEC §9.2 п.4).
    if (useEmbedded()) return true;
    if (!lastProblem.empty()) problem += "; " + lastProblem;
    return false;
}

}  // namespace mrproper::platform::rulesync
