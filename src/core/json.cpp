#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mrproper::json {
namespace {

constexpr int kMaxDepth = 64;

void appendEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void appendNumber(std::string& out, double n) {
    if (!std::isfinite(n)) {
        out += "null";
        return;
    }
    if (n == static_cast<double>(static_cast<long long>(n))) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", n);
    out += buf;
}

class Parser {
public:
    explicit Parser(std::string_view text) : t_(text) {}

    Value parseDocument() {
        skipWs();
        Value v = parseValue(0);
        skipWs();
        if (i_ != t_.size()) {
            fail("лишние символы после значения");
        }
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        throw ParseError(std::string(what) + " (смещение " + std::to_string(i_) + ")");
    }

    void skipWs() {
        while (i_ < t_.size()) {
            const char c = t_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        if (i_ < t_.size() && t_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view lit) {
        if (t_.substr(i_, lit.size()) == lit) {
            i_ += lit.size();
            return true;
        }
        return false;
    }

    Value parseValue(int depth) {
        if (depth > kMaxDepth) {
            fail("слишком глубокая вложенность");
        }
        if (i_ >= t_.size()) {
            fail("неожиданный конец входа");
        }
        switch (t_[i_]) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': return Value(parseString());
            case 't':
                if (literal("true")) return Value(true);
                fail("ожидалось true");
            case 'f':
                if (literal("false")) return Value(false);
                fail("ожидалось false");
            case 'n':
                if (literal("null")) return Value();
                fail("ожидалось null");
            default: return parseNumber();
        }
    }

    Value parseObject(int depth) {
        ++i_;  // '{'
        std::vector<std::pair<std::string, Value>> members;
        skipWs();
        if (consume('}')) return Value::object(std::move(members));
        for (;;) {
            skipWs();
            if (i_ >= t_.size() || t_[i_] != '"') {
                fail("ожидался ключ в кавычках");
            }
            std::string key = parseString();
            skipWs();
            if (!consume(':')) fail("ожидалось ':' после ключа");
            skipWs();
            members.emplace_back(std::move(key), parseValue(depth + 1));
            skipWs();
            if (consume(',')) continue;
            if (consume('}')) return Value::object(std::move(members));
            fail("ожидалась ',' или '}'");
        }
    }

    Value parseArray(int depth) {
        ++i_;  // '['
        std::vector<Value> items;
        skipWs();
        if (consume(']')) return Value::array(std::move(items));
        for (;;) {
            skipWs();
            items.push_back(parseValue(depth + 1));
            skipWs();
            if (consume(',')) continue;
            if (consume(']')) return Value::array(std::move(items));
            fail("ожидалась ',' или ']'");
        }
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    unsigned parseHex4() {
        if (i_ + 4 > t_.size()) fail("обрыв в \\u-escape");
        unsigned value = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail("не-hex символ в \\u-escape");
            }
        }
        return value;
    }

    std::string parseString() {
        ++i_;  // '"'
        std::string out;
        for (;;) {
            if (i_ >= t_.size()) fail("строка не закрыта");
            const char c = t_[i_++];
            if (c == '"') return out;
            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20) fail("управляющий символ в строке");
                out.push_back(c);
                continue;
            }
            if (i_ >= t_.size()) fail("обрыв после \\");
            const char e = t_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < t_.size() && t_[i_] == '\\' &&
                        t_[i_ + 1] == 'u') {
                        i_ += 2;
                        const unsigned low = parseHex4();
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            appendUtf8(out, cp);
                            cp = low;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("неизвестный escape");
            }
        }
    }

    Value parseNumber() {
        const size_t start = i_;
        if (i_ < t_.size() && (t_[i_] == '-' || t_[i_] == '+')) ++i_;
        bool any = false;
        while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') {
            ++i_;
            any = true;
        }
        if (i_ < t_.size() && t_[i_] == '.') {
            ++i_;
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') {
                ++i_;
                any = true;
            }
        }
        if (!any) fail("ожидалось число");
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '-' || t_[i_] == '+')) ++i_;
            bool expDigits = false;
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') {
                ++i_;
                expDigits = true;
            }
            if (!expDigits) fail("битая экспонента");
        }
        const std::string buffer(t_.substr(start, i_ - start));
        return Value(std::strtod(buffer.c_str(), nullptr));
    }

    std::string_view t_;
    size_t i_{0};
};

}  // namespace

Value Value::array(std::vector<Value> items) {
    Value v;
    v.type_ = Type::Array;
    v.arr_ = std::move(items);
    return v;
}

Value Value::object(std::vector<std::pair<std::string, Value>> members) {
    Value v;
    v.type_ = Type::Object;
    v.obj_ = std::move(members);
    return v;
}

bool Value::asBool() const {
    if (type_ != Type::Bool) throw std::runtime_error("ожидалось поле типа bool");
    return bool_;
}

double Value::asNumber() const {
    if (type_ != Type::Number) throw std::runtime_error("ожидалось поле типа number");
    return num_;
}

const std::string& Value::asString() const {
    if (type_ != Type::String) throw std::runtime_error("ожидалось поле типа string");
    return str_;
}

const std::vector<Value>& Value::items() const {
    if (type_ != Type::Array) throw std::runtime_error("ожидалось поле типа array");
    return arr_;
}

const std::vector<std::pair<std::string, Value>>& Value::members() const {
    if (type_ != Type::Object) throw std::runtime_error("ожидалось поле типа object");
    return obj_;
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& [k, v] : obj_) {
        if (k == key) return &v;
    }
    return nullptr;
}

const Value& Value::require(std::string_view key) const {
    const Value* found = find(key);
    if (found == nullptr) {
        throw std::runtime_error("нет обязательного поля \"" + std::string(key) + "\"");
    }
    return *found;
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent * (depth + 1)), ' ') : std::string();
    const std::string padEnd = pretty ? std::string(static_cast<size_t>(indent * depth), ' ') : std::string();

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: appendNumber(out, num_); break;
        case Type::String: appendEscaped(out, str_); break;
        case Type::Array: {
            if (arr_.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            bool first = true;
            for (const auto& item : arr_) {
                if (!first) out += ',';
                first = false;
                if (pretty) {
                    out += '\n';
                    out += pad;
                }
                item.dumpTo(out, indent, depth + 1);
            }
            if (pretty) {
                out += '\n';
                out += padEnd;
            }
            out += ']';
            break;
        }
        case Type::Object: {
            if (obj_.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            bool first = true;
            for (const auto& [key, value] : obj_) {
                if (!first) out += ',';
                first = false;
                if (pretty) {
                    out += '\n';
                    out += pad;
                }
                appendEscaped(out, key);
                out += pretty ? ": " : ":";
                value.dumpTo(out, indent, depth + 1);
            }
            if (pretty) {
                out += '\n';
                out += padEnd;
            }
            out += '}';
            break;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

Value parse(std::string_view text) {
    return Parser(text).parseDocument();
}

}  // namespace mrproper::json
