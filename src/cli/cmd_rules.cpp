// mrproper-cli: команды «rules validate» и «rules verify» — проверка набора
// правил очистки из командной строки (SPEC §9.2 «Обновление правил с Git»,
// §11 «Стратегия тестирования», §12 «Обновление правил с GitHub работает»,
// ADR-008 «правила никогда не применяются без проверки»).
//
// Контракт команды (сигнатуры, коды возврата, шов для подписи, границы) живёт
// в cmd_rules.hpp — единственном объявлении об этом API. Здесь только
// реализация, по тому же принципу, по которому cmd_scan.cpp повторяет контракт
// cmd_scan.hpp.
//
// ---------------------------------------------------------------------------
// Зачем это две команды, а не одна
// ---------------------------------------------------------------------------
// «validate» отвечает на вопрос автора правил: «мой набор вообще разбирается?»
// Это шаг 5 §9.2 п.3 плюс кросс-проверки ядра (уникальность id, непустой
// набор, совпадение schemaVersion и версии в манифесте и в файлах правил).
// Подпись здесь не требуется: подписывает набор владелец ключа, а не автор.
//
// «verify» отвечает на вопрос CI: «это тот самый набор, который подписал
// владелец, и с подписи он не менялся?» Это полная цепочка §9.2 п.2 в том же
// порядке, в каком её реализует core::rulesync, и переставлять её нельзя:
//     1) схема манифеста;
//     2) minAppVersion ≤ версии приложения;
//     3) подпись Ed25519 манифеста;
//     4) SHA-256 и размер каждого файла из манифеста;
//     5) разбор файлов парсером правил.
// Пропуск любого шага — ненулевой код возврата, а не предупреждение
// (ADR-008: правила управляют тем, что приложение удаляет).
//
// ---------------------------------------------------------------------------
// Коды возврата (общая таблица CLI; те же значения в cmd_report.cpp)
// ---------------------------------------------------------------------------
//     0   успех;
//    64   неверные аргументы (EX_USAGE) — нет подкоманды, неизвестный флаг,
//         опция без значения;
//    65   данные не годятся (EX_DATAERR) — набор правил не проходит проверку,
//         отчёт повреждён, схема неизвестна;
//    66   нет входа (EX_NOINPUT) — каталог или файл не найден или не читается;
//    69   проверка невозможна (EX_UNAVAILABLE) — набор в порядке, но
//         обязательный шаг выполнить нечем (нет верификатора Ed25519); сюда же
//         попадает вызов несуществующей подкоманды, например «rules update»;
//    70   внутренняя ошибка (EX_SOFTWARE) — непойманное исключение.
//
// Значения взяты из соглашения sysexits.h: их понимает любой CI и любой
// скрипт, в отличие от «1 — ошибка, 2 — тоже ошибка».
//
// ---------------------------------------------------------------------------
// Контракт с остальным CLI
// ---------------------------------------------------------------------------
// Объявления — в cmd_rules.hpp; здесь определяются обе точки входа, они живут в
// namespace mrproper::cli и зависят только от core и стандартной библиотеки,
// поэтому их можно вызвать из любого dispatch-а. args — то, что стоит после
// «rules» (argv[1..argc-1], уже в UTF-8); второй вариант отбрасывает argv[0]
// и принимает узкие указатели.
//
// Потоки: в out — машинный результат (текстовый итог или --json), в err —
// диагностика и предупреждения. Разделение обязательно, иначе
// «rules validate --json > out.json» испортится прогрессом (SPEC §6.2:
// «--json» — машинный вывод).
//
// ---------------------------------------------------------------------------
// Зависимости и границы
// ---------------------------------------------------------------------------
// Только core (SPEC §6.1, ADR-004) и стандартная библиотека: ни WinHTTP, ни
// платформенных обёрток, ни engine. Файлы читаются через std::filesystem,
// окружение — через GetEnvironmentStringsW. Команда должна работать в CI на
// любой машине и не тянуть за собой весь платформенный слой.
//
// Приватный ключ Ed25519 живёт офлайн у владельца (SPEC §9.2), а сама
// криптографическая проверка подписи — не код этого файла: примитивов Ed25519
// в репозитории нет, и писать их здесь означало бы «реализацию, которую
// нечем проверить» (см. верх src/platform/rulesync_client.hpp, где верификатор
// специально сделан вводимым). Поэтому здесь есть шов: приложение или тест
// внедряет примитив через setRuleSignatureVerifier(). Пока внедрённого
// верификатора нет, «rules verify» говорит об этом прямо и возвращает 69, а
// не «успех»: «нечем проверить подпись» и «подпись верна» — разные утверждения.
//
// ---------------------------------------------------------------------------
// Мелочи, на которых легко ошибиться
// ---------------------------------------------------------------------------
//   * набор читается по МАНИФЕСТУ, а не «по всем *.json в каталоге»: файл, не
//     перечисленный в манифесте, — отказ (ADR-008), а не «лишний»;
//   * хеш считает ядро (core::rulesync::Sha256), и перед доверием любому
//     сравнению прогоняется core::sha256SelfTest(): модуль, который решает,
//     что удалять, не вправе молча доверять собственной криптографии;
//   * неразрешённые переменные окружения (%LOCALAPPDATA% и подобные при
//     запуске в CI) — не ошибка: правило просто неактивно. Это попадает в
//     warnings, чтобы набор не выглядел проверенным полностью;
//   * русский текст идёт в потоки как UTF-8; вывод в консоль переводится в
//     UTF-8 явно (SetConsoleOutputCP), иначе кириллица в cmd.exe — мусор.

#include "cmd_rules.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <new>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/rulesync.hpp"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace mrproper::cli {

namespace {

// Внедрённый верификатор. Переменная, а не параметр команды: зовут её и CLI,
// и тест, и UI, а команда одна. Тип Ed25519Verify объявлен в cmd_rules.hpp —
// там же объявлена и setRuleSignatureVerifier, регистрирующая этот шов.
Ed25519Verify g_signatureVerifier;

}  // namespace

// Определение регистрации шва; объявление — в cmd_rules.hpp.
void setRuleSignatureVerifier(Ed25519Verify verifier) { g_signatureVerifier = std::move(verifier); }

namespace {

// ---------------------------------------------------------------------------
// Коды возврата и константы
// ---------------------------------------------------------------------------

constexpr int kExitOk = 0;
constexpr int kExitUsage = 64;
constexpr int kExitDataError = 65;
constexpr int kExitNoInput = 66;
constexpr int kExitUnavailable = 69;
constexpr int kExitSoftware = 70;

// Потолок на один файл правил: набор — это десятки файлов по десятки
// килобайт (ядро допускает до core::kMaxManifestFiles = 4096 файлов), а
// мегабайтный «файл правил» — либо ошибка автора, либо попытка положить в
// правила файл с данными.
constexpr std::uintmax_t kMaxRuleFileBytes = 16u * 1024u * 1024u;

// Потолок на манифест: тот же, что у ядра, чтобы CLI и движок считали одно и
// то же «большим».
constexpr std::uintmax_t kMaxManifestBytes = core::kMaxManifestBytes;

constexpr const char* kDefaultSetDirectory = "rules";
constexpr const char* kDefaultManifestName = "manifest.json";
constexpr const char* kDefaultSignatureName = "rules.sig";
constexpr const char* kJsonExtension = ".json";
constexpr std::uintmax_t kMaxKeyFileBytes = 4u * 1024u;

// Версия приложения для шага 2 §9.2. CMake обязан передать её из
// project(MrProper VERSION …), иначе версия в отчёте и в --version разъедутся
// (packaging/README.md §3). Без определения берётся значение, совпадающее с
// project() на текущий момент.
#if defined(MRPROPER_VERSION_STRING)
constexpr const char* kAppVersion = MRPROPER_VERSION_STRING;
#else
constexpr const char* kAppVersion = "0.1.0";
#endif

// ---------------------------------------------------------------------------
// Пути и текст
// ---------------------------------------------------------------------------

// Пути приходят из argv в UTF-8 (SPEC §5 «не-ASCII»), а узкий конструктор
// std::filesystem::path на Windows использует текущую кодовую страницу ANSI:
// русский каталог превратился бы в мусор. Конструктор от char8_t всегда UTF-8.
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Расширение сравнивается без учёта регистра: на Windows «Temp.JSON» и
// «temp.json» — один файл, и набор, где они различаются, не должен проходить
// проверку молча.
bool hasJsonExtension(const std::filesystem::path& path) {
    return toLowerAscii(toUtf8(path.extension())) == kJsonExtension;
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

// «x/y» вместо «x\y»: ядро нормализует пути набора так же, и сводка проверки
// обязана показывать имена в том виде, в каком их сравнивают.
std::string normalizePath(std::string_view path) {
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

// Кавычки вокруг пути: пробелы в «C:\Program Files\…» не должны сливаться с
// окружающим текстом в логе.
std::string quoted(const std::filesystem::path& path) { return "\"" + toUtf8(path) + "\""; }

// ---------------------------------------------------------------------------
// Ввод-вывод
// ---------------------------------------------------------------------------

struct ReadResult {
    bool ok{};
    std::string bytes;
    std::string problem;
};

ReadResult readFileBytes(const std::filesystem::path& path, std::uintmax_t limit) {
    ReadResult result;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        result.problem = "не удалось узнать размер " + quoted(path) + ": " + ec.message();
        return result;
    }
    if (size > limit) {
        result.problem = "файл " + quoted(path) + " больше " + std::to_string(limit) + " байт";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.problem = "файл " + quoted(path) + " не открывается на чтение";
        return result;
    }
    std::string bytes;
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        stream.read(bytes.data(), static_cast<std::streamsize>(size));
        if (stream.gcount() != static_cast<std::streamsize>(size)) {
            result.problem = "файл " + quoted(path) + " прочитан не полностью";
            return result;
        }
    }
    result.bytes = std::move(bytes);
    result.ok = true;
    return result;
}

// Дамп окружения «NAME=value\n» — тот же формат, который ждёт
// core::loadVerifiedRuleSet. Скрытые переменные Windows («=C:=C:\…») в дамп
// не попадают: имя начинается с «=» и ни одному локатору правил не отвечает.
std::string environmentDump() {
#if defined(_WIN32)
    LPWCH block = ::GetEnvironmentStringsW();
    if (block == nullptr) return {};
    std::string out;
    for (const wchar_t* cursor = block; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1u) {
        const std::wstring_view entry(cursor);
        if (entry.empty() || entry.front() == L'=') continue;
        const std::size_t equals = entry.find(L'=');
        if (equals == std::wstring_view::npos || equals == 0) continue;

        const std::wstring_view name = entry.substr(0, equals);
        const std::wstring_view value = entry.substr(equals + 1u);
        const int nameBytes =
            ::WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
        const int valueBytes = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                                     nullptr, 0, nullptr, nullptr);
        if (nameBytes <= 0 || valueBytes <= 0) continue;

        std::string record;
        record.resize(static_cast<std::size_t>(nameBytes) + static_cast<std::size_t>(valueBytes) + 2u);
        ::WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), record.data(), nameBytes,
                              nullptr, nullptr);
        std::size_t offset = static_cast<std::size_t>(nameBytes);
        record[offset++] = '=';
        ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), record.data() + offset,
                              valueBytes, nullptr, nullptr);
        offset += static_cast<std::size_t>(valueBytes);
        record[offset++] = '\n';
        out.append(record, 0, offset);
    }
    ::FreeEnvironmentStringsW(block);
    return out;
#else
    return {};
#endif
}

// Имена переменных окружения, которых нет в дампе: по ним видно, почему
// правило молчит. Собираются из самих локаторов, поэтому список «чего не
// хватает» берётся из правил, а не из выдуманного перечня.
std::set<std::string> missingEnvironmentVariables(const core::RuleSet& set, const std::string& env) {
    std::set<std::string> present;
    std::size_t begin = 0;
    while (begin < env.size()) {
        std::size_t end = env.find('\n', begin);
        if (end == std::string::npos) end = env.size();
        const std::string line = env.substr(begin, end - begin);
        begin = end + 1u;
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos) present.insert(line.substr(0, equals));
    }

    std::set<std::string> missing;
    const auto collect = [&missing, &present](std::string_view text) {
        std::size_t pos = text.find('%');
        while (pos != std::string_view::npos) {
            const std::size_t close = text.find('%', pos + 1u);
            if (close == std::string_view::npos) return;
            const std::string name(text.substr(pos + 1u, close - pos - 1u));
            if (!name.empty() && present.find(name) == present.end()) missing.insert(name);
            pos = text.find('%', close + 1u);
        }
    };
    for (const core::Rule& rule : set.rules) {
        collect(rule.locator);
        for (const std::string& exclude : rule.locatorExcludes) collect(exclude);
    }
    return missing;
}

// ---------------------------------------------------------------------------
// base64 (подпись и публичный ключ приезжают в base64 — tools/rule_keys.md §1)
// ---------------------------------------------------------------------------

// Строгий разбор: допустимы только A–Z a–z 0–9 + «/» и выравнивание «=».
// Пробельные символы по краям отбрасываются (файл мог прийти с переводом
// строки), внутри — отказ: молча вычищать мусор из подписи нельзя.
std::optional<std::vector<std::uint8_t>> decodeBase64Strict(std::string_view text) {
    const auto isSpace = [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; };
    std::string_view body = text;
    while (!body.empty() && isSpace(body.front())) body.remove_prefix(1);
    while (!body.empty() && isSpace(body.back())) body.remove_suffix(1);
    if (body.empty() || body.size() % 4u != 0u) return std::nullopt;

    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kAlphabet[i])] = i;

    std::vector<std::uint8_t> out;
    out.reserve(body.size() / 4u * 3u);
    for (std::size_t i = 0; i < body.size(); i += 4u) {
        int values[4] = {0, 0, 0, 0};
        std::size_t padding = 0;
        for (std::size_t j = 0; j < 4u; ++j) {
            const char c = body[i + j];
            if (c == '=') {
                // «=» допустим только двумя последними символами последней группы.
                if (i + 4u != body.size() || j < 2u) return std::nullopt;
                ++padding;
                continue;
            }
            if (padding > 0) return std::nullopt;
            const int value = table[static_cast<unsigned char>(c)];
            if (value < 0) return std::nullopt;
            values[j] = value;
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(values[0]) << 18) |
                                     (static_cast<std::uint32_t>(values[1]) << 12) |
                                     (static_cast<std::uint32_t>(values[2]) << 6) |
                                     static_cast<std::uint32_t>(values[3]);
        out.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xFFu));
        if (padding < 2u) out.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xFFu));
        if (padding < 1u) out.push_back(static_cast<std::uint8_t>(triple & 0xFFu));
    }
    return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> decodeFixedLength(std::string_view base64, std::string& problem) {
    const std::optional<std::vector<std::uint8_t>> bytes = decodeBase64Strict(base64);
    if (!bytes) {
        problem = "не base64: ожидаются только A-Z a-z 0-9, «+», «/» и выравнивание «=»";
        return std::nullopt;
    }
    if (bytes->size() != N) {
        problem = "ожидалось " + std::to_string(N) + " байт, получено " + std::to_string(bytes->size());
        return std::nullopt;
    }
    std::array<std::uint8_t, N> out{};
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return out;
}

// ---------------------------------------------------------------------------
// Набор на диске
// ---------------------------------------------------------------------------

struct SetOnDisk {
    std::filesystem::path directory;
    std::filesystem::path manifestPath;
    std::string manifestBytes;
    std::vector<core::VerifiedFile> files;                     // для сверки с манифестом
    std::vector<std::pair<std::string, std::string>> contents;  // путь из манифеста → байты
    std::vector<std::string> ignored;                          // не *.json — предупреждение, не отказ
    std::string problem;                                       // не читается — отказ
};

// Прочитать каталог набора: манифест и все *.json, кроме самого манифеста.
// Файлы, не перечисленные в манифесте, сюда тоже попадают: их отказ выносит
// ядро (requireKnownFilesOnly), а не молчаливый фильтр здесь.
SetOnDisk readSet(const std::filesystem::path& directory, const std::filesystem::path& manifestPath) {
    SetOnDisk set;
    set.directory = directory;
    set.manifestPath = manifestPath;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        set.problem = "каталог набора не найден: " + quoted(directory);
        return set;
    }

    const ReadResult manifest = readFileBytes(manifestPath, kMaxManifestBytes);
    if (!manifest.ok) {
        set.problem = manifest.problem;
        return set;
    }
    set.manifestBytes = manifest.bytes;

    std::vector<std::filesystem::path> ruleFiles;
    for (std::filesystem::directory_iterator it(directory, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            set.problem = "не удалось перечислить " + quoted(directory) + ": " + ec.message();
            return set;
        }
        if (ec || !it->is_regular_file(ec)) continue;
        if (!hasJsonExtension(it->path())) {
            // rules.sig — нормальная часть набора, его отсутствие в этом месте
            // не приговор: на отсутствие подписи отвечает шаг 3 §9.2.
            if (!equalsIgnoreCase(toUtf8(it->path().filename()), kDefaultSignatureName)) {
                set.ignored.push_back(toUtf8(it->path().filename()));
            }
            continue;
        }
        if (it->path().filename() == manifestPath.filename()) continue;
        ruleFiles.push_back(it->path());
    }
    // Порядок чтения — по имени: вывод проверки должен быть детерминированным
    // (SPEC §11.4, golden-тесты).
    std::sort(ruleFiles.begin(), ruleFiles.end(),
              [](const std::filesystem::path& left, const std::filesystem::path& right) {
                  return toUtf8(left.filename()) < toUtf8(right.filename());
              });

    for (const std::filesystem::path& path : ruleFiles) {
        const ReadResult read = readFileBytes(path, kMaxRuleFileBytes);
        if (!read.ok) {
            set.problem = read.problem;
            return set;
        }
        const std::string name = normalizePath(toUtf8(path.filename()));
        set.files.push_back(core::makeVerifiedFile(name, read.bytes));
        set.contents.emplace_back(name, std::move(read.bytes));
    }
    std::sort(set.ignored.begin(), set.ignored.end());
    return set;
}

// ---------------------------------------------------------------------------
// Разбор аргументов
// ---------------------------------------------------------------------------

enum class Command { Help, Validate, Verify };

struct Options {
    Command command{Command::Help};
    bool help{};
    std::filesystem::path setDirectory{pathFromUtf8(kDefaultSetDirectory)};
    std::filesystem::path manifestPath{};
    std::filesystem::path signaturePath{};
    std::filesystem::path publicKeyPath{};
    std::string appVersion{kAppVersion};
    bool json{};
    bool quiet{};
    bool verbose{};
    bool useEnvironment{true};
    bool allowUnverifiedSignature{};
    // Подкоманда названа, но её нет (сейчас — «update»). Отдельный флаг, а не
    // problem: код отказа другой (69, а не 64), потому что аргументы здесь
    // вовсе не неверные — такой команды просто не существует.
    bool unavailable{};
    std::vector<std::string> warnings;  // наполняется по ходу проверки
    std::string problem;                // пусто — разбор удался
};

struct ArgsCursor {
    const std::vector<std::string>& items;
    std::size_t position{0};

    [[nodiscard]] bool done() const { return position >= items.size(); }
};

bool takeValue(ArgsCursor& cursor, const std::string& flag, std::string& value, std::string& problem) {
    // Главный цикл разбора прочитал имя опции как items[position++], поэтому
    // position уже указывает на СЛЕДУЮЩЕЕ слово — на значение. Значит:
    //   * «значения нет» — это ровно position == items.size();
    //   * значение читается по текущему position, и только потом position
    //     двигается дальше.
    // Чтение по position + 1 (как было) уходило на один элемент за конец
    // вектора: в Debug это «vector subscript out of range» на
    // «rules validate --set nosuchdir», в Release — тихо мусор в пути.
    if (cursor.position >= cursor.items.size()) {
        problem = "опция " + flag + " требует значения";
        return false;
    }
    value = cursor.items[cursor.position];
    ++cursor.position;
    return true;
}

Options parseArgs(const std::vector<std::string>& argv) {
    Options options;
    ArgsCursor cursor{argv, 0};
    bool commandSeen = false;
    bool positionalSeen = false;

    while (!cursor.done()) {
        const std::string item = cursor.items[cursor.position++];
        if (item == "-h" || item == "--help") {
            options.command = Command::Help;
            options.help = true;
            return options;
        }
        if (item == "validate" && !commandSeen) {
            options.command = Command::Validate;
            commandSeen = true;
            continue;
        }
        if (item == "verify" && !commandSeen) {
            options.command = Command::Verify;
            commandSeen = true;
            continue;
        }
        if (item == "update" && !commandSeen) {
            // «rules update» не молча превращается в проверку каталога с
            // именем «update»: такой вызов читается как «обнови набор», и
            // разбор его как позиционного пути приводил к отказу «каталог не
            // найден: update» — то есть к сообщению о ерунде вместо ответа на
            // реальный вопрос. Обновление действительно есть, но не здесь:
            // §9.2 п.1 — модуль rulesync при запуске приложения (не чаще раза
            // в 24 ч), §9.2 п.5 — кнопки «проверить сейчас» и «вернуть
            // встроенный набор» в настройках.
            //
            // Проверка «!commandSeen» обязательна: «rules verify update» — это
            // проверка набора из каталога, который автор назвал update, и
            // ломать её нельзя.
            options.unavailable = true;
            options.command = Command::Help;
            options.problem =
                "«update» — не подкоманда rules: обновление набора выполняет модуль rulesync "
                "при запуске приложения (§9.2 п.1) и кнопки в настройках (§9.2 п.5), "
                "а из командной строки доступны validate и verify";
            return options;
        }
        if (item == "--set" || item == "--rules-dir" || item == "--dir") {
            std::string value;
            if (!takeValue(cursor, item, value, options.problem)) return options;
            options.setDirectory = pathFromUtf8(value);
            continue;
        }
        if (item == "--manifest" || item == "--signature" || item == "--public-key") {
            std::string value;
            if (!takeValue(cursor, item, value, options.problem)) return options;
            if (item == "--manifest") {
                options.manifestPath = pathFromUtf8(value);
            } else if (item == "--signature") {
                options.signaturePath = pathFromUtf8(value);
            } else {
                options.publicKeyPath = pathFromUtf8(value);
            }
            continue;
        }
        if (item == "--app-version") {
            if (!takeValue(cursor, item, options.appVersion, options.problem)) return options;
            continue;
        }
        if (item == "--json") {
            options.json = true;
            continue;
        }
        if (item == "--quiet" || item == "-q") {
            options.quiet = true;
            continue;
        }
        if (item == "--verbose" || item == "-v") {
            options.verbose = true;
            continue;
        }
        if (item == "--no-env") {
            options.useEnvironment = false;
            continue;
        }
        if (item == "--no-signature" || item == "--allow-unverified") {
            options.allowUnverifiedSignature = true;
            continue;
        }
        if (!item.empty() && item.front() == '-') {
            options.problem = "неизвестная опция «" + item + "»";
            return options;
        }
        // Позиционный аргумент — каталог набора. Второй позиционный — ошибка:
        // опечатка в скрипте CI не должна молча указывать не туда.
        if (positionalSeen) {
            options.problem = "лишний аргумент «" + item + "», ожидался один каталог набора";
            return options;
        }
        options.setDirectory = pathFromUtf8(item);
        positionalSeen = true;
    }

    if (!commandSeen) {
        options.problem = "не указана подкоманда: validate или verify";
        options.command = Command::Help;
    }
    return options;
}

// ---------------------------------------------------------------------------
// Вывод
// ---------------------------------------------------------------------------

const char* const kUsage =
    "mrproper-cli rules — проверка набора правил очистки (SPEC §9.2, ADR-008)\n"
    "\n"
    "  rules validate [каталог] [опции]  набор разбирается парсером правил: манифест,\n"
    "                                   SHA-256 и размер каждого файла, уникальность\n"
    "                                   id, совпадение schemaVersion и версий\n"
    "  rules verify   [каталог] [опции]  полная цепочка §9.2: схема манифеста,\n"
    "                                   minAppVersion, подпись Ed25519, SHA-256\n"
    "                                   каждого файла, разбор правил\n"
    "\n"
    "Подкоманды update у rules нет: обновление набора из сети выполняет модуль\n"
    "rulesync при запуске приложения (§9.2 п.1, не чаще раза в 24 ч), а «проверить\n"
    "сейчас» и «вернуть встроенный набор» — кнопки в настройках (§9.2 п.5).\n"
    "Из командной строки набор только проверяют, поэтому «rules update» даёт\n"
    "отказ с кодом 69, а не проверку каталога с именем update.\n"
    "\n"
    "Опции:\n"
    "  --set <каталог>        каталог набора (по умолчанию rules)\n"
    "  --manifest <файл>      манифест (по умолчанию <каталог>\\manifest.json)\n"
    "  --signature <файл>     подпись, base64 от 64 байт (verify; по умолчанию\n"
    "                         <каталог>\\rules.sig)\n"
    "  --public-key <файл>    публичный ключ, base64 от 32 байт (verify)\n"
    "  --app-version <версия> версия приложения для minAppVersion (по умолчанию —\n"
    "                         версия сборки)\n"
    "  --json                 машинный вывод в stdout, диагностика в stderr\n"
    "  --verbose, -v          построчный вывод по каждому файлу набора\n"
    "  --quiet, -q            без текстового итога, только ошибки\n"
    "  --no-env               не подставлять переменные окружения в локаторы\n"
    "  --no-signature         verify без шага подписи (только для разработки, не\n"
    "                         для приёмки набора в CI)\n"
    "  -h, --help             эта справка\n"
    "\n"
    "Коды возврата:\n"
    "  0 успех   64 неверные аргументы   65 данные не годятся\n"
    "  66 нет входа   69 проверка невозможна (в том числе неизвестная подкоманда)\n"
    "  70 внутренняя ошибка\n";

// ---------------------------------------------------------------------------

const char* fileCheckToken(core::FileCheck check) {
    switch (check) {
        case core::FileCheck::Ok: return "ok";
        case core::FileCheck::Missing: return "missing";
        case core::FileCheck::SizeMismatch: return "size-mismatch";
        case core::FileCheck::HashMismatch: return "hash-mismatch";
        case core::FileCheck::Unlisted: return "unlisted";
        case core::FileCheck::Duplicate: return "duplicate";
    }
    return "unknown";
}

std::size_t countOkFiles(const core::RuleSetVerification& verification) {
    std::size_t ok = 0;
    for (const core::FileVerdict& verdict : verification.files) {
        if (verdict.check == core::FileCheck::Ok) ++ok;
    }
    return ok;
}

// Сводка проверки в тексте: каждая строка помечена шагом §9.2, чтобы вывод
// читался как отчёт о цепочке, а не как набор утверждений.
void printText(std::ostream& out, const Options& options, const std::string& command, const SetOnDisk& set,
               const core::RuleSetVerification& verification, std::size_t ruleCount, const std::string& signatureDetail) {
    if (options.quiet) return;
    out << "Команда:   " << command << '\n';
    out << "Набор:     " << toUtf8(set.directory) << '\n';
    out << "Манифест:  " << toUtf8(set.manifestPath) << " (" << set.manifestBytes.size() << " байт)\n";
    if (verification.manifestParsed) {
        out << "Версия:    " << verification.manifest.version << " (схема " << verification.manifest.schemaVersion
            << ", minAppVersion "
            << (verification.manifest.minAppVersion.empty() ? "—" : verification.manifest.minAppVersion)
            << ", версия приложения " << options.appVersion << ")\n";
    }
    out << "Шаг 1. Схема манифеста:   " << (verification.schemaOk ? "ок" : "НЕ ПОДДЕРЖИВАЕТСЯ") << '\n';
    out << "Шаг 2. minAppVersion:     " << (verification.versionOk ? "ок" : verification.versionDetail) << '\n';
    out << "Шаг 3. Подпись:           " << signatureDetail << '\n';
    out << "Шаг 4. Файлы:            " << countOkFiles(verification) << " из " << verification.files.size()
        << " совпали с манифестом (SHA-256 и размер)\n";
    if (options.verbose) {
        for (const core::FileVerdict& verdict : verification.files) {
            out << "                 " << (verdict.check == core::FileCheck::Ok ? "ок   " : "ОТКАЗ")
                << "  " << verdict.path << " — " << verdict.describe() << '\n';
        }
    }
    out << "Шаг 5. Правила:          ";
    if (ruleCount == 0) {
        out << "не разобраны\n";
    } else {
        out << ruleCount << " правил разобрано, идентификаторы уникальны\n";
    }
}

std::string buildJson(const Options& options, const std::string& command, const SetOnDisk& set,
                      const core::RuleSetVerification& verification, std::size_t ruleCount,
                      const std::string& signatureDetail, const std::vector<std::string>& problems, int exitCode) {
    std::vector<json::Value> files;
    if (options.verbose) {
        files.reserve(verification.files.size());
        for (const core::FileVerdict& verdict : verification.files) {
            files.push_back(json::Value::object({
                {"path", json::Value(verdict.path)},
                {"check", json::Value(fileCheckToken(verdict.check))},
                {"ok", json::Value(verdict.check == core::FileCheck::Ok)},
                {"detail", json::Value(verdict.describe())},
            }));
        }
    }

    std::vector<json::Value> problemValues;
    problemValues.reserve(problems.size());
    for (const std::string& problem : problems) problemValues.push_back(json::Value(problem));
    std::vector<json::Value> warningValues;
    warningValues.reserve(options.warnings.size());
    for (const std::string& warning : options.warnings) warningValues.push_back(json::Value(warning));

    const json::Value signature = json::Value::object({
        {"checked", json::Value(verification.signatureChecked)},
        {"ok", json::Value(verification.signatureOk)},
        {"detail", json::Value(signatureDetail)},
    });

    const json::Value root = json::Value::object({
        {"command", json::Value(command)},
        {"ok", json::Value(exitCode == kExitOk)},
        {"set", json::Value(toUtf8(set.directory))},
        {"manifest", json::Value(toUtf8(set.manifestPath))},
        {"manifestBytes", json::Value(static_cast<double>(set.manifestBytes.size()))},
        {"version", verification.manifestParsed ? json::Value(verification.manifest.version) : json::Value()},
        {"schema", verification.manifestParsed ? json::Value(verification.manifest.schemaVersion) : json::Value()},
        {"minAppVersion", verification.manifestParsed ? json::Value(verification.manifest.minAppVersion) : json::Value()},
        {"appVersion", json::Value(options.appVersion)},
        {"fileCount", json::Value(static_cast<double>(verification.files.size()))},
        {"filesOk", json::Value(static_cast<double>(countOkFiles(verification)))},
        {"ruleCount", json::Value(static_cast<double>(ruleCount))},
        {"signature", signature},
        {"files", json::Value::array(std::move(files))},
        {"problems", json::Value::array(std::move(problemValues))},
        {"warnings", json::Value::array(std::move(warningValues))},
        {"exitCode", json::Value(exitCode)},
    });

    std::string text = root.dump(2);
    text.push_back('\n');
    return text;
}

// ---------------------------------------------------------------------------
// Проверка
// ---------------------------------------------------------------------------

// Шаг 3 §9.2 разложен на четыре исхода, и различать их обязательно: «подписи
// нет», «подпись битая» и «проверять нечем» — три разные причины, а для CI
// это три разных решения.
enum class SignatureStep { Skipped, Ready, NoInput, NoVerifier };

// Внедряет в политику ядра проверку подписи. Всё, что можно проверить без
// примитива (файл на месте, это base64 от 64 байт, ключ — 32 ненулевых байта),
// проверяется здесь; криптографию делает внедрённая функция.
SignatureStep prepareSignature(const Options& options, const SetOnDisk& set, core::VerificationPolicy& policy,
                               std::string& signatureText, std::string& detail, std::string& problem) {
    const std::filesystem::path signaturePath =
        options.signaturePath.empty() ? options.setDirectory / kDefaultSignatureName : options.signaturePath;

    const ReadResult signature = readFileBytes(signaturePath, kMaxKeyFileBytes);
    if (!signature.ok) {
        problem = "подпись не прочитана: " + signature.problem;
        detail = "нет файла подписи (" + toUtf8(signaturePath) + ")";
        return SignatureStep::NoInput;
    }

    std::string localProblem;
    const std::optional<std::array<std::uint8_t, 64>> rawSignature = decodeFixedLength<64>(signature.bytes, localProblem);
    if (!rawSignature) {
        problem = "подпись \"" + toUtf8(signaturePath) + "\": " + localProblem;
        detail = "подпись не декодируется";
        return SignatureStep::NoInput;
    }

    if (options.publicKeyPath.empty()) {
        problem = "публичный ключ не задан: укажите --public-key (base64 от 32 байт)";
        detail = "подпись есть, ключа нет";
        return SignatureStep::NoInput;
    }
    const ReadResult keyFile = readFileBytes(options.publicKeyPath, kMaxKeyFileBytes);
    if (!keyFile.ok) {
        problem = "публичный ключ не прочитан: " + keyFile.problem;
        detail = "ключ не читается";
        return SignatureStep::NoInput;
    }
    std::string keyProblem;
    const std::optional<std::array<std::uint8_t, 32>> key = decodeFixedLength<32>(keyFile.bytes, keyProblem);
    if (!key) {
        problem = "публичный ключ \"" + toUtf8(options.publicKeyPath) + "\": " + keyProblem;
        detail = "ключ не декодируется";
        return SignatureStep::NoInput;
    }
    const bool zeroKey = std::all_of(key->begin(), key->end(), [](std::uint8_t byte) { return byte == 0; });
    if (zeroKey) {
        problem = "публичный ключ \"" + toUtf8(options.publicKeyPath) + "\" состоит из нулей";
        detail = "ключ пустой";
        return SignatureStep::NoInput;
    }

    if (!g_signatureVerifier) {
        problem = "криптографическая проверка подписи недоступна: Ed25519-верификатор не внедрён "
                  "(setRuleSignatureVerifier) — проверить подпись нечем";
        detail = "подпись и ключ на месте, верификатора нет";
        return SignatureStep::NoVerifier;
    }

    const Ed25519Verify verifySignature = g_signatureVerifier;
    const std::array<std::uint8_t, 32> publicKey = *key;
    signatureText = signature.bytes;
    // Манифест проверяется теми байтами, которые подписывались: аргумент
    // signatureBytes ядро передаёт как есть (base64 файла подписи), а
    // манифест берётся из набора на диске.
    policy.verifySignature = [&verifySignature, &set, publicKey](std::string_view,
                                                                std::string_view signatureBytes) {
        std::string local;
        const std::optional<std::array<std::uint8_t, 64>> raw = decodeFixedLength<64>(signatureBytes, local);
        if (!raw) return false;
        return verifySignature(set.manifestBytes, publicKey, *raw);
    };
    detail = "проверяется: " + toUtf8(signaturePath) + " (64 байта), ключ " + toUtf8(options.publicKeyPath);
    return SignatureStep::Ready;
}

int runCheck(Options& options, const std::string& command, std::ostream& out, std::ostream& err) {
    const bool verify = command == "verify";

    // Своя криптография не проверена — сравнения хешей не будет вообще.
    if (!core::sha256SelfTest()) {
        err << "ОШИБКА: SHA-256 не проходит самопроверку (core::sha256SelfTest): хешам доверять нельзя\n";
        return kExitSoftware;
    }

    const std::filesystem::path manifestPath =
        options.manifestPath.empty() ? options.setDirectory / kDefaultManifestName : options.manifestPath;
    const SetOnDisk set = readSet(options.setDirectory, manifestPath);
    if (!set.problem.empty()) {
        err << "ОШИБКА: " << set.problem << '\n';
        return kExitNoInput;
    }
    for (const std::string& ignored : set.ignored) {
        options.warnings.push_back("файл \"" + ignored + "\" не *.json и в проверку набора не входит");
    }

    // Политика шага 3. По умолчанию подпись обязательна в verify и не нужна в
    // validate; --no-signature снимает её осознанно и попадает в warnings.
    core::VerificationPolicy policy;
    policy.requireKnownFilesOnly = true;
    policy.requireSignature = false;
    std::string signatureText;
    std::string signatureDetail = "не проверялась: validate проверяет разбор набора, а не подпись";
    SignatureStep signatureStep = SignatureStep::Skipped;
    std::vector<std::string> problems;

    if (verify) {
        if (options.allowUnverifiedSignature) {
            options.warnings.push_back(
                "--no-signature: подпись не проверялась, такой результат не годится для приёмки набора в CI");
            signatureDetail = "пропущена (--no-signature)";
        } else {
            policy.requireSignature = true;
            std::string signatureProblem;
            signatureStep = prepareSignature(options, set, policy, signatureText, signatureDetail, signatureProblem);
            if (signatureStep == SignatureStep::NoInput || signatureStep == SignatureStep::NoVerifier) {
                problems.push_back(signatureProblem);
                if (signatureStep == SignatureStep::NoInput) {
                    // Шаг 3 не выполнен по входным данным: ядру сказать «подпись
                    // не требуется» честнее, чем «верификатор не задан».
                    policy.requireSignature = false;
                    signatureText.clear();
                }
            }
        }
    }

    const core::RuleSetVerification verification =
        core::verifyRuleSet(set.manifestBytes, signatureText, set.files, options.appVersion, policy);
    for (const std::string& problem : verification.problems) problems.push_back(problem);

    // Шаг 5 §9.2 п.3: парсер правил. Неизвестное поле и битый JSON здесь уже
    // ошибка: после проверки целостности это не «сеть», а содержимое набора.
    std::size_t ruleCount = 0;
    if (problems.empty()) {
        const std::string env = options.useEnvironment ? environmentDump() : std::string();
        try {
            bool unresolved = false;
            const core::RuleSet parsed = core::loadVerifiedRuleSet(verification, set.contents, env, &unresolved);
            core::validateRuleSet(parsed);
            ruleCount = parsed.size();
            if (ruleCount == 0) {
                problems.push_back("набор не содержит ни одного правила");
            }
            if (unresolved && options.useEnvironment) {
                const std::set<std::string> missing = missingEnvironmentVariables(parsed, env);
                std::string names;
                for (const std::string& name : missing) {
                    if (!names.empty()) names += ", ";
                    names += name;
                }
                options.warnings.push_back("часть правил не раскрылась: нет переменных окружения " +
                                           (names.empty() ? std::string("(перечень пуст)") : names) +
                                           " — такие правила неактивны");
            } else if (unresolved) {
                options.warnings.push_back("--no-env: локаторы правил не раскрывались");
            }
        } catch (const std::exception& error) {
            problems.push_back(std::string("набор не разбирается парсером правил: ") + error.what());
        }
    }

    int exitCode = kExitOk;
    if (!problems.empty()) {
        exitCode = signatureStep == SignatureStep::NoVerifier ? kExitUnavailable : kExitDataError;
    }

    for (const std::string& warning : options.warnings) err << "ПРЕДУПРЕЖДЕНИЕ: " << warning << '\n';
    for (const std::string& problem : problems) err << "ОШИБКА: " << problem << '\n';

    printText(out, options, command, set, verification, ruleCount, signatureDetail);
    if (options.json) out << buildJson(options, command, set, verification, ruleCount, signatureDetail, problems, exitCode);
    return exitCode;
}

}  // namespace

// ---------------------------------------------------------------------------
// Точки входа
// ---------------------------------------------------------------------------

int cmdRules(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
#if defined(_WIN32)
    // Русский текст в cmd.exe: без этого консоль печатает мусор. При
    // перенаправлении в файл кодовая страница не нужна, но и не вредит.
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    Options options = parseArgs(args);
    if (options.help) {
        out << kUsage;
        return kExitOk;
    }
    if (options.unavailable) {
        err << "ОШИБКА: " << options.problem << "\n\n" << kUsage;
        return kExitUnavailable;
    }
    if (!options.problem.empty()) {
        err << "ОШИБКА: " << options.problem << "\n\n" << kUsage;
        return kExitUsage;
    }
    if (options.command == Command::Help) {
        out << kUsage;
        return kExitOk;
    }

    try {
        return runCheck(options, options.command == Command::Verify ? "verify" : "validate", out, err);
    } catch (const std::bad_alloc&) {
        err << "ОШИБКА: не хватило памяти\n";
        return kExitSoftware;
    } catch (const std::exception& error) {
        err << "ОШИБКА: " << error.what() << '\n';
        return kExitSoftware;
    }
}

int cmdRules(int argc, char** argv, std::ostream& out, std::ostream& err) {
    std::vector<std::string> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    return cmdRules(args, out, err);
}

}  // namespace mrproper::cli
