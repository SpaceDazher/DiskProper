// Проверка и откат набора правил: манифест, SHA-256 каждого файла, версия
// схемы, политика отката (SPEC §9.2, ADR-008).
//
// Контракт, порядок проверок и причины решений описаны в rulesync.hpp —
// здесь реализация. Модуль переносимый: ни Windows API, ни сети, ни ФС.
#include "rulesync.hpp"

#include <algorithm>
#include <cstring>
#include <map>

#include "json.hpp"

namespace mrproper::core {
namespace {

// ------------------------------------------------------- SHA-256, FIPS 180-4

constexpr std::uint32_t kSha256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

// Эталонные ответы NIST FIPS 180-4 (приложение B): пустая строка и «abc».
constexpr char kSha256OfEmpty[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr char kSha256OfAbc[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// Поворот вправо. bits всегда из [1,31] (см. вызовы ниже), проверка оставлена,
// чтобы UB (сдвиг на 32) был недостижим даже после правки расписания.
constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) {
    if (bits == 0u || bits >= 32u) return value;
    return (value >> bits) | (value << (32u - bits));
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ----------------------------------------------------------- разбор манифеста

const std::vector<std::string>& manifestFields() {
    // «schema» — имя из SPEC §9.2, «schemaVersion» — как в файлах правил
    // (core::rules), чтобы одно и то же поле не называлось двумя способами.
    static const std::vector<std::string> kFields = {"schema", "schemaVersion", "version", "minAppVersion", "files"};
    return kFields;
}

const std::vector<std::string>& manifestFileFields() {
    static const std::vector<std::string> kFields = {"path", "sha256", "size"};
    return kFields;
}

void rejectUnknownFields(const json::Value& node, const std::vector<std::string>& allowed, const std::string& origin) {
    for (const auto& member : node.members()) {
        if (std::find(allowed.begin(), allowed.end(), member.first) == allowed.end()) {
            // Неизвестное поле — ошибка, а не предупреждение: набор приходит из
            // сети и управляет удалением файлов (ADR-008).
            throw RuleSyncError(origin + ": неизвестное поле \"" + member.first + "\"");
        }
    }
}

const json::Value& requireNode(const json::Value& parent, std::string_view key, const std::string& origin) {
    const json::Value* node = parent.find(key);
    if (node == nullptr) throw RuleSyncError(origin + ": нет обязательного поля \"" + std::string(key) + "\"");
    return *node;
}

const std::string& requireString(const json::Value& parent, std::string_view key, const std::string& origin) {
    const json::Value& node = requireNode(parent, key, origin);
    if (!node.isString()) throw RuleSyncError(origin + ": поле \"" + std::string(key) + "\" должно быть строкой");
    return node.asString();
}

std::uint64_t requireCount(const json::Value& parent, std::string_view key, const std::string& origin) {
    const json::Value& node = requireNode(parent, key, origin);
    if (!node.isNumber()) throw RuleSyncError(origin + ": поле \"" + std::string(key) + "\" должно быть числом");
    const double raw = node.asNumber();
    // 2^53 — предел точности double: всё, что дальше, читать нельзя.
    if (!(raw >= 0.0) || raw > 9007199254740992.0) {
        throw RuleSyncError(origin + ": поле \"" + std::string(key) + "\" должно быть неотрицательным целым числом");
    }
    const auto value = static_cast<std::uint64_t>(raw);
    if (static_cast<double>(value) != raw) {
        throw RuleSyncError(origin + ": поле \"" + std::string(key) + "\" должно быть целым числом");
    }
    return value;
}

std::string normalizeRulePath(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) out.push_back(c == '\\' ? '/' : c);
    return out;
}

// Относительный путь внутри набора. «../», абсолютные пути, диски, «//»,
// управляющие символы и символы, недопустимые в именах Windows, — отказ:
// манифест приходит из сети и указывает, какие файлы читать движку очистки.
bool isSafeRelativePath(std::string_view path) {
    if (path.empty() || path.size() > 200u) return false;
    if (path.front() == '/' || path.front() == '\\') return false;
    if (path.find_first_of("<>:\"|?*") != std::string_view::npos) return false;
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20u || byte == 0x7fu) return false;
    }
    std::string_view rest = path;
    while (true) {
        const std::size_t slash = rest.find_first_of("/\\");
        const std::string_view component = rest.substr(0, slash);
        if (component.empty()) return false;  // «//» или хвостовой разделитель
        if (component == "." || component == "..") return false;
        // Windows молча срезает хвостовые пробелы и точки: «a.json.» и «a. json »
        // указали бы на другой файл, чем указано в манифесте.
        if (component.back() == ' ' || component.back() == '.') return false;
        if (slash == std::string_view::npos) break;
        rest = rest.substr(slash + 1);
    }
    return true;
}

// ------------------------------------------------------------------ версии

std::vector<long long> versionComponents(std::string_view version) {
    std::vector<long long> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = version.find('.', start);
        const std::string_view piece = version.substr(start, dot == std::string_view::npos ? std::string_view::npos
                                                                                           : dot - start);
        if (piece.empty()) {
            throw RuleSyncError("версия \"" + std::string(version) + "\" пуста или заканчивается точкой");
        }
        long long value = 0;
        for (const char c : piece) {
            if (c < '0' || c > '9') {
                throw RuleSyncError("версия \"" + std::string(version) + "\" не числовая");
            }
            value = value * 10 + (c - '0');
            if (value > 1000000000LL) {
                throw RuleSyncError("компонент версии в \"" + std::string(version) + "\" неправдоподобно велик");
            }
        }
        parts.push_back(value);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

const char* fileCheckName(FileCheck check) {
    switch (check) {
        case FileCheck::Ok: return "ok";
        case FileCheck::Missing: return "missing";
        case FileCheck::SizeMismatch: return "size";
        case FileCheck::HashMismatch: return "hash";
        case FileCheck::Unlisted: return "unlisted";
        case FileCheck::Duplicate: return "duplicate";
    }
    return "unknown";
}

}  // namespace

// --------------------------------------------------------------------- SHA-256

Sha256::Sha256() {
    state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Sha256::compress(const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) | static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + choose + kSha256RoundConstants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t major = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + major;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
    // После finish() добавление запрещено: подписанный дайджес измениться не может.
    if (finished_ || data == nullptr) return;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    totalBits_ += static_cast<std::uint64_t>(size) * 8u;

    std::size_t offset = 0;
    if (buffered_ > 0) {
        const std::size_t take = std::min(size, std::size_t{64} - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes, take);
        buffered_ += take;
        offset = take;
        if (buffered_ == 64) {
            compress(buffer_.data());
            buffered_ = 0;
        }
    }
    while (offset + 64 <= size) {
        compress(bytes + offset);
        offset += 64;
    }
    if (offset < size) {
        const std::size_t rest = size - offset;
        std::memcpy(buffer_.data() + buffered_, bytes + offset, rest);
        buffered_ += rest;
    }
}

void Sha256::update(std::string_view text) { update(text.data(), text.size()); }

Sha256Digest Sha256::finish() {
    if (!finished_) {
        const std::uint64_t bitLength = totalBits_;
        const std::uint8_t one = static_cast<std::uint8_t>(0x80u);
        const std::uint8_t zero = static_cast<std::uint8_t>(0x00u);
        update(&one, 1);
        while (buffered_ != 56) update(&zero, 1);
        std::uint8_t tail[8];
        for (std::size_t i = 0; i < 8; ++i) {
            tail[i] = static_cast<std::uint8_t>((bitLength >> (56u - 8u * static_cast<unsigned>(i))) & 0xffu);
        }
        update(tail, sizeof(tail));
        for (std::size_t i = 0; i < 8; ++i) {
            digest_[4 * i] = static_cast<std::uint8_t>(state_[i] >> 24);
            digest_[4 * i + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            digest_[4 * i + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
            digest_[4 * i + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        finished_ = true;
    }
    return digest_;
}

Sha256Digest Sha256::of(std::string_view text) {
    Sha256 hash;
    hash.update(text);
    return hash.finish();
}

std::string toHex(const Sha256Digest& digest) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0fu]);
    }
    return out;
}

bool parseSha256Hex(std::string_view hex, Sha256Digest& out) {
    if (hex.size() != 64) return false;
    Sha256Digest parsed{};
    for (std::size_t i = 0; i < 32; ++i) {
        const int high = hexValue(hex[2 * i]);
        const int low = hexValue(hex[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        parsed[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    out = parsed;
    return true;
}

std::string sha256Hex(std::string_view text) { return toHex(Sha256::of(text)); }

bool sha256SelfTest() {
    // Эталон — константы из FIPS 180-4, а не хеш, посчитанный этой же
    // реализацией: иначе проверка ничего не проверяла бы.
    return sha256Hex("") == kSha256OfEmpty && sha256Hex("abc") == kSha256OfAbc;
}

// -------------------------------------------------------------------- манифест

int supportedManifestSchemaVersion() { return 1; }

const ManifestFile* Manifest::byPath(std::string_view path) const {
    for (const auto& file : files) {
        if (file.path == path) return &file;
    }
    return nullptr;
}

Manifest parseRuleSetManifest(std::string_view text, std::string_view origin) {
    const std::string source(origin);
    if (text.size() > kMaxManifestBytes) {
        throw RuleSyncError(source + ": манифест больше " + std::to_string(kMaxManifestBytes) + " байт");
    }

    json::Value document;
    try {
        document = json::parse(text);
    } catch (const json::ParseError& e) {
        throw RuleSyncError(source + ": невалидный JSON — " + e.what());
    }
    if (!document.isObject()) throw RuleSyncError(source + ": корень манифеста должен быть объектом");
    rejectUnknownFields(document, manifestFields(), source);

    Manifest manifest;
    // Схема: ключ называется «schema» (SPEC §9.2) или «schemaVersion» (как в
    // файлах правил). Задать оба с разными значениями нельзя — это неоднозначность.
    const json::Value* schema = document.find("schema");
    const json::Value* schemaVersion = document.find("schemaVersion");
    for (const json::Value* node : {schema, schemaVersion}) {
        if (node != nullptr && !node->isNumber()) throw RuleSyncError(source + ": версия схемы должна быть числом");
    }
    if (schema != nullptr && schemaVersion != nullptr && schema->asNumber() != schemaVersion->asNumber()) {
        throw RuleSyncError(source + ": поля \"schema\" и \"schemaVersion\" расходятся");
    }
    const json::Value* schemaNode = schema != nullptr ? schema : schemaVersion;
    int declaredSchema = supportedManifestSchemaVersion();  // поле необязательное: отсутствие = текущая схема
    if (schemaNode != nullptr) {
        const double raw = schemaNode->asNumber();
        if (raw < 0.0 || raw > 2147483647.0 || static_cast<double>(static_cast<int>(raw)) != raw) {
            throw RuleSyncError(source + ": версия схемы должна быть целым числом");
        }
        declaredSchema = static_cast<int>(raw);
    }
    if (declaredSchema != supportedManifestSchemaVersion()) {
        throw RuleSyncError(source + ": схема манифеста " + std::to_string(declaredSchema) +
                            ", поддерживается только " + std::to_string(supportedManifestSchemaVersion()));
    }
    manifest.schemaVersion = declaredSchema;

    manifest.version = requireString(document, "version", source);
    if (manifest.version.empty() || manifest.version.size() > 64u) {
        throw RuleSyncError(source + ": версия набора обязательна и не длиннее 64 символов");
    }
    for (const char c : manifest.version) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20u || byte == 0x7fu) throw RuleSyncError(source + ": в версии набора есть управляющие символы");
    }

    if (const json::Value* minApp = document.find("minAppVersion")) {
        if (!minApp->isString()) throw RuleSyncError(source + ": minAppVersion должен быть строкой");
        manifest.minAppVersion = minApp->asString();
        if (!manifest.minAppVersion.empty()) {
            // Плохую версию пропустить нельзя: неизвестная минимальная версия —
            // это неизвестные требования к набору.
            (void)versionComponents(manifest.minAppVersion);
        }
    }

    const json::Value& files = requireNode(document, "files", source);
    if (!files.isArray()) throw RuleSyncError(source + ": files должен быть массивом");
    if (files.items().empty()) throw RuleSyncError(source + ": files пуст — набор без правил не применяется");
    if (files.items().size() > kMaxManifestFiles) {
        throw RuleSyncError(source + ": файлов больше " + std::to_string(kMaxManifestFiles));
    }

    for (const auto& item : files.items()) {
        if (!item.isObject()) throw RuleSyncError(source + ": элемент files должен быть объектом");
        rejectUnknownFields(item, manifestFileFields(), source);
        ManifestFile file;
        file.path = normalizeRulePath(requireString(item, "path", source));
        if (!isSafeRelativePath(file.path)) {
            throw RuleSyncError(source + ": недопустимый путь \"" + file.path +
                                "\" (нужен относительный путь внутри набора без \"..\")");
        }
        if (manifest.byPath(file.path) != nullptr) {
            throw RuleSyncError(source + ": файл \"" + file.path + "\" перечислен дважды");
        }
        const std::string& hash = requireString(item, "sha256", source);
        Sha256Digest digest{};
        if (!parseSha256Hex(hash, digest)) {
            throw RuleSyncError(source + ": в \"" + file.path + "\" ожидается SHA-256 — 64 hex-символа, получено \"" +
                                hash + "\"");
        }
        file.sha256 = toHex(digest);
        // requireCount ждёт родительский объект и ключ в нём, поэтому передаём
        // элемент files, а не сам узел "size": иначе любой файл с объявленным
        // размером падал с «нет обязательного поля size».
        if (item.find("size") != nullptr) {
            file.size = requireCount(item, "size", source);
            file.sizeDeclared = true;
        }
        manifest.files.push_back(file);
    }

    return manifest;
}

// -------------------------------------------------------------------- версии

int compareVersions(std::string_view left, std::string_view right) {
    const std::vector<long long> a = versionComponents(left);
    const std::vector<long long> b = versionComponents(right);
    const std::size_t count = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        // «1.2» == «1.2.0»: недостающие компоненты считаются нулями.
        const long long leftPart = i < a.size() ? a[i] : 0;
        const long long rightPart = i < b.size() ? b[i] : 0;
        if (leftPart < rightPart) return -1;
        if (leftPart > rightPart) return 1;
    }
    return 0;
}

bool isAppVersionSupported(std::string_view minAppVersion, std::string_view appVersion) {
    if (minAppVersion.empty()) return true;  // ограничений нет
    if (appVersion.empty()) {
        throw RuleSyncError("версия приложения неизвестна — нечем подтвердить совместимость с набором");
    }
    return compareVersions(appVersion, minAppVersion) >= 0;
}

// ------------------------------------------------------------------ проверки

std::string FileVerdict::describe() const {
    switch (check) {
        case FileCheck::Ok:
            return path + ": совпадает с манифестом";
        case FileCheck::Missing:
            return path + ": файл отсутствует";
        case FileCheck::SizeMismatch:
            return path + ": размер " + std::to_string(actualSize) + " вместо " + std::to_string(expectedSize);
        case FileCheck::HashMismatch:
            return path + ": SHA-256 " + actualSha256 + " вместо " + expectedSha256;
        case FileCheck::Unlisted:
            return path + ": файл не перечислен в манифесте";
        case FileCheck::Duplicate:
            return path + ": файл передан дважды";
    }
    return path + ": " + fileCheckName(check);
}

bool RuleSetVerification::ok() const {
    return manifestParsed && schemaOk && versionOk && signatureOk && problems.empty();
}

std::vector<std::string> RuleSetVerification::errors() const { return problems; }

std::string RuleSetVerification::summary() const {
    if (!manifestParsed) return "манифест не разобран";
    const std::string head = manifest.version.empty() ? std::string("набор без версии") : "набор " + manifest.version;
    if (ok()) {
        return signatureChecked ? head + ": проверен, подпись верна" : head + ": проверен";
    }
    if (!problems.empty()) return head + ": отказ — " + problems.front();
    return head + ": отказ";
}

VerifiedFile makeVerifiedFile(std::string_view path, std::string_view content) {
    VerifiedFile file;
    file.path = normalizeRulePath(path);
    file.size = content.size();
    file.digest = Sha256::of(content);
    return file;
}

RuleSetVerification verifyRuleSet(std::string_view manifestBytes, std::string_view signatureBytes,
                                  const std::vector<VerifiedFile>& files, std::string_view appVersion,
                                  const VerificationPolicy& policy) {
    RuleSetVerification report;

    // Шаг 1 (SPEC §9.2 п.2): схема манифеста. Не разобрался — дальше идти
    // незачем: неизвестно, что вообще проверять.
    try {
        report.manifest = parseRuleSetManifest(manifestBytes);
        report.manifestParsed = true;
    } catch (const RuleSyncError& e) {
        report.problems.push_back(e.what());
        return report;
    }
    // parseManifest уже отверг неподдерживаемую схему; флаг нужен вызывающему,
    // который читает отчёт и не обязан помнить этот факт.
    report.schemaOk = report.manifest.schemaVersion == supportedManifestSchemaVersion();
    if (!report.schemaOk) {
        report.problems.push_back("схема манифеста " + std::to_string(report.manifest.schemaVersion) +
                                  " не поддерживается");
    }

    // Шаг 2: minAppVersion ≤ версии приложения.
    try {
        report.versionOk = isAppVersionSupported(report.manifest.minAppVersion, appVersion);
    } catch (const RuleSyncError& e) {
        report.versionOk = false;
        report.versionDetail = e.what();
    }
    if (!report.versionOk) {
        if (report.versionDetail.empty()) {
            report.versionDetail = "набор требует приложение не ниже " + report.manifest.minAppVersion +
                                   ", текущая версия " + std::string(appVersion);
        }
        report.problems.push_back(report.versionDetail);
    }

    // Шаг 3: подпись — раньше хешей. Подпись доказывает происхождение манифеста,
    // хеши доказывают целостность уже подписанного; без подписи хеши можно
    // переписать вместе с самим манифестом (ADR-008).
    if (policy.requireSignature && !policy.verifySignature) {
        // Отказ по умолчанию, а не «предупреждение»: забытый верификатор не
        // должен молча превращать проверку в отсутствие проверки.
        report.signatureChecked = false;
        report.signatureOk = false;
        report.signatureDetail = "подпись обязательна, но верификатор не задан";
    } else if (signatureBytes.empty()) {
        report.signatureChecked = false;
        report.signatureOk = !policy.requireSignature;
        report.signatureDetail = report.signatureOk ? "подпись не требуется" : "подпись отсутствует";
    } else if (!policy.verifySignature) {
        report.signatureChecked = false;
        report.signatureOk = true;
        report.signatureDetail = "проверка подписи отключена политикой";
    } else {
        report.signatureChecked = true;
        report.signatureOk = policy.verifySignature(manifestBytes, signatureBytes);
        report.signatureDetail = report.signatureOk ? "подпись верна" : "подпись не прошла проверку";
    }
    // Подробности формулируются самодостаточно: они попадают в problems и в
    // лог без дополнительной обвязки.
    if (!report.signatureOk) report.problems.push_back(report.signatureDetail);

    // Шаг 4: размер и SHA-256 каждого файла манифеста.
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string path = normalizeRulePath(files[i].path);
        if (index.find(path) != index.end()) {
            FileVerdict verdict;
            verdict.check = FileCheck::Duplicate;
            verdict.path = path;
            report.files.push_back(verdict);
            report.problems.push_back(verdict.describe());
            continue;
        }
        index[path] = i;
    }

    for (const auto& expected : report.manifest.files) {
        FileVerdict verdict;
        verdict.path = expected.path;
        verdict.expectedSha256 = expected.sha256;
        verdict.expectedSize = expected.size;
        verdict.sizeChecked = expected.sizeDeclared;
        const auto it = index.find(expected.path);
        if (it == index.end()) {
            verdict.check = FileCheck::Missing;
            report.files.push_back(verdict);
            report.problems.push_back(verdict.describe());
            continue;
        }
        const VerifiedFile& actual = files[it->second];
        verdict.actualSize = actual.size;
        if (expected.sizeDeclared && actual.size != expected.size) {
            verdict.check = FileCheck::SizeMismatch;
            verdict.actualSha256 = toHex(actual.digest);
            report.files.push_back(verdict);
            report.problems.push_back(verdict.describe());
            continue;
        }
        Sha256Digest expectedDigest{};
        (void)parseSha256Hex(expected.sha256, expectedDigest);  // формат уже проверен при разборе манифеста
        if (actual.digest == expectedDigest) {
            verdict.check = FileCheck::Ok;
            verdict.actualSha256 = expected.sha256;
        } else {
            verdict.check = FileCheck::HashMismatch;
            verdict.actualSha256 = toHex(actual.digest);
        }
        report.files.push_back(verdict);
        if (verdict.check != FileCheck::Ok) report.problems.push_back(verdict.describe());
    }

    // Файл вне манифеста — правило, которое никто не подписывал. При строгой
    // политике это отказ, при мягкой — замечание для журнала.
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string path = normalizeRulePath(files[i].path);
        if (report.manifest.byPath(path) != nullptr) continue;
        FileVerdict verdict;
        verdict.check = FileCheck::Unlisted;
        verdict.path = path;
        report.files.push_back(verdict);
        if (policy.requireKnownFilesOnly) report.problems.push_back(verdict.describe());
    }

    return report;
}

void verifyRuleSetOrThrow(std::string_view manifestBytes, std::string_view signatureBytes,
                          const std::vector<VerifiedFile>& files, std::string_view appVersion,
                          const VerificationPolicy& policy) {
    const RuleSetVerification report = verifyRuleSet(manifestBytes, signatureBytes, files, appVersion, policy);
    if (report.ok()) return;
    std::string message = "набор правил отвергнут";
    if (report.manifestParsed && !report.manifest.version.empty()) {
        message += " (" + report.manifest.version + ")";
    }
    message += ": ";
    for (std::size_t i = 0; i < report.problems.size(); ++i) {
        if (i != 0) message += "; ";
        message += report.problems[i];
    }
    throw RuleSyncError(message);
}

RuleSet loadVerifiedRuleSet(const RuleSetVerification& verification,
                            const std::vector<std::pair<std::string, std::string>>& contents,
                            std::string_view env, bool* anyUnresolved) {
    if (!verification.ok()) {
        throw RuleSyncError("набор не прошёл проверку целостности: " + verification.summary());
    }

    std::map<std::string, std::string> provided;
    for (const auto& entry : contents) provided[normalizeRulePath(entry.first)] = entry.second;
    for (const auto& entry : contents) {
        if (verification.manifest.byPath(normalizeRulePath(entry.first)) == nullptr) {
            throw RuleSyncError("файл \"" + entry.first + "\" не перечислен в манифесте — набор не разбирается");
        }
    }

    // Порядок файлов — как в манифесте: разбор детерминирован, это нужно
    // golden-тестам (SPEC §11.4).
    std::vector<std::pair<std::string, std::string>> ordered;
    ordered.reserve(verification.manifest.files.size());
    for (const auto& file : verification.manifest.files) {
        const auto it = provided.find(file.path);
        if (it == provided.end()) {
            throw RuleSyncError("файл манифеста \"" + file.path + "\" не передан на разбор");
        }
        ordered.emplace_back(file.path, it->second);
    }

    // Шаг 3 (SPEC §9.2): парсер правил. Здесь битый JSON и неизвестное поле
    // становятся ошибкой — после проверки целостности это уже не «сеть», а
    // содержимое набора.
    bool unresolved = false;
    RuleSet set = loadRuleFiles(ordered, std::string(env), &unresolved);

    if (set.schemaVersion != verification.manifest.schemaVersion) {
        throw RuleSyncError("файлы правил объявляют schemaVersion=" + std::to_string(set.schemaVersion) +
                            ", манифест — " + std::to_string(verification.manifest.schemaVersion));
    }
    if (!set.version.empty() && set.version != verification.manifest.version) {
        throw RuleSyncError("файлы правил объявляют версию " + set.version + ", манифест — " +
                            verification.manifest.version);
    }

    if (anyUnresolved != nullptr) *anyUnresolved = unresolved;
    return set;
}

// ------------------------------------------------------------ откат и офлайн

bool RuleSetStatus::shouldCheck(std::int64_t nowEpochSeconds) const {
    if (!autoUpdateEnabled) return false;
    if (lastCheckEpochSeconds <= 0) return true;
    // Часы пошли назад — проверку разрешаем, иначе автообновление встало бы
    // навсегда.
    if (nowEpochSeconds < lastCheckEpochSeconds) return true;
    return nowEpochSeconds - lastCheckEpochSeconds >= kUpdateCheckIntervalSeconds;
}

UpdateDecision decideUpdate(const RuleSetVerification& verification, const RuleSetStatus& current) {
    if (verification.ok()) {
        return {UpdateAction::ApplyCandidate, current.verified ? "набор проверен, обновляемся"
                                                             : "текущий набор не подтверждён, заменяем проверенным"};
    }
    // Кандидат проверяется, пока не применён, поэтому отказ — это «оставить как
    // есть», а не откат: откатываться не от чего (SPEC §9.2 п.3-4).
    if (current.verified) {
        return {UpdateAction::KeepCurrent,
                "кандидат отвергнут, работаем на текущем наборе: " + verification.summary()};
    }
    if (!current.verifiedVersion.empty()) {
        return {UpdateAction::RollbackToLastGood,
                "текущий набор не подтверждён и кандидат не прошёл проверку — откат на " + current.verifiedVersion};
    }
    return {UpdateAction::UseEmbeddedSet,
            "ни текущий набор, ни кандидат не подтверждены — работаем на встроенном"};
}

RuleSetStatus withAppliedCandidate(const RuleSetStatus& current, const RuleSetVerification& verification,
                                   std::int64_t nowEpochSeconds) {
    if (!verification.ok()) {
        throw RuleSyncError("непроверенный набор не применяется: " + verification.summary());
    }
    RuleSetStatus next = current;
    next.version = verification.manifest.version;
    next.verifiedVersion = verification.manifest.version;
    next.installedEpochSeconds = nowEpochSeconds;
    next.lastCheckEpochSeconds = nowEpochSeconds;
    next.lastCheckOk = true;
    next.lastResult = verification.summary();
    next.verified = true;
    next.embedded = false;  // проверенный набор пришёл извне, встроенным он не стал
    return next;
}

RuleSetStatus withFailedCheck(const RuleSetStatus& current, const RuleSetVerification& verification,
                              std::int64_t nowEpochSeconds) {
    RuleSetStatus next = current;
    next.lastCheckEpochSeconds = nowEpochSeconds;
    next.lastCheckOk = false;
    next.lastResult = verification.summary();
    if (!next.verified) {
        // Не подтверждён ни один набор: остаётся встроенный — он цел по
        // построению, иначе приложение осталось бы без правил (SPEC §9.2 п.4).
        next.embedded = true;
        next.version.clear();
        next.verified = true;
    }
    return next;
}

RuleSetStatus resetToEmbeddedSet(const RuleSetStatus& current, std::int64_t nowEpochSeconds) {
    RuleSetStatus next = current;
    next.embedded = true;
    next.version.clear();
    next.verified = true;
    next.autoUpdateEnabled = false;  // до следующего явного включения (SPEC §9.2)
    next.lastCheckEpochSeconds = nowEpochSeconds;
    next.lastResult = "возвращён встроенный набор, автообновление отключено";
    return next;
}

StartupChoice chooseStartupRuleSet(const RuleSetStatus& current) {
    if (current.verified) {
        return {current.embedded, current.version,
                current.embedded ? "работаем на встроенном наборе" : "набор " + current.version + " подтверждён"};
    }
    return {true, {}, "текущий набор не прошёл проверку — встроенный набор (SPEC §9.2 п.4)"};
}

std::string serializeRuleSetStatus(const RuleSetStatus& status) {
    const json::Value document = json::Value::object({
        {"version", json::Value(status.version)},
        {"verifiedVersion", json::Value(status.verifiedVersion)},
        {"installed", json::Value(static_cast<double>(status.installedEpochSeconds))},
        {"lastCheck", json::Value(static_cast<double>(status.lastCheckEpochSeconds))},
        {"lastResult", json::Value(status.lastResult)},
        {"lastCheckOk", json::Value(status.lastCheckOk)},
        {"autoUpdate", json::Value(status.autoUpdateEnabled)},
        {"verified", json::Value(status.verified)},
        {"embedded", json::Value(status.embedded)},
    });
    return document.dump(2);
}

RuleSetStatus parseRuleSetStatus(std::string_view text) {
    // Битое состояние — не ошибка: приложение обязано стартовать на встроенном
    // наборе, а не падать на экране настроек. Отсутствующие поля — тоже
    // встроенный набор и включённое автообновление.
    RuleSetStatus status;
    json::Value document;
    try {
        document = json::parse(text);
    } catch (const json::ParseError&) {
        return status;
    }
    if (!document.isObject()) return status;

    const auto readString = [&document](std::string_view key, std::string& out) {
        const json::Value* value = document.find(key);
        if (value != nullptr && value->isString()) out = value->asString();
    };
    const auto readNumber = [&document](std::string_view key, std::int64_t& out) {
        const json::Value* value = document.find(key);
        if (value != nullptr && value->isNumber()) out = static_cast<std::int64_t>(value->asNumber());
    };
    const auto readFlag = [&document](std::string_view key, bool& out) {
        const json::Value* value = document.find(key);
        if (value != nullptr && value->isBool()) out = value->asBool();
    };

    readString("version", status.version);
    readString("verifiedVersion", status.verifiedVersion);
    readString("lastResult", status.lastResult);
    readNumber("installed", status.installedEpochSeconds);
    readNumber("lastCheck", status.lastCheckEpochSeconds);
    readFlag("lastCheckOk", status.lastCheckOk);
    readFlag("autoUpdate", status.autoUpdateEnabled);
    readFlag("verified", status.verified);
    readFlag("embedded", status.embedded);
    return status;
}

}  // namespace mrproper::core
