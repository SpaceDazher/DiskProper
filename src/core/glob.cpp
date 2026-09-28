#include "glob.hpp"

#include <cctype>
#include <string>

namespace mrproper::core {
namespace {

inline bool isSep(char c) { return c == '/' || c == '\\'; }

inline char fold(char c, CaseMode mode) {
    if (mode == CaseMode::AsciiInsensitive) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return c;
}

// Разбирает [abc] / [a-z] / [!abc], начиная с '[' (i указывает на '[').
// При успехе i сдвигается за закрывающую скобку, out получает нормализованный класс.
bool parseClass(std::string_view pat, size_t& i, std::string& out) {
    const size_t start = i;
    ++i;
    bool negate = false;
    if (i < pat.size() && (pat[i] == '!' || pat[i] == '^')) {
        negate = true;
        ++i;
    }
    std::string body;
    while (i < pat.size()) {
        const char c = pat[i];
        if (c == ']' && !body.empty()) {
            ++i;
            out.clear();
            if (negate) out.push_back('!');
            out += body;
            return true;
        }
        body.push_back(c);
        ++i;
    }
    i = start;
    return false;
}

bool inClass(const std::string& cls, char c) {
    size_t i = 0;
    bool negate = false;
    if (i < cls.size() && (cls[i] == '!' || cls[i] == '^')) {
        negate = true;
        ++i;
    }
    const char f = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool found = false;
    while (i + 1 < cls.size()) {
        if (cls[i + 1] == '-') {
            const char lo = static_cast<char>(std::tolower(static_cast<unsigned char>(cls[i])));
            const char hi = static_cast<char>(std::tolower(static_cast<unsigned char>(cls[i + 2])));
            if (f >= lo && f <= hi) found = true;
            i += 3;
            continue;
        }
        if (std::tolower(static_cast<unsigned char>(cls[i])) == std::tolower(static_cast<unsigned char>(f))) {
            found = true;
        }
        ++i;
    }
    if (i < cls.size() &&
        std::tolower(static_cast<unsigned char>(cls[i])) == std::tolower(static_cast<unsigned char>(f))) {
        found = true;
    }
    return negate ? !found : found;
}

bool matchHere(std::string_view pat, std::string_view str, CaseMode mode) {
    if (pat.empty()) return str.empty();
    const char pc = pat[0];

    if (pc == '*') {
        size_t np = 1;
        bool dstar = false;
        while (np < pat.size() && pat[np] == '*') {
            dstar = true;
            ++np;
        }
        // «**/» способно соответствовать нулю сегментов: «a/**/b» == «a/b».
        if (dstar && np < pat.size() && isSep(pat[np])) {
            if (matchHere(pat.substr(np + 1), str, mode)) return true;
        }
        const std::string_view rest = pat.substr(np);
        // Только «**» в конце шаблона поглощает весь остаток пути; одиночный «*» — нет,
        // иначе он пересёк бы границу сегмента.
        if (dstar && rest.empty()) return true;
        if (dstar) {
            // Кандидаты: начало строки, позиции разделителей и позиции сразу после них —
            // этого достаточно, чтобы остаток начинался либо с разделителя, либо с литерала.
            if (matchHere(rest, str, mode)) return true;
            for (size_t k = 0; k < str.size(); ++k) {
                if (!isSep(str[k])) continue;
                if (matchHere(rest, str.substr(k), mode)) return true;
                if (matchHere(rest, str.substr(k + 1), mode)) return true;
            }
            return false;
        }
        // «*» не пересекает границу сегмента.
        for (size_t k = 0;; ++k) {
            if (matchHere(rest, str.substr(k), mode)) return true;
            if (k == str.size() || isSep(str[k])) return false;
        }
    }

    if (str.empty()) return false;

    if (pc == '?') {
        if (isSep(str[0])) return false;
        return matchHere(pat.substr(1), str.substr(1), mode);
    }

    if (pc == '[') {
        std::string cls;
        size_t q = 0;
        if (!parseClass(pat, q, cls)) {
            // Незакрытая скобка — трактуем как обычный символ.
            return str[0] == '[' && matchHere(pat.substr(1), str.substr(1), mode);
        }
        if (isSep(str[0]) || !inClass(cls, str[0])) return false;
        return matchHere(pat.substr(q), str.substr(1), mode);
    }

    if (fold(str[0], mode) != fold(pc, mode)) return false;
    return matchHere(pat.substr(1), str.substr(1), mode);
}

}  // namespace

std::string normalizeSeparators(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (const char c : path) {
        out.push_back(isSep(c) ? '/' : c);
    }
    return out;
}

bool matchPath(std::string_view pattern, std::string_view path, CaseMode mode) {
    // Разделители приводим к одному виду здесь, чтобы правило и путь можно было
    // писать в любой системе сборки (WSL/Linux для тестов, Windows для прогона).
    return matchHere(normalizeSeparators(pattern), normalizeSeparators(path), mode);
}

bool isValidPattern(std::string_view pattern) {
    if (pattern.empty()) return false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '[') {
            std::string cls;
            size_t q = i;
            if (!parseClass(pattern, q, cls)) return false;
            i = q - 1;
        }
    }
    return true;
}

std::string expandEnvironment(std::string_view pattern, const std::string& env, bool* ok) {
    std::string out;
    bool resolved = true;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            out.push_back(pattern[i]);
            continue;
        }
        const size_t close = pattern.find('%', i + 1);
        if (close == std::string_view::npos) {
            out.push_back('%');
            continue;
        }
        const std::string name(pattern.substr(i + 1, close - i - 1));
        if (name.empty()) {
            out.push_back('%');
            continue;
        }
        const size_t sep = env.find(name + "=");
        if (sep == std::string::npos) {
            resolved = false;
            out.append(pattern.substr(i, close - i + 1));
        } else {
            const size_t valueEnd = env.find('\n', sep);
            out.append(env, sep + name.size() + 1, valueEnd - sep - name.size() - 1);
        }
        i = close;
    }
    if (ok != nullptr) *ok = resolved;
    return out;
}

}  // namespace mrproper::core
