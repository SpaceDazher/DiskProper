// MrProper — HTTP-загрузка набора правил на WinHTTP. Реализация §9.2 шага 1
// («запрос только GET по HTTPS, таймаут 10 с, без куки и авторизация»).
// Шапка net.hpp объясняет, почему именно так; здесь — как это сделано и
// почему именно этими вызовами.
//
// Форма обмена. Сессия и соединение живут весь вызов fetch, а запрос
// создаётся заново на каждый прыжок редиректа: после ответа с кодом 3xx
// handle запроса использовать нельзя, и переиспользование его означало бы
// либо утечку, либо чтение тела предыдущего адреса в новый ответ. Ручной
// разбор редиректов вместо WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS — не
// педантизм: политика по умолчанию разрешает переход https → http, а §9.2 и
// ADR-008 требуют, чтобы понижение до незащищённого канала было отказом, а не
// тихим «следуем». Здесь политика выставлена в NEVER, а каждый Location
// проходит тот же строгий разбор, что и исходная ссылка.
//
// Куки и авторизация. Обе возможности выключаются на сессии:
// WINHTTP_DISABLE_COOKIES и WINHTTP_DISABLE_AUTHENTICATION. Дальше —
// перестраховка, а не украшение: WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH
// запрещает отвечать на 401 своими учётными данными, а
// WINHTTP_OPTION_REJECT_USERPWD_IN_URL запрещает сам такой URL. Первые две
// настройки обязательны — их отказ означает, что канал не соответствует
// §9.2, и fetch возвращает SecurityPolicyFailed, а не «как получилось».
// Последние две подстраховочные: разбор URL уже запрещает userinfo, поэтому
// их отказ (например, на необычной сборке WinHTTP) — повод для debug-записи,
// а не для отказа всей загрузки.
//
// Таймауты. У WinHTTP нет общего дедлайна — есть четыре фазы. Бюджет §9.2
// делится на них поровну, но не ниже kMinPhaseTimeout, и, что важнее, общий
// остаток пересчитывается перед каждым прыжком: цепочка из пяти редиректов
// съедает те же 10 с, а не 50. Точное время завершения остаётся за WinHTTP:
// фаза — это «ждать не дольше», а не «вернусь через», поэтому фактическое
// время может оказаться чуть больше суммы фаз.
//
// Предел тела. Content-Length проверяется до чтения, а при его отсутствии —
// по накопленному, порциями по 8 КиБ. Тело ответа — недоверенный ввод
// (ADR-008), поэтому оно не растёт без предела; при превышении тело
// осознанно не дочитывается и в результат не попадает.
#include "net.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <vector>

// windows.h включается вторым: он нужен для типов HINTERNET и кодов ошибок,
// а объявления WinHTTP лежат в winhttp.h.
#include <windows.h> // NOLINT(bugprone-suspicious-include) — слой Win32, единственное законное место
#include <winhttp.h>

#include "core/log.hpp"
#include "win_error.hpp"
#include "win_handle.hpp"

namespace mrproper::platform::net {
namespace {

// HINTERNET — это void*, а закрывает его WinHttpCloseHandle, а не
// CloseHandle. Политика закрытия в win_handle.hpp (ADR-001) как раз для таких
// случаев: свой тип, четыре строки, и забыть закрыть handle уже нельзя.
struct InternetHandlePolicy {
    using HandleType = HINTERNET;

    [[nodiscard]] static bool isValid(HandleType handle) noexcept {
        return handle != nullptr;
    }

    static void close(HandleType handle) noexcept {
        if (isValid(handle)) ::WinHttpCloseHandle(handle);
    }
};

using InternetHandle = platform::unique_handle<InternetHandlePolicy>;

// Имя события в логе — одной строкой, чтобы по grep находились все записи.
constexpr std::string_view kLogEvent = "platform.net.fetch";

// Порция чтения тела. 8 КиБ — размер по умолчанию у WinHTTP плюс запас;
// чтение крупными кусками выигрыша не даёт, а мелкие упираются в вызовы.
constexpr DWORD kReadChunkBytes = 8u * 1024u;

// Потолок для длины в WinHttpSetTimeouts (там int). Значение выше
// INT_MAX не имеет смысла и приводится к этому потолку.
constexpr int kMaxTimeoutMs = 0x7FFFFFFF;

// ---------------------------------------------------------------------------
// Разбор ссылки
// ---------------------------------------------------------------------------

bool isAsciiPrintable(char ch) noexcept {
    const auto value = static_cast<unsigned char>(ch);
    return value >= 0x21u && value <= 0x7Eu;
}

bool isDigit(char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

bool isHexDigit(char ch) noexcept {
    return isDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = static_cast<char>(std::tolower(static_cast<unsigned char>(left[i])));
        const auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(right[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

// Схема со знаком «://». Отдельная функция, потому что её результат —
// различие между «ссылка не та» (UnsupportedScheme, осмысленная причина для
// лога) и «ссылка — мусор» (InvalidUrl).
FetchStatus takeScheme(std::string_view url, std::string_view& rest) noexcept {
    const std::size_t separator = url.find("://");
    if (separator == std::string_view::npos || separator == 0) {
        return FetchStatus::InvalidUrl;
    }
    const std::string_view scheme = url.substr(0, separator);
    if (!equalsIgnoreCase(scheme, "https")) {
        return FetchStatus::UnsupportedScheme;
    }
    rest = url.substr(separator + 3);
    return FetchStatus::Ok;
}

// Проверка пути с запросом. Пробелы и управляющие символы сервер или
// WinHTTP исказит по-своему; «%» без двух шестнадцатеричных цифр — обрыв
// экранировки, а не символ по имени; обратная косая черта нормализуется
// серверами по-разному, то есть такая ссылка значит не одно и то же на
// первом и последнем прыжке.
FetchStatus checkTarget(std::string_view target) noexcept {
    for (std::size_t i = 0; i < target.size(); ++i) {
        const char ch = target[i];
        if (!isAsciiPrintable(ch)) {
            return FetchStatus::InvalidUrl;
        }
        if (ch == '\\') {
            return FetchStatus::InvalidUrl;
        }
        if (ch == '%') {
            if (i + 2 >= target.size() || !isHexDigit(target[i + 1]) || !isHexDigit(target[i + 2])) {
                return FetchStatus::InvalidUrl;
            }
            i += 2;
        }
    }
    return FetchStatus::Ok;
}

FetchStatus parseUrlParts(std::string_view url, Url& out) noexcept {
    if (url.empty() || url.size() > kMaxUrlLength) {
        return FetchStatus::InvalidUrl;
    }

    std::string_view rest;
    const FetchStatus scheme = takeScheme(url, rest);
    if (scheme != FetchStatus::Ok) {
        return scheme;
    }

    // Якорь и всё после него серверу не отправляется: отбрасываем сразу,
    // чтобы дальше не возникало вопроса, копировать его в Location или нет.
    if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
        rest = rest.substr(0, hash);
    }

    std::size_t authorityEnd = rest.size();
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '/' || rest[i] == '?') {
            authorityEnd = i;
            break;
        }
    }
    const std::string_view authority = rest.substr(0, authorityEnd);
    std::string_view target = rest.substr(authorityEnd);
    // Путь в ссылке необязателен: без него сервер отвечает на корень. Вспомогательная
    // строка живёт до конца функции — target на неё ссылается.
    std::string rooted;
    if (target.empty() || target.front() == '?') {
        rooted.reserve(target.size() + 1);
        rooted.push_back('/');
        rooted.append(target.data(), target.size());
        target = std::string_view(rooted);
    }

    if (authority.empty()) {
        return FetchStatus::InvalidUrl;
    }
    // userinfo запрещён: логин и пароль в ссылке — это и есть авторизация,
    // которой в §9.2 нет, и утечка, если ссылка попадёт в лог.
    if (authority.find('@') != std::string_view::npos) {
        return FetchStatus::InvalidUrl;
    }
    // Литеральный IPv6 в скобках для ссылки на публичный репозиторий не
    // нужен, аWinHttpConnect ждёт имя без скобок, то есть нужен отдельный
    // разбор — лишняя поверхность на входе, где ошибка стоит дорого.
    if (authority.find('[') != std::string_view::npos || authority.find(']') != std::string_view::npos) {
        return FetchStatus::InvalidUrl;
    }

    std::string_view host = authority;
    std::string_view port;
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty()) {
        return FetchStatus::InvalidUrl;
    }
    for (const char ch : host) {
        // Только ASCII: punycode приходит от вызывающего, а не-ASCII имя
        // потребовало бы IdnToAscii и линковки с dnsapi.
        if (!isAsciiPrintable(ch) || ch == ':' || ch == ' ') {
            return FetchStatus::InvalidUrl;
        }
    }

    std::uint16_t hostPort = 443;
    bool explicitPort = false;
    if (!port.empty()) {
        std::uint32_t value = 0;
        for (const char ch : port) {
            if (!isDigit(ch)) {
                return FetchStatus::InvalidUrl;
            }
            value = value * 10u + static_cast<std::uint32_t>(ch - '0');
            if (value > 65535u) {
                return FetchStatus::InvalidUrl;
            }
        }
        if (value == 0) {
            return FetchStatus::InvalidUrl;
        }
        hostPort = static_cast<std::uint16_t>(value);
        explicitPort = true;
    }

    const FetchStatus targetCheck = checkTarget(target);
    if (targetCheck != FetchStatus::Ok) {
        return targetCheck;
    }

    out.host.assign(host);
    out.port = hostPort;
    out.target.assign(target);
    out.explicitPort = explicitPort;
    return FetchStatus::Ok;
}

// Разбор Location. RFC 3986 в том виде, в каком это нужно для редиректа:
// абсолютная ссылка, «//host/path» (схема-относительная) или относительный
// путь от каталога текущего документа. Всё, что пришло, снова проходит
// строгий разбор — иначе «проверенная ссылка» проверялась бы только на
// первом прыжке.
FetchStatus resolveRedirect(const Url& base, std::string_view location, Url& out) noexcept {
    // Схему и длину смотрим до любых преобразований: «http://…» обязан быть
    // отказом InsecureRedirect, а не «ссылкой без схемы».
    if (const std::size_t separator = location.find("://");
        separator != std::string_view::npos && separator > 0) {
        if (!equalsIgnoreCase(location.substr(0, separator), "https")) {
            return FetchStatus::InsecureRedirect;
        }
        return parseUrlParts(location, out);
    }
    if (location.rfind("//", 0) == 0) {
        std::string absolute;
        absolute.reserve(location.size() + 6);
        absolute.append("https:", 6);
        absolute.append(location.data(), location.size());
        return parseUrlParts(absolute, out);
    }

    // Относительный адрес: от каталога текущего пути, с удалением сегментов
    // «.», «..» и пустых. Схема и хост остаются прежними, но путь всё равно
    // проверяется целиком.
    std::string target(base.target);
    if (location.empty()) {
        return FetchStatus::InvalidRedirect;
    }
    if (location.front() == '/') {
        target.assign(location);
    } else {
        const std::size_t lastSlash = base.target.rfind('/');
        target.resize(lastSlash == std::string_view::npos ? 0 : lastSlash + 1);
        target.append(location);
    }
    const std::size_t hash = target.find('#');
    if (hash != std::string::npos) {
        target.resize(hash);
    }

    std::vector<std::string_view> segments;
    std::string_view view(target);
    while (!view.empty()) {
        std::size_t cut = view.find('/', 1);
        if (cut == std::string_view::npos) {
            cut = view.size();
        }
        const std::string_view segment = view.substr(0, cut);
        if (segment == "..") {
            // Выход выше корня не имеет смысла и для HTTP, но именно такая
            // форма в Location — признак того, что цепочку собирает не
            // сервер, а перенаправляющая страница. Оставляем как есть: путь
            // ниже корня для сервера не значит ничего плохого, а вот тихо
            // выкидывать сегменты — значит менять адрес, который прислали.
            if (!segments.empty()) {
                segments.pop_back();
            }
        } else if (segment != ".") {
            segments.push_back(segment);
        }
        view = cut < view.size() ? view.substr(cut) : std::string_view{};
    }

    std::string normalized;
    for (const std::string_view segment : segments) {
        normalized.append(segment);
    }
    if (normalized.empty() || normalized.front() != '/') {
        return FetchStatus::InvalidRedirect;
    }

    out.host.assign(base.host);
    out.port = base.port;
    out.explicitPort = base.explicitPort;
    out.target = std::move(normalized);
    const FetchStatus targetCheck = checkTarget(out.target);
    if (targetCheck != FetchStatus::Ok) {
        return targetCheck;
    }
    return FetchStatus::Ok;
}

// ---------------------------------------------------------------------------
// Ответ сервера
// ---------------------------------------------------------------------------

// Код ответа в 3xx, за которым имеет смысл идти. 300 (Multiple Choices) и 304
// (Not Modified) — не редирект: первый требует выбора, второй не имеет тела.
bool isRedirectCode(unsigned code) noexcept {
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

bool isSuccessCode(unsigned code) noexcept {
    return code >= 200 && code < 300;
}

// Значение заголовка строкой. WinHttpQueryHeaders требует двух вызовов:
// первый спрашивает размер, второй читает. Отсутствующий заголовок — не
// ошибка запроса, а пустая строка: Content-Length у ответа без тела нет, и
// это норма. Имя заголовка передаётся как NULL — тогда его называет сам
// уровень запроса (WINHTTP_HEADER_NAME_BY_INDEX, то же значение NULL).
std::string queryHeader(HINTERNET request, DWORD infoLevel) {
    DWORD length = 0;
    if (::WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length,
                              WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return {};
    }
    if (length == 0) {
        return {};
    }
    std::vector<char> buffer(length, '\0');
    DWORD read = 0;
    if (::WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &read,
                              WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return {};
    }
    if (static_cast<std::size_t>(read) >= buffer.size()) {
        read = static_cast<DWORD>(buffer.size() - 1);
    }
    return std::string(buffer.data(), read);
}

// Один заголовок из блока, полученного целиком. Блок приходит ASCII,
// строка сравнивается без учёта регистра (RFC 7230), значение обрезается по
// краям. Портной строки в значении быть не может: CRLF внутри не
// заканчивает строку, а CR где-то посередине — мусор, который мы не
// используем ни в ссылке, ни в подписи.
std::string pickHeader(std::string_view raw, std::string_view name) {
    std::size_t lineStart = 0;
    while (lineStart < raw.size()) {
        std::size_t lineEnd = raw.find("\r\n", lineStart);
        if (lineEnd == std::string_view::npos) {
            lineEnd = raw.size();
        }
        const std::string_view line = raw.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 2;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        if (!equalsIgnoreCase(line.substr(0, colon), name)) {
            continue;
        }

        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }
    return {};
}

// Блок заголовков целиком — нужен ради Location, отдельного уровня запроса
// для него в WinHTTP нет.
std::string queryRawHeaders(HINTERNET request) {
    return queryHeader(request, WINHTTP_QUERY_RAW_HEADERS_CRLF);
}

// ---------------------------------------------------------------------------
// Коды ошибок WinHTTP → FetchStatus
// ---------------------------------------------------------------------------

// Раскладка по коду. Порядок важен: частные случаи (таймаут, TLS, имя не
// найдено) перечислены раньше общего Unavailable, иначе отказ «сертификат не
// выдан» читался бы в логе как «что-то с сетью».
FetchStatus classifyError(DWORD code) noexcept {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:
        case ERROR_WINHTTP_OPERATION_CANCELLED:
            return FetchStatus::Timeout;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        case ERROR_WINHTTP_SECURE_FAILURE_PROXY:
            return FetchStatus::TlsFailed;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
        case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
        case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:
        case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:
        case ERROR_WINHTTP_NOT_INITIALIZED:
            return FetchStatus::ConnectFailed;
        case ERROR_WINHTTP_RESEND_REQUEST:
        case ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW:
        case ERROR_WINHTTP_CHUNKED_ENCODING_HEADER_SIZE_OVERFLOW:
            return FetchStatus::SendFailed;
        case ERROR_WINHTTP_INVALID_URL:
        case ERROR_WINHTTP_INVALID_HEADER:
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        case ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH:
        case ERROR_WINHTTP_HEADER_NOT_FOUND:
            return FetchStatus::ResponseInvalid;
        case ERROR_WINHTTP_REDIRECT_FAILED:
            return FetchStatus::InvalidRedirect;
        default:
            break;
    }
    // Всё остальное — включая ERROR_WINHTTP_LOGIN_FAILURE (сервер потребовал
    // авторизацию, которой у нас нет и быть не должно) и нехватку ресурсов:
    // причина попадёт в лог отдельной строкой, а состояние означает одно и то
    // же — «продолжаем на прошлом наборе правил» (§9.2 шаг 4).
    return FetchStatus::Unavailable;
}

// Человеческое объяснение кода: у WinHTTP текста нет (это не коды Win32,
// FormatMessage их не знает), поэтому словарь держим здесь. Не локализация —
// сообщения слоя русские, UI переводит свои подписи отдельно.
std::string errorText(DWORD code) noexcept {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:
            return "истекло время ожидания";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "имя узла не разрешено";
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return "соединение не установлено";
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "разрыв соединения";
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
            return "TLS-рукопожатие не состоялось";
        case ERROR_WINHTTP_SECURE_FAILURE_PROXY:
            return "прокси подменил сертификат";
        case ERROR_WINHTTP_INVALID_URL:
            return "сервер отклонил ссылку";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        case ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH:
            return "ответ не похож на HTTP";
        case ERROR_WINHTTP_LOGIN_FAILURE:
            return "сервер потребовал авторизацию";
        case ERROR_WINHTTP_REDIRECT_FAILED:
            return "редирект обработан не удалось";
        case ERROR_WINHTTP_OUT_OF_HANDLES:
            return "не хватило ресурсов";
        default:
            break;
    }
    return "ошибка WinHTTP";
}

// ---------------------------------------------------------------------------
// Сессия
// ---------------------------------------------------------------------------

// Код отказа и его код Win32 — пара, потому что лог должен показать оба
// (SPEC §5, §12: «все ошибки в логе с путём и HRESULT»). GetLastError()
// читается сразу после неудачного вызова WinHTTP: любое другое обращение к
// API способно затереть код.
struct Failure {
    DWORD code{ERROR_SUCCESS};
};

// Снимок GetLastError(). Звать сразу после неудачного вызова WinHTTP: любое
// другое обращение к API способно затереть код (то же правило, что в
// win_error.hpp).
void noteFailure(Failure& failure) noexcept {
    failure.code = static_cast<DWORD>(::GetLastError());
}

// Остаток общего бюджета. Неположительный остаток — бюджет исчерпан, и
// идти в сеть с нулевым таймаутом бессмысленно.
std::chrono::milliseconds remainingBudget(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

int toWinHttpMilliseconds(std::chrono::milliseconds value) noexcept {
    if (value <= std::chrono::milliseconds::zero()) {
        return 0;
    }
    const auto count = value.count();
    if (count >= static_cast<std::chrono::milliseconds::rep>(kMaxTimeoutMs)) {
        return kMaxTimeoutMs;
    }
    return static_cast<int>(count);
}

// Таймауты фаз под остаток бюджета: поровну, но не меньше
// kMinPhaseTimeout. Сумма частей равна остатку (плюс округление), поэтому
// обмен укладывается в общий бюджет, а не в его восьмерь.
void applyTimeouts(HINTERNET session, std::chrono::milliseconds budget, Failure& failure) {
    std::chrono::milliseconds phase = budget / 4;
    if (phase < kMinPhaseTimeout) {
        phase = kMinPhaseTimeout;
    }
    const int ms = toWinHttpMilliseconds(phase);
    if (::WinHttpSetTimeouts(session, ms, ms, ms, ms) == FALSE) {
        failure.code = static_cast<DWORD>(::GetLastError());
    }
}

// Выключить куки и авторизацию. Обязательные настройки §9.2: их отказ
// означает «канал не тот», и продолжать нельзя.
void applySecurityPolicy(HINTERNET session, Failure& failure) {
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (::WinHttpSetOption(session, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) == FALSE) {
        noteFailure(failure);
        return;
    }
    // Ответ на 401 своими учётными данными WinHTTP здесь не отправит: даже
    // если где-то в системе есть имя прокси, модуль его не знает и не
    // спросит. Дублирует WINHTTP_DISABLE_AUTHENTICATION, но дешёвый.
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (::WinHttpSetOption(session, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)) == FALSE) {
        noteFailure(failure);
        return;
    }
    // TLS не ниже 1.2 и без отката на SSL3/TLS1.0. Отказ настройки — тоже
    // отказ: молча продолжить со «старым» набором протоколов значило бы
    // разрешить ровно то, что §9.2 и ADR-008 запрещают.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)) == FALSE) {
        noteFailure(failure);
        return;
    }
    // Редиректы разбираем сами (см. шапку файла): иначе политика по
    // умолчанию тихо разрешит уход на http.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (::WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)) == FALSE) {
        noteFailure(failure);
    }
}

// Перестраховочные настройки: разбор URL уже запретил userinfo, поэтому
// отказ здесь — не повод отказывать в загрузке, а повод сказать об этом в
// лог на debug. Иначе «сеть недоступна» и «WinHTTP не умеет эту настройку»
// были бы одной строкой.
void applyHardening(HINTERNET session) {
    DWORD reject = TRUE;
    if (::WinHttpSetOption(session, WINHTTP_OPTION_REJECT_USERPWD_IN_URL, &reject, sizeof(reject)) == FALSE) {
        mrproper::core::logDebug(kLogEvent, "WinHTTP не принял запрет логина в ссылке");
    }
}

// Один ответ: код, тело и то, что из заголовков. Собирается отдельно от
// FetchResult, потому что ответ читается постепенно, а заполнять результат
// по частям значит показать вызывающему полузаполненное тело.
struct Response {
    unsigned code{0};
    std::string body;
    std::string contentType;
    std::string etag;
    std::string lastModified;
    std::string location;
    bool tooLarge{false};
};

// Прочитать тело целиком, не превышая предел. Content-Length проверяется до
// чтения (экономит трафик на заведомо большом ответе), а при chunked или
// без заголовка — по накопленному, порциями.
void readBody(HINTERNET request, std::size_t maxBytes, Response& response, Failure& failure) {
    std::vector<char> buffer(kReadChunkBytes);

    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(request, &available) == FALSE) {
            noteFailure(failure);
            return;
        }
        if (available == 0) {
            return;
        }
        if (response.body.size() + static_cast<std::size_t>(available) > maxBytes) {
            // Тело осознанно не дочитывается: половина файла правил хуже, чем
            // её отсутствие, потому что вызывающий проверяет хеш целиком.
            response.tooLarge = true;
            return;
        }

        DWORD readBytes = 0;
        const std::size_t want =
            std::min(static_cast<std::size_t>(kReadChunkBytes), static_cast<std::size_t>(available));
        if (::WinHttpReadData(request, buffer.data(), static_cast<DWORD>(want), &readBytes) == FALSE) {
            noteFailure(failure);
            return;
        }
        if (readBytes == 0) {
            // Данных заявлено было, а пришло ноль: это конец тела, и крутить
            // цикл дальше бессмысленно.
            return;
        }
        response.body.append(buffer.data(), readBytes);
    }
}

// Один прыжок: открыть запрос, отправить GET без единого своего заголовка,
// принять ответ, прочитать тело. Никаких WinHttpAddRequestHeaders и
// WinHttpSetCredentials в модуле нет — это и есть «без кук и авторизации».
bool sendRequest(HINTERNET connect, const Url& url, const FetchOptions& options, Response& response,
                 Failure& failure) {
    // Отправка начинается с чистого кода: SUCCESS здесь означает «отправлено»,
    // и следы от предыдущего прыжка не должны выглядеть как отказ этого.
    failure.code = ERROR_SUCCESS;
    const std::wstring host = platform::toUtf16(url.host);
    const std::wstring target = platform::toUtf16(url.target);
    if (host.empty() || target.empty()) {
        failure.code = ERROR_INVALID_PARAMETER;
        return false;
    }

    // WINHTTP_FLAG_SECURE: TLS включается явно, а не «по умолчанию на порту
    // 443» — иначе неявная схема однажды окажется забытой.
    const DWORD flags = WINHTTP_FLAG_SECURE | WINHTTP_FLAG_BYPASS_PROXY_CACHE;
    InternetHandle request = platform::adopt<InternetHandlePolicy>(::WinHttpOpenRequest(
        connect, L"GET", target.c_str(), L"HTTP/1.1", WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.valid()) {
        noteFailure(failure);
        return false;
    }

    // WINHTTP_NO_ADDITIONAL_HEADERS: ни Cookie, ни Authorization, ни
    // User-Agent вручную — значит, отправлять нечего и нечем.
    if (::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ==
        FALSE) {
        noteFailure(failure);
        return false;
    }
    if (::WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        noteFailure(failure);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusLength = sizeof(statusCode);
    if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                              &statusLength, WINHTTP_NO_HEADER_INDEX) == FALSE) {
        noteFailure(failure);
        return false;
    }
    response.code = static_cast<unsigned>(statusCode);

    const std::string rawHeaders = queryRawHeaders(request.get());
    if (response.code >= 300 && response.code < 400) {
        // Адрес следующего прыжка нужен только здесь; для 2xx он в памяти не
        // держится.
        response.location = pickHeader(rawHeaders, "Location");
        return true;
    }

    response.contentType = pickHeader(rawHeaders, "Content-Type");
    response.etag = pickHeader(rawHeaders, "ETag");
    response.lastModified = pickHeader(rawHeaders, "Last-Modified");
    if (!isSuccessCode(response.code)) {
        // Тело ошибки не читаем: это HTML-страница, а разбирать её должен
        // тот, кто покажет пользователю текст ошибки (§7), и модуль, который
        // приносит файлы правил, к этому отношения не имеет.
        return true;
    }

    const std::size_t maxBytes = options.maxBodyBytes == 0 ? kDefaultMaxBodyBytes : options.maxBodyBytes;
    const std::string declared = pickHeader(rawHeaders, "Content-Length");
    if (!declared.empty()) {
        std::size_t announced = 0;
        bool digits = true;
        for (const char ch : declared) {
            if (!isDigit(ch)) {
                digits = false;
                break;
            }
            announced = announced * 10u + static_cast<std::size_t>(ch - '0');
        }
        // Content-Length принадлежит сети, а не нам: мусор в нём или число
        // больше предела — повод не читать тело вовсе, а не повод поверить.
        if (digits && announced > maxBytes) {
            response.tooLarge = true;
            return true;
        }
    }

    readBody(request.get(), maxBytes, response, failure);
    return failure.code == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Обмен целиком
// ---------------------------------------------------------------------------

void fetchImpl(std::string_view url, const FetchOptions& options, FetchResult& result) {
    Url current;
    const FetchStatus parsed = parseHttpsUrl(url, current);
    if (parsed != FetchStatus::Ok) {
        result.status = parsed;
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    const std::wstring agent = platform::toUtf16(options.userAgent);

    // Ссылка, с которой ответ придёт на самом деле: до редиректов это
    // исходная, после каждого прыжка — текущая. Нужна и в логе, и вызывающему:
    // «какой адрес на самом деле отдал файл» — половина разбора отказа.
    result.finalUrl = formatUrl(current);

    // WINHTTP_NO_PROXY_NAME с WINHTTP_ACCESS_TYPE_NO_PROXY — соединение
    // прямое: см. ProxyMode в заголовке, где сказано, почему системный
    // прокси по умолчанию выключен.
    const DWORD accessType =
        options.proxy == ProxyMode::System ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
    InternetHandle session = platform::adopt<InternetHandlePolicy>(
        ::WinHttpOpen(agent.empty() ? nullptr : agent.c_str(), accessType, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.valid()) {
        result.status = FetchStatus::ConnectFailed;
        result.win32Error = static_cast<std::uint32_t>(::GetLastError());
        return;
    }

    Failure failure;
    applySecurityPolicy(session.get(), failure);
    if (failure.code != ERROR_SUCCESS) {
        result.status = FetchStatus::SecurityPolicyFailed;
        result.win32Error = failure.code;
        return;
    }
    applyHardening(session.get());

    // Соединение переиспользуется, пока хост и порт те же: пять редиректов
    // внутри одного домена не должны открывать пять соединений.
    InternetHandle connect;
    Url connected;

    for (;;) {
        const std::chrono::milliseconds budget = remainingBudget(deadline);
        if (budget <= std::chrono::milliseconds::zero()) {
            result.status = FetchStatus::Timeout;
            return;
        }
        applyTimeouts(session.get(), budget, failure);
        if (failure.code != ERROR_SUCCESS) {
            result.status = classifyError(failure.code);
            result.win32Error = failure.code;
            return;
        }

        if (!connect.valid() || connected.host != current.host || connected.port != current.port) {
            const std::wstring host = platform::toUtf16(current.host);
            connect =
                platform::adopt<InternetHandlePolicy>(::WinHttpConnect(session.get(), host.c_str(), current.port, 0));
            if (!connect.valid()) {
                const DWORD code = static_cast<DWORD>(::GetLastError());
                result.status = classifyError(code);
                result.win32Error = code;
                return;
            }
            connected.host.assign(current.host);
            connected.port = current.port;
        }

        Response response;
        if (!sendRequest(connect.get(), current, options, response, failure)) {
            result.status = classifyError(failure.code);
            result.win32Error = failure.code;
            return;
        }

        if (response.tooLarge) {
            result.httpStatus = response.code;
            result.status = FetchStatus::TooLarge;
            return;
        }

        if (isRedirectCode(response.code)) {
            if (static_cast<int>(result.redirects) >= options.maxRedirects) {
                result.httpStatus = response.code;
                result.status = FetchStatus::TooManyRedirects;
                return;
            }
            if (response.location.empty()) {
                result.httpStatus = response.code;
                result.status = FetchStatus::InvalidRedirect;
                return;
            }
            Url next;
            const FetchStatus moved = resolveRedirect(current, response.location, next);
            if (moved != FetchStatus::Ok) {
                result.httpStatus = response.code;
                result.status = moved;
                return;
            }
            current = std::move(next);
            ++result.redirects;
            result.finalUrl = formatUrl(current);
            continue;
        }

        result.httpStatus = response.code;
        if (!isSuccessCode(response.code)) {
            result.status = FetchStatus::HttpError;
            return;
        }

        result.body = std::move(response.body);
        result.contentType = std::move(response.contentType);
        result.etag = std::move(response.etag);
        result.lastModified = std::move(response.lastModified);
        result.status = FetchStatus::Ok;
        return;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Имена состояний
// ---------------------------------------------------------------------------

const char* toString(FetchStatus status) noexcept {
    switch (status) {
        case FetchStatus::Ok:
            return "ok";
        case FetchStatus::InvalidUrl:
            return "invalid_url";
        case FetchStatus::UnsupportedScheme:
            return "unsupported_scheme";
        case FetchStatus::InsecureRedirect:
            return "insecure_redirect";
        case FetchStatus::TooManyRedirects:
            return "too_many_redirects";
        case FetchStatus::InvalidRedirect:
            return "invalid_redirect";
        case FetchStatus::SecurityPolicyFailed:
            return "security_policy_failed";
        case FetchStatus::ConnectFailed:
            return "connect_failed";
        case FetchStatus::SendFailed:
            return "send_failed";
        case FetchStatus::ReceiveFailed:
            return "receive_failed";
        case FetchStatus::Timeout:
            return "timeout";
        case FetchStatus::TlsFailed:
            return "tls_failed";
        case FetchStatus::HttpError:
            return "http_error";
        case FetchStatus::TooLarge:
            return "too_large";
        case FetchStatus::ResponseInvalid:
            return "response_invalid";
        case FetchStatus::Unavailable:
            break;
    }
    return "unavailable";
}

// ---------------------------------------------------------------------------
// Разбор и сборка ссылки
// ---------------------------------------------------------------------------

FetchStatus parseHttpsUrl(std::string_view url, Url& out) {
    return parseUrlParts(url, out);
}

std::string formatUrl(const Url& url) {
    std::string text = "https://";
    text += url.host;
    if (url.port != 443) {
        text += ':';
        text += std::to_string(static_cast<unsigned>(url.port));
    }
    text += url.target;
    return text;
}

std::string urlEncodePath(std::string_view path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const char raw : path) {
        const auto value = static_cast<unsigned char>(raw);
        // Буквы, цифры, «-_.~», «/», «:» — и есть «уже экранировано»: двоеточие
        // нужно для указания ревизии тега, слэш — разделитель сегментов пути.
        const bool plain = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                          (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' ||
                          value == '~' || value == '/' || value == ':';
        if (plain) {
            out.push_back(raw);
            continue;
        }
        // Каждый байт UTF-8 кодируется отдельно, поэтому не-ASCII имя тега
        // даёт корректную последовательность %XX, а не «символ по имени».
        out.push_back('%');
        out.push_back(kHex[(value >> 4) & 0x0Fu]);
        out.push_back(kHex[value & 0x0Fu]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Загрузка
// ---------------------------------------------------------------------------

FetchResult fetch(std::string_view url, const FetchOptions& options) noexcept {
    FetchResult result;
    const auto started = std::chrono::steady_clock::now();
    try {
        fetchImpl(url, options, result);
    } catch (const std::bad_alloc&) {
        // Ответ неполный — в результат не попадает: вызывающий получит
        // пустое тело вместе с отказом, а не половину файла правил.
        result.body.clear();
        result.status = FetchStatus::Unavailable;
        result.win32Error = static_cast<std::uint32_t>(ERROR_NOT_ENOUGH_MEMORY);
        mrproper::core::logWarn(kLogEvent, "не хватило памяти на загрузку");
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    // Поля собираются вручную: макросы MRP_LOG_* для списка из двух и более
    // пар непригодны (detail::logFieldList разворачивает пакет в один вызов
    // logField, и такой вызов не разрешается). Чужой заголовок не правим.
    mrproper::core::LogFields fields{
        mrproper::core::logField("url", std::string(url)),
        mrproper::core::logField("status", toString(result.status)),
    };
    if (result.httpStatus != 0) {
        fields.push_back(mrproper::core::logField("httpStatus", static_cast<std::int64_t>(result.httpStatus)));
    }
    if (result.redirects != 0) {
        fields.push_back(mrproper::core::logField("redirects", static_cast<std::int64_t>(result.redirects)));
    }
    if (result.ok()) {
        fields.push_back(mrproper::core::logField("bytes", static_cast<std::int64_t>(result.body.size())));
        mrproper::core::logInfo(kLogEvent, "файл скачан", fields);
        return result;
    }

    if (result.win32Error != 0) {
        fields.push_back(mrproper::core::logField("win32", static_cast<std::int64_t>(result.win32Error)));
        fields.push_back(mrproper::core::logField("errorText", errorText(result.win32Error)));
    }
    fields.push_back(mrproper::core::logField("elapsedMs", result.elapsed.count()));
    // §9.2 шаг 4: отказ тихий для пользователя, но обязателен в логе.
    if (options.logFailures) {
        mrproper::core::logWarn(kLogEvent, "файл не скачан", fields);
    }
    return result;
}

std::string describe(const FetchResult& result) {
    std::string out = "http=";
    if (result.httpStatus != 0) {
        out += std::to_string(result.httpStatus);
    } else {
        out += "-";
    }
    out += ", status=";
    out += toString(result.status);
    if (result.win32Error != 0) {
        out += ", win32=";
        out += std::to_string(static_cast<unsigned long>(result.win32Error));
        out += " (";
        out += errorText(result.win32Error);
        out += ")";
    }
    if (result.redirects != 0) {
        out += ", редиректов=";
        out += std::to_string(static_cast<unsigned long>(result.redirects));
    }
    if (result.ok()) {
        out += ", байт=";
        out += std::to_string(result.body.size());
    }
    out += ", мс=";
    out += std::to_string(result.elapsed.count());
    return out;
}

}  // namespace mrproper::platform::net
