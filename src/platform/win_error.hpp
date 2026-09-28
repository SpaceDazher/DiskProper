// MrProper — ошибки Win32: HRESULT, системный текст и переводы UTF-8 ↔ UTF-16.
// Спека: §9.1 ADR-001 («изоляция WinAPI в platform/»), §5 и §12 («все ошибки в
// логе с путём и HRESULT»), §6.3 (пути и WinAPI — UTF-16, ядро — UTF-8).
//
// Заголовок самодостаточен: все функции помечены inline, поэтому модуль не
// требует .cpp и не добавляет символов в линковку. Живёт он в platform/,
// потому что это единственное место проекта, где <windows.h> разрешён
// (SPEC §6.2, ADR-004): ядро обязано собираться и проверяться без Windows.
//
// Модуль делает две вещи.
//
// 1. Приводит отказ к одному виду. WinAPI сообщает об ошибке двумя способами:
//    GetLastError() у «классических» функций и HRESULT у COM/WMI. Здесь оба
//    сворачиваются в WinErrorInfo{HRESULT, имя вызова, текст системы, путь} —
//    то, что можно записать в лог, показать в UI и бросить как исключение.
//    Текст берётся у самой системы (FormatMessageW), поэтому соответствует
//    языку установки, а не захардкоженному словарю: утилитой пользуется
//    русскоязычный пользователь, и «Access denied» рядом с путём в логе — это
//    половина диагностики.
//
// 2. Переводит UTF-8 ↔ UTF-16. Ядро оперирует UTF-8 (core::log, отчёты), пути
//    и WinAPI — UTF-16 (SPEC §6.3), поэтому любой вызов в слое проходит через
//    toUtf8/toUtf16. Свои переводы, а не core::log: текст ошибки нужен и UI,
//    где логгер может быть не инициализирован, и заголовку об ошибках не
//    следует тянуть за собой модуль журналирования.
//
// Чего модуль сознательно не делает. Ни одна функция не бросает исключение
// неявно: бросают только те, в имени которых это написано (throw…/check…),
// и выбирает их вызывающий — в горячем цикле обхода каталогов отказ это флаг,
// а не исключение (SPEC §6.4). Исключение, которого можно ждать от любой
// функции, одно: std::bad_alloc при выделении памяти.
#pragma once

#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mrproper::platform {

// Весь модуль написан в расчёте на 16-битный wchar_t: суррогатные пары,
// U+FFFD и приведение длины в int имеют смысл только здесь. Проверка не ради
// Windows (её задаёт сам <windows.h>), а ради того, чтобы перенос на другую
// платформу с 32-битным wchar_t не прошёл тихо.
static_assert(sizeof(wchar_t) == 2, "MrProper: слой Win32 предполагает 16-битный wchar_t");

// ---------------------------------------------------------------------------
// HRESULT и коды Win32
// ---------------------------------------------------------------------------

// HRESULT_FROM_WIN32 раскрывается в 0x80070000 | (код & 0xFFFF), поэтому по
// старшим битам видно, что это обёрнутый GetLastError(), а не «настоящий»
// HRESULT из COM/WMI. Различать их обязательно: FormatMessageW с
// FORMAT_MESSAGE_FROM_HRESULT на обёртке текста не даст.
inline constexpr std::uint32_t kWin32HresultFacility = 0x80070000u;

// Успех в HRESULT — это «неотрицательно» (как SUCCEEDED из WinNT.h), а не «ноль»:
// S_FALSE тоже успех.
[[nodiscard]] constexpr bool isSuccess(HRESULT hr) noexcept {
    return hr >= 0;
}

[[nodiscard]] constexpr bool isFailure(HRESULT hr) noexcept {
    return hr < 0;
}

[[nodiscard]] constexpr bool isWin32Hresult(HRESULT hr) noexcept {
    return (static_cast<std::uint32_t>(hr) & 0xFFFFF000u) == kWin32HresultFacility;
}

// HRESULT, которым WinAPI «обернул» код Win32. Ровно то, что SPEC §12 требует
// писать в лог рядом с путём.
[[nodiscard]] inline HRESULT hresultFromWin32(DWORD win32Code) noexcept {
    return HRESULT_FROM_WIN32(win32Code);
}

// Код Win32 внутри HRESULT_FROM_WIN32. У «настоящего» HRESULT подделки нет:
// false, и вызывающий работает с самим HRESULT.
[[nodiscard]] inline bool tryWin32Code(HRESULT hr, DWORD& code) noexcept {
    if (!isWin32Hresult(hr)) return false;
    code = static_cast<DWORD>(static_cast<std::uint32_t>(hr) & 0xFFFFu);
    return true;
}

// Снимок последней ошибки. Звать сразу после неудачного вызова WinAPI: любое
// другое обращение к API — даже успешное — способно затереть код, поэтому
// «посмотреть GetLastError() в catch на пятом экране» не работает никогда.
[[nodiscard]] inline HRESULT lastErrorHresult() noexcept {
    return hresultFromWin32(::GetLastError());
}

// ---------------------------------------------------------------------------
// Текст системного сообщения (UTF-16)
// ---------------------------------------------------------------------------

// Форматирование текста ошибки: FormatMessageW, обрезка краёв, освобождение
// блока. Помещено в detail, потому что это детали реализации, а не контракт:
// наружу выходят win32ErrorText/hresultErrorText.
namespace detail {

// Флаги FormatMessage, которых нет макросом в заголовках Windows SDK, а есть
// только в документации. FORMAT_MESSAGE_FROM_HRESULT имеет то же значение, что
// FORMAT_MESSAGE_FROM_SYSTEM (0x00001000), поэтому отдельного флага не нужно:
// «взять текст HRESULT» и «взять текст системного кода» — один и тот же бит.
inline constexpr DWORD kFormatMessageIgnoreDeactivate = 0x00020000u;

[[nodiscard]] inline bool isMessageSpace(wchar_t symbol) noexcept {
    return symbol == L' ' || symbol == L'\t' || symbol == L'\r' || symbol == L'\n';
}

// FormatMessageW щедро добавляет в конце CRLF (иногда два), а в начале —
// отступы. В логе и в UI это читается как мусор, поэтому текст подрезается.
[[nodiscard]] inline std::wstring_view trimMessage(std::wstring_view text) noexcept {
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && isMessageSpace(text[first])) ++first;
    while (last > first && isMessageSpace(text[last - 1])) --last;
    return text.substr(first, last - first);
}

// Общий шаг обоих FormatMessageW: выделить, обрезать, скопировать и освободить.
// При ALLOCATE_BUFFER API выдаёт указатель на блок в *ppBuffer — не освободить
// его значит течь на килобайты на каждой неудаче, поэтому LocalFree обязателен.
[[nodiscard]] inline std::wstring takeFormattedMessage(DWORD code, DWORD flags) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        ::FormatMessageW(flags, nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0,
                         nullptr);
    if (length == 0 || buffer == nullptr) return {};
    // Копия до LocalFree: наружу уходит std::wstring, а не view в чужой блок.
    const std::wstring text(trimMessage(std::wstring_view(buffer, length)));
    ::LocalFree(buffer);
    return text;
}

}  // namespace detail

// Текст Win32-кода в UTF-16. flags — набор FORMAT_MESSAGE_* для FormatMessageW;
// по умолчанию «системный текст, вставки не разворачивать». Пустая строка означает
// «система не знает такой код»: для кодов, смысл которых знает только
// приложение, это обычное дело, и выглядеть как сбой оно не должно.
[[nodiscard]] inline std::wstring formatSystemMessage(
    DWORD code,
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS) {
    return detail::takeFormattedMessage(code, flags);
}

// Текст HRESULT в UTF-16. Обёрнутый Win32-код разворачивается в текст кода,
// «настоящий» HRESULT читается системным текстом по самому значению (формат
// сообщения HRESULT — тот же бит FORMAT_MESSAGE_FROM_SYSTEM, см. detail выше).
[[nodiscard]] inline std::wstring formatHresultMessage(HRESULT hr) {
    DWORD win32Code = ERROR_SUCCESS;
    if (tryWin32Code(hr, win32Code)) return formatSystemMessage(win32Code);

    constexpr DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                             detail::kFormatMessageIgnoreDeactivate;
    return detail::takeFormattedMessage(static_cast<DWORD>(hr), flags);
}

// UTF-8-формы тех же текстов. Определения — ниже, после toUtf8.
[[nodiscard]] inline std::string win32ErrorText(DWORD code);

[[nodiscard]] inline std::string hresultErrorText(HRESULT hr);

// ---------------------------------------------------------------------------
// UTF-8 ↔ UTF-16
// ---------------------------------------------------------------------------

// Предел WinAPI: длина передаётся в int. Всё длиннее пришлось бы резать на
// куски, но одиночный путь или текст ошибки такой длины не встречается — и
// честный отказ лучше молчаливо обрезанного результата.
inline constexpr std::size_t kMaxTextLength = 0x7FFFFFFFu;

// Сколько единиц займёт результат: 0 при отказе WinAPI или слишком длинном
// входе. Пустой вход даёт 0 — вызывающий в этом случае просто ничего не
// дописывает.
[[nodiscard]] inline std::size_t utf16Size(std::string_view utf8) noexcept {
    if (utf8.empty() || utf8.size() > kMaxTextLength) return 0;
    const int units = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    return units > 0 ? static_cast<std::size_t>(units) : 0;
}

[[nodiscard]] inline std::size_t utf8Size(std::wstring_view utf16) noexcept {
    if (utf16.empty() || utf16.size() > kMaxTextLength) return 0;
    const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0,
                                           nullptr, nullptr);
    return bytes > 0 ? static_cast<std::size_t>(bytes) : 0;
}

// Строгие варианты: результат ДОПИСЫВАЕТСЯ в out (буфер не очищается) — одна
// функция обслуживает и одиночное преобразование, и горячий цикл обхода, где
// буфер заранее зарезервирован на utf16Size/utf8Size и на каждый элемент не
// выделяется новая строка. false означает «вход невалиден или WinAPI отказал»,
// и out при этом не изменён. Основа для проверки данных, пришедших из файла или
// сети: молча подменённая кодовая точка в имени файла хуже отказа.
[[nodiscard]] inline bool tryToUtf16(std::string_view utf8, std::wstring& out) {
    const std::size_t units = utf16Size(utf8);
    if (units == 0) return utf8.empty();
    const std::size_t saved = out.size();
    out.resize(saved + units);
    const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                              out.data() + saved, static_cast<int>(units));
    // Тот же вход и та же длина, что в utf16Size, поэтому отказ здесь — уже
    // не про кодовую страницу. Откатываем буфер, чтобы out не остался частично
    // заполненным нечитаемыми нулями.
    if (written <= 0) {
        out.resize(saved);
        return false;
    }
    out.resize(saved + static_cast<std::size_t>(written));
    return true;
}

[[nodiscard]] inline bool tryToUtf8(std::wstring_view utf16, std::string& out) {
    const std::size_t bytes = utf8Size(utf16);
    if (bytes == 0) return utf16.empty();
    const std::size_t saved = out.size();
    out.resize(saved + bytes);
    const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), static_cast<int>(utf16.size()),
                                              out.data() + saved, static_cast<int>(bytes), nullptr, nullptr);
    if (written <= 0) {
        out.resize(saved);
        return false;
    }
    out.resize(saved + static_cast<std::size_t>(written));
    return true;
}

// Мягкие варианты: всегда дают текст, негодные единицы заменяя на U+FFFD
// (в направлении UTF-16 → UTF-8 сама WinAPI без WC_ERR_INVALID_CHARS подставляет
// '?'). Для сообщения об ошибке и для подписи в UI потеря одной кодовой точки
// не важна, а исключение из логирования — важна.
[[nodiscard]] inline std::wstring toUtf16(std::string_view utf8) {
    std::wstring out;
    if (tryToUtf16(utf8, out)) return out;
    if (utf8.empty() || utf8.size() > kMaxTextLength) return out;
    // Строгий разбор не прошёл: повторяем без MB_ERR_INVALID_CHARS, и WinAPI
    // подставит U+FFFD вместо негодных байтов.
    const int units = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (units <= 0) return out;
    out.resize(static_cast<std::size_t>(units));
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), units);
    if (written <= 0) {
        out.clear();
        return out;
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

[[nodiscard]] inline std::string toUtf8(std::wstring_view utf16) {
    std::string out;
    if (tryToUtf8(utf16, out)) return out;
    if (utf16.empty() || utf16.size() > kMaxTextLength) return out;
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return out;
    out.resize(static_cast<std::size_t>(bytes));
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), bytes, nullptr,
                                              nullptr);
    if (written <= 0) {
        out.clear();
        return out;
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

// Перегрузки для C-строк: nullptr — это пустой текст, а не повод падать. Вызов
// через один wstring_view на nullptr был бы неопределённым поведением, а
// nullptr в слое Win32 встречается чаще, чем хотелось бы: неудачный
// GetModuleFileName, отсутствующий аргумент в обёртке, пустая строка из
// реестра.
[[nodiscard]] inline std::wstring toUtf16(const char* utf8) {
    return utf8 == nullptr ? std::wstring{} : toUtf16(std::string_view(utf8));
}

[[nodiscard]] inline std::string toUtf8(const wchar_t* utf16) {
    return utf16 == nullptr ? std::string{} : toUtf8(std::wstring_view(utf16));
}

// Проверки без единого выделения памяти. Пустая строка корректна.
[[nodiscard]] inline bool isValidUtf8(std::string_view utf8) noexcept {
    return utf8.empty() || utf16Size(utf8) > 0;
}

[[nodiscard]] inline bool isValidUtf16(std::wstring_view utf16) noexcept {
    return utf16.empty() || utf8Size(utf16) > 0;
}

// Текст ошибки в UTF-8 — форма, в которой его пишет лог (SPEC §5, §12).
// Успешный HRESULT тоже имеет осмысленный текст: «успех» вместо пустой строки,
// чтобы случайная запись в лог не выглядела как отказ.
[[nodiscard]] inline std::string win32ErrorText(DWORD code) {
    return toUtf8(formatSystemMessage(code));
}

[[nodiscard]] inline std::string hresultErrorText(HRESULT hr) {
    if (isSuccess(hr)) return "успех";
    return toUtf8(formatHresultMessage(hr));
}

// ---------------------------------------------------------------------------
// Ошибка как данные
// ---------------------------------------------------------------------------

// Диагностика одного отказа: HRESULT, вызов, путь и текст системы. Это то, что
// уходит в лог (SPEC §5, §12) и то, из чего строится исключение. Структура
// намеренно простая и копируемая: её читают и UI, и запись лога, и отчёт.
struct WinErrorInfo {
    HRESULT hr{E_FAIL};   // заглушка: настоящий код приходит из lastErrorInfo/hresultInfo
    std::string api;      // "CreateFileW" — имя вызова, без скобок и аргументов
    std::string text;     // текст системы в UTF-8, может быть пустым
    std::string path;     // путь в UTF-8, к которому относился вызов, может быть пустым

    // Однострочное представление для лога: вызов, текст, коды и путь. Вида
    // «CreateFileW: Не удается найти указанный файл. [HRESULT=0x80070002,
    // Win32=2] path=C:\Users\...\Temp». Порядок полей выбран так, чтобы по
    // одному grep в логе было видно и что упало, и с каким кодом.
    [[nodiscard]] std::string toString() const {
        std::string out;
        out.reserve(api.size() + text.size() + path.size() + 64);
        out += api.empty() ? "Win32" : api;
        out += ": ";
        out += text.empty() ? "текст недоступен" : text;

        char codes[64] = {};
        const unsigned long hresult = static_cast<unsigned long>(static_cast<std::uint32_t>(hr));
        DWORD win32Code = ERROR_SUCCESS;
        if (tryWin32Code(hr, win32Code)) {
            std::snprintf(codes, sizeof(codes), " [HRESULT=0x%08lX, Win32=%lu]", hresult,
                          static_cast<unsigned long>(win32Code));
        } else {
            std::snprintf(codes, sizeof(codes), " [HRESULT=0x%08lX]", hresult);
        }
        out += codes;

        if (!path.empty()) {
            out += " path=";
            out += path;
        }
        return out;
    }
};

// Путь в ошибке принимается в любой из форм, в которых он живёт в проекте:
// пустой, UTF-8 (std::string, const char*) и UTF-16 (std::wstring, const
// wchar*). Приведение делается один раз здесь, чтобы ни один вызов не
// вспоминал про кодировки в самый неподходящий момент.
[[nodiscard]] inline std::string pathToUtf8(std::string_view path) {
    return std::string(path);
}

[[nodiscard]] inline std::string pathToUtf8(const std::string& path) {
    return path;
}

[[nodiscard]] inline std::string pathToUtf8(const char* path) {
    return path == nullptr ? std::string{} : std::string(path);
}

[[nodiscard]] inline std::string pathToUtf8(const std::wstring& path) {
    return toUtf8(path);
}

[[nodiscard]] inline std::string pathToUtf8(const wchar_t* path) {
    return toUtf8(path);
}

// Диагностика по известному HRESULT. Путь необязателен: не у каждого вызова
// WinAPI есть путь (нет у CreateEventW, есть у CreateFileW).
template <typename Path = std::string>
[[nodiscard]] inline WinErrorInfo hresultInfo(HRESULT hr, std::string_view api, const Path& path = Path{}) {
    WinErrorInfo info;
    info.hr = hr;
    info.api = std::string(api);
    info.text = hresultErrorText(hr);
    info.path = pathToUtf8(path);
    return info;
}

// Диагностика по свежему GetLastError(). Порядок обязателен: вызвать сразу
// после неудачного WinAPI-вызова.
template <typename Path = std::string>
[[nodiscard]] inline WinErrorInfo lastErrorInfo(std::string_view api, const Path& path = Path{}) {
    return hresultInfo(lastErrorHresult(), api, path);
}

// Текст без исключений — для горячих циклов и для мест, где вызывающий сам
// решает, что делать с отказом (пропустить элемент, повторить, записать в лог).
[[nodiscard]] inline std::string describeLastError(std::string_view api) {
    return lastErrorInfo(api).toString();
}

[[nodiscard]] inline std::string describeHresult(HRESULT hr, std::string_view api) {
    return hresultInfo(hr, api).toString();
}

// ---------------------------------------------------------------------------
// Ошибка как исключение
// ---------------------------------------------------------------------------

// Исключение слоя Win32. Текст what() — та же строка, что уходит в лог, поэтому
// «необработанное исключение» в журнале читается без расшифровки. Код и путь
// остаются доступны по частям: UI показывает путь, отчёт — HRESULT.
class WinError : public std::runtime_error {
public:
    explicit WinError(WinErrorInfo info)
        : std::runtime_error(info.toString()),
          info_(std::move(info)) {
    }

    [[nodiscard]] const WinErrorInfo& info() const noexcept {
        return info_;
    }

    [[nodiscard]] HRESULT code() const noexcept {
        return info_.hr;
    }

private:
    WinErrorInfo info_;
};

// Бросить по свежему GetLastError(). Только там, где отказ действительно
// исключителен: нет ни значения, с которым можно продолжить, ни повода ждать.
template <typename Path = std::string>
[[noreturn]] inline void throwLastError(std::string_view api, const Path& path = Path{}) {
    throw WinError(lastErrorInfo(api, path));
}

// Бросить по известному HRESULT — так ведут себя COM/WMI и функции WinHTTP.
template <typename Path = std::string>
[[noreturn]] inline void throwHresultFailure(HRESULT hr, std::string_view api, const Path& path = Path{}) {
    throw WinError(hresultInfo(hr, api, path));
}

// Проверка HRESULT с выбросом: короткая замена «if (FAILED(hr)) …» в местах,
// где продолжать нечего. В горячем цикле — isSuccess() и ветка отказа, а не
// это.
template <typename Path = std::string>
inline void checkHresult(HRESULT hr, std::string_view api, const Path& path = Path{}) {
    if (isSuccess(hr)) return;
    throwHresultFailure(hr, api, path);
}

}  // namespace mrproper::platform
