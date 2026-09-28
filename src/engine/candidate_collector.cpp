// Реализация сбора кандидатов: разворот локаторов, min-age, исключения,
// группировка по профилю. Контракт, границы и инварианты — в
// candidate_collector.hpp; здесь код и только те замечания, которые видны
// с этого уровня.
//
// Порядок чтения: разбор локатора на сегменты (путь) → обход сегментов с
// перечислением каталогов (разворот) → отбор файлов по исключениям, возрасту и
// «внутри корня» (обход дерева) → сборка CleanupCandidate (результат).
//
// Слой: файл не включает windows.h и не знает ни о platform::vfs, ни о
// scanner::*. Всё чтение диска — через FileSystemProbe (см. заголовок), поэтому
// модуль собирается и проверяется на любом хосте (SPEC §6.1, ADR-004).
//
// ---------------------------------------------------------------------------
// Три решения, которые стоит объяснить до кода
// ---------------------------------------------------------------------------
//
// 1. «Возраст не доказан» — это «не young», а не «young». Файл, у которого ФС не
//    отдала время записи, при minAgeDays > 0 в кандидат не попадает: удалять
//    файл, чей возраст неизвестен, нельзя (SPEC §10, приоритет «безопасность
//    данных» в §1.1). Такой файл считается в stats.filesUnknownAge, и набор
//    правил с массой таких файлов виден по цифре.
//
// 2. Аллоцированный размер, которого нет, заменяется ЛОГИЧЕСКИМ, а не нулём.
//    FR-4 считает освобождаемое место по аллоцированному размеру, и план
//    (engine::plan_builder) берёт reclaimBytes из него. Молчаливый ноль
//    занизил бы обещание пользователю («освободим 4 ГБ» → освободилось 3,1 ГБ),
//    а логический размер — честная верхняя оценка для несжатого файла. Факт
//    подмены считается в stats.allocatedEstimated и попадает в reasons: цифра,
//    которой нельзя доверять, должна быть видна.
//
// 3. Порядок результата не сортируется. Сортировка по размеру — дело UI и плана
//    (SPEC §6.2), а scan должен быть идемпотентным (SPEC §8 Этап 2, проверка
//    e2e): один и тот же набор правил на одной и той же ФС обязан давать одну и
//    ту же последовательность. Порядок правил — как в наборе, внутри правила —
//    как перечислил каталог.
#include "candidate_collector.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/glob.hpp"
#include "core/log.hpp"
#include "core/model.hpp"
#include "core/rules.hpp"

namespace mrproper::engine {
namespace {

constexpr char kSeparator = '\\';

bool isSeparator(char c) noexcept { return c == '/' || c == '\\'; }

char asciiLower(char c) noexcept {
    // Только ASCII — по той же причине, что и в scoring_bridge: сворачивание
    // регистра в полном Unicode требует таблицы Unicode, а авторитетное
    // сравнение путей живёт в platform::vfs_paths (CompareStringOrdinal).
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string toLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(asciiLower(c));
    }
    return out;
}

bool equalsIgnoreAsciiCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (asciiLower(left[i]) != asciiLower(right[i])) return false;
    }
    return true;
}

// Путь в той форме, в какой его хранит модель (§6.3): обратный слэш, без
// повторов, без хвостового разделителя (кроме корня тома — «C:\» без него не
// путь). Разделители приводятся здесь, а не в glob: core::matchPath
// приводит их сам, но собирать путь приходится здесь же, и сравнивать его
// потом тоже здесь же.
std::string normalizePath(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    std::size_t i = 0;
    // UNC-префикс «\\» переживает схлопывание: без него «\\server\share» стал бы
    // «\server\share», а это другой путь (корень текущего тома), и каталога с
    // таким именем не существует.
    if (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1])) {
        out.push_back(kSeparator);
        out.push_back(kSeparator);
        i = 2;
    }
    for (; i < path.size(); ++i) {
        const char c = path[i];
        if (isSeparator(c)) {
            if (!out.empty() && out.back() == kSeparator) continue;  // «C:\\Temp\\» → «C:\Temp»
            out.push_back(kSeparator);
        } else {
            out.push_back(c);
        }
    }
    const bool driveRoot = out.size() == 3 && out[1] == ':' && out.back() == kSeparator;
    while (out.size() > 1 && out.back() == kSeparator && !driveRoot) {
        out.pop_back();
    }
    return out;
}

std::string joinPath(std::string_view base, std::string_view name) {
    std::string out(base);
    if (!out.empty() && out.back() != kSeparator) {
        out.push_back(kSeparator);
    }
    out.append(name);
    return out;
}

// Разбор пути на сегменты с сохранением корня: «C:\a\b» → «C:\», «a», «b».
// Корневой сегмент сохраняет хвостовой разделитель, иначе joinPath склеил бы
// «C:» с «Windows» в «C:Windows» — другой, несуществующий путь.
std::vector<std::string> splitSegments(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t pos = 0;

    if (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1])) {
        // UNC: «\\server\share\…». Сервер и шаре — один сегмент: оба обязательны.
        const std::size_t serverEnd = path.find_first_of("/\\", 2);
        if (serverEnd == std::string_view::npos) {
            parts.emplace_back(path);
            return parts;
        }
        const std::size_t shareEnd = path.find_first_of("/\\", serverEnd + 1);
        if (shareEnd == std::string_view::npos) {
            parts.emplace_back(path);
            return parts;
        }
        parts.emplace_back(path.substr(0, shareEnd + 1));
        pos = shareEnd + 1;
    } else if (path.size() >= 3 && path[1] == ':' && isSeparator(path[2])) {
        parts.emplace_back(path.substr(0, 3));  // «C:\»
        pos = 3;
    } else if (!path.empty() && isSeparator(path[0])) {
        parts.emplace_back(1, kSeparator);  // путь от корня текущего тома
        pos = 1;
    }

    while (pos < path.size()) {
        const std::size_t next = path.find_first_of("/\\", pos);
        if (next == std::string_view::npos) {
            parts.emplace_back(path.substr(pos));
            break;
        }
        parts.emplace_back(path.substr(pos, next - pos));
        pos = next + 1;
    }
    return parts;
}

// Есть ли в сегменте подстановка: «*», «?», «[».
bool hasWildcard(std::string_view segment) noexcept {
    for (const char c : segment) {
        if (c == '*' || c == '?' || c == '[') return true;
    }
    return false;
}

bool isDoubleStar(std::string_view segment) noexcept { return segment == "**"; }

// Осталась ли неразрешённая %ПЕРЕМЕННАЯ%. core::expandEnvironment оставляет
// неизвестные переменные в шаблоне как есть (docs/rules-authoring.md §4.1), и
// правило с такой переменной просто неактивно: искать каталог «%APPDATA%\...» —
// значит выдумать путь, а это прямо запрещено (§5.4).
//
// Имя переменной — буквы, цифры, «_», скобки (ProgramFiles(x86)). Если между
// процентами попалось что-то с разделителем или точкой, это не переменная, а
// часть имени файла: «100%discount%\x» должно остаться таким, каким написано.
bool hasUnresolvedVariable(std::string_view path) {
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '%') continue;
        const std::size_t close = path.find('%', i + 1);
        if (close == std::string_view::npos) break;
        const std::string_view name = path.substr(i + 1, close - i - 1);
        if (!name.empty()) {
            bool plausible = true;
            for (const char c : name) {
                const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                     c == '_' || c == '(' || c == ')';
                if (!allowed) {
                    plausible = false;
                    break;
                }
            }
            if (plausible) return true;
        }
        i = close;
    }
    return false;
}

// Начинается ли путь с корня: буква диска, UNC или разделитель. Относительный
// локатор — дефект набора правил, а не повод искать каталог относительно
// текущего каталога процесса (он разный у приложения, CLI и ярлыка).
bool isAbsolutePattern(const std::string& firstSegment) noexcept {
    if (firstSegment.size() >= 3 && firstSegment[1] == ':' && isSeparator(firstSegment[2])) return true;
    return !firstSegment.empty() && isSeparator(firstSegment.front());
}

// Ли path внутри root. Текстовое сравнение с проверкой границы по
// разделителю: «C:\Users\Ev» не влезает в «C:\Users\Evil».
//
// Направление ошибки здесь — единственное, которое допустимо: элемент вне
// корня не учитывается и попадает в stats.outsideRoot. Сравнение то же по
// смыслу, что у scoring_bridge::isPathUnderRoot (эвристика уверенности) и у
// platform::vfs_paths::checkRuleRoot (авторитетная проверка перед операцией);
// здесь оно — жёсткий фильтр в горячем цикле, поэтому живёт рядом с обходом,
// а не тянет зависимость от соседнего модуля движка.
bool isInsideRoot(const std::string& root, std::string_view path) noexcept {
    if (path.size() < root.size()) return false;
    if (!equalsIgnoreAsciiCase(path.substr(0, root.size()), root)) return false;
    if (path.size() == root.size()) return true;                       // сам корень
    if (isSeparator(root.back())) return true;                         // корень кончается на «C:\»
    return isSeparator(path[root.size()]);                             // «...\Ev» против «...\Evil»
}

// Имя элемента, если он лежит непосредственно в корне. Нужно фильтру листьев:
// «iconcache*.db» относится к верхнему уровню каталога, а вложенные
// «...\sub\iconcache*.db» под него не подпадают.
bool directChildName(const std::string& root, std::string_view path, std::string& name) {
    if (path.size() <= root.size()) return false;
    if (!equalsIgnoreAsciiCase(path.substr(0, root.size()), root)) return false;
    std::string_view rest = path.substr(root.size());
    if (!isSeparator(root.back())) {
        // У корня-каталога хвостового разделителя нет («C:\Users\x»), значит
        // разделитель обязателен. У корня тома он уже есть («C:\»), и «C:\a» —
        // прямой ребёнок «C:\», а не путь внутри «C:».
        if (rest.empty() || !isSeparator(rest.front())) return false;
        rest.remove_prefix(1);
    }
    if (rest.empty() || rest.find_first_of("/\\") != std::string_view::npos) return false;
    name.assign(rest);
    return true;
}

// Сумма с насыщением: суммировать размеры дерева в uint64 дешевле, чем
// проверять переполнение на каждом файле, но «объём кандидата стал нулевым
// после 16 экзабайтов» — тот вид отчёта, после которого перестают доверять
// и предыдущим цифрам.
std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

// Курсор разворота: путь + значение первого подставленного сегмента (профиль).
struct Cursor {
    std::string path;
    std::string profile;
    bool hasProfile{};
};

// Всё, что накоплено по одному корню. Отдельная структура, а не поля прямо в
// кандидате: половину счётчиков (отсечённые, исключённые, ссылки) в
// CleanupCandidate нет места — они уходят в CollectStats и в reasons.
struct RootTally {
    std::uint64_t files{};
    std::uint64_t logical{};
    std::uint64_t allocated{};
    std::int64_t oldestWrite{};
    std::int64_t newestWrite{};
    std::int64_t lastAccess{};
    std::uint64_t tooYoung{};
    std::uint64_t unknownAge{};
    std::uint64_t excludedFiles{};
    std::uint64_t prunedDirs{};
    std::uint64_t reparse{};
    std::uint64_t allocatedEstimated{};
    std::uint64_t sizeUnknown{};
    std::uint64_t outsideRoot{};
};

// Контекст обхода одного корня: правило (исключения), корень (граница), фильтр
// листьев и возрастной порог. Вынесен в структуру, потому что посетитель
// std::function получает один аргумент и не имеет права захватывать семь
// переменных из области видимости по ссылке — это работает, но стоит дороже
// каждого вызова на файле.
struct RootScan {
    const core::Rule* rule{};
    std::string root;
    std::string leafPattern;
    std::int64_t oldestAllowed{};  // время записи не новее этого значения
    bool ageFilter{};
    bool includeReparse{};
    CollectStats* stats{};
    RootTally tally;

    // Фильтр листьев действует на верхнем уровне корня. Это ровно то, что
    // обещает glob: «*» не пересекает разделитель (docs/rules-authoring.md §4),
    // поэтому «C:\Logs\*.log» — это файлы каталога, а не всё дерево под ним, а
    // «iconcache*.db» не должен притащить «sub\iconcache*.db».
    //
    // Вложенные файлы при этом не теряются: «…\Cache\**\*.log» разворачивается
    // в отдельный корень каждому подкаталогу Cache, и «*.log» каждого из них
    // отбирается его собственным корнем. Разворачивать «**» рекурсивно внутри
    // одного кандидата значило бы либо считать поддерево дважды, либо проверять
    // относительный путь по шаблону — и оба варианта дороже, чем отдельный
    // корень на каталог.
    [[nodiscard]] bool leafAllows(const ProbeEntry& entry) const {
        if (leafPattern.empty()) return true;
        if (equalsIgnoreAsciiCase(entry.path, root)) return true;  // корень-файл сам по себе
        std::string name;
        if (!directChildName(root, entry.path, name)) return false;  // не верхний уровень
        return core::matchPath(leafPattern, name);
    }

    void countFile(const ProbeEntry& entry) {
        ++stats->filesSeen;
        ++tally.files;
        if (!entry.sizeKnown) {
            ++stats->sizeUnknown;
            ++tally.sizeUnknown;
        } else {
            tally.logical = saturatingAdd(tally.logical, entry.logicalBytes);
            if (entry.allocatedKnown) {
                tally.allocated = saturatingAdd(tally.allocated, entry.allocatedBytes);
            } else {
                // См. пункт 2 в шапке файла: логический вместо нуля, и это видно.
                tally.allocated = saturatingAdd(tally.allocated, entry.logicalBytes);
                ++stats->allocatedEstimated;
                ++tally.allocatedEstimated;
            }
        }
        if (entry.writeTime != 0) {
            if (tally.oldestWrite == 0 || entry.writeTime < tally.oldestWrite) {
                tally.oldestWrite = entry.writeTime;
            }
            if (entry.writeTime > tally.newestWrite) {
                tally.newestWrite = entry.writeTime;
            }
        }
        if (entry.accessTime != 0 && entry.accessTime > tally.lastAccess) {
            tally.lastAccess = entry.accessTime;
        }
    }

    [[nodiscard]] VisitStep visit(const ProbeEntry& entry) {
        if (entry.reparsePoint && !includeReparse) {
            // FR-6: по ссылке не идём, содержимое её цели — чужие данные.
            ++stats->reparseSkipped;
            ++tally.reparse;
            return entry.isDirectory ? VisitStep::Skip : VisitStep::Continue;
        }
        if (!isInsideRoot(root, entry.path)) {
            ++stats->outsideRoot;
            ++tally.outsideRoot;
            return entry.isDirectory ? VisitStep::Skip : VisitStep::Continue;
        }
        if (rule->excluded(entry.path)) {
            // Исключение выигрывает у совпадения всегда (docs/rules-authoring.md §6.4),
            // а каталог под исключением не обходится вовсе: «...\Service Worker\
            // ScriptCache» — это сотни мегабайт, которые нельзя даже измерить.
            if (entry.isDirectory) {
                ++stats->directoriesPruned;
                ++tally.prunedDirs;
                return VisitStep::Skip;
            }
            ++stats->filesExcluded;
            ++tally.excludedFiles;
            return VisitStep::Continue;
        }
        if (entry.isDirectory) return VisitStep::Continue;
        if (!leafAllows(entry)) return VisitStep::Continue;
        if (ageFilter) {
            if (entry.writeTime == 0) {
                ++stats->filesUnknownAge;
                ++tally.unknownAge;
                return VisitStep::Continue;
            }
            if (entry.writeTime > oldestAllowed) {
                ++stats->filesTooYoung;
                ++tally.tooYoung;
                return VisitStep::Continue;
            }
        }
        countFile(entry);
        return VisitStep::Continue;
    }
};

void addNote(CollectStats& stats, std::size_t maxNotes, std::string note) {
    if (stats.notes.size() < maxNotes) {
        stats.notes.push_back(std::move(note));
    } else {
        ++stats.notesDropped;
    }
}

// Развернуть «**» в середине локатора: ноль уровней уже учтён вызывающим,
// здесь — все потомки каждого курсора. Обход в ширину с пределом глубины и
// бюджетом каталогов; рекурсия не используется намеренно (глубокое дерево
// плюс рекурсия — это переполнение стека на «C:\Users\...\AppData\...»).
//
// Предел maxVisitedDirs ограничивает ПЕРЕБОР, а не результат: результат
// ограничивается maxRootsPerRule отдельно, когда из курсоров делаются корни.
// Иначе «**» под %LOCALAPPDATA% (десятки тысяч каталогов) упёрся бы в предел
// корней там, где он не имеет отношения к делу.
void expandDoubleStar(FileSystemProbe& probe, const std::vector<Cursor>& current, std::vector<Cursor>& out,
                      std::unordered_set<std::string>& seen, const ExpandLimits& limits, std::stop_token token,
                      std::size_t& visitedDirs, bool& truncated) {
    std::vector<Cursor> frontier = current;
    for (std::size_t level = 0; level < limits.maxDepth && !frontier.empty(); ++level) {
        if (token.stop_requested()) return;
        std::vector<Cursor> nextFrontier;
        for (const Cursor& cursor : frontier) {
            if (++visitedDirs > limits.maxVisitedDirs) {
                truncated = true;
                return;
            }
            std::vector<ProbeEntry> entries;
            if (!probe.listDirectory(cursor.path, token, entries)) continue;
            for (const ProbeEntry& entry : entries) {
                if (!entry.isDirectory || entry.reparsePoint) continue;  // FR-6
                // Путь в форме модели — дальше он сравнивается с исключениями
                // правила и с корнем, собранным нашими же разделителями
                // (см. такую же правку в ветке с «*»).
                const std::string path = normalizePath(entry.path);
                if (!seen.insert(toLowerAscii(path)).second) continue;
                const Cursor child{path, cursor.profile, cursor.hasProfile};
                nextFrontier.push_back(child);
                out.push_back(child);
            }
        }
        frontier = std::move(nextFrontier);
    }
}

LocatedRoot makeRoot(const core::Rule& rule, const std::string& path, const std::string& leafPattern,
                      const std::string& profile, bool hasProfile) {
    LocatedRoot root;
    root.ruleId = rule.id;
    root.path = path;
    root.leafPattern = leafPattern;
    root.profile = profile;
    root.groupedByProfile = rule.groupByProfile && hasProfile;
    return root;
}

// Профиль — значение ПЕРВОГО подставленного сегмента: у браузеров это каталог
// профиля («Default», «Profile 1»), и именно он назван в объяснении FR-4
// («кэш браузера Edge, профиль Default»). Последующие подстановки («*\Dawn*
// Cache») — уже деталь внутри профиля, а не отдельная группа.
std::string profileFrom(const Cursor& cursor, const std::string& segmentValue) {
    return cursor.hasProfile ? cursor.profile : segmentValue;
}

// Тело разворота; публичная функция ниже только перекладывает флаг «список
// корней обрезан» в выходной параметр, чтобы не копировать вектор.
std::vector<LocatedRoot> collectRoots(const core::Rule& rule, FileSystemProbe& probe, const ExpandLimits& limits,
                                      std::stop_token token, bool& truncated) {
    std::vector<LocatedRoot> roots;

    // Предел на число корней. «**» в середине локатора способен дать их десятки
    // тысяч («%LOCALAPPDATA%\**\Cache»), а каждый корень — это отдельный обход
    // дерева. Обрезанный список неполон, и об этом говорится в лог и в счётчик:
    // половина правды в отчёте хуже явной точки обрыва.
    const auto addRoot = [&](LocatedRoot root) {
        if (roots.size() >= limits.maxRootsPerRule) {
            truncated = true;
            return false;
        }
        roots.push_back(std::move(root));
        return true;
    };

    const std::string pattern = normalizePath(rule.resolvedLocator.empty() ? rule.locator : rule.resolvedLocator);
    if (pattern.empty() || hasUnresolvedVariable(pattern)) return roots;

    const std::vector<std::string> segments = splitSegments(pattern);
    if (segments.empty() || !isAbsolutePattern(segments.front())) return roots;

    // Сегменты делятся на три части: корень (segments[0]), середина
    // (segments[1 .. n-2]) и последний сегмент, который либо продолжает корень,
    // либо становится фильтром листьев. Разводить их надо ДО обхода: если
    // последний сегмент прогнать в общей очереди как каталог, «C:\Windows\Temp»
    // превратится в «C:\Windows\Temp\Temp», а «C:\Logs\*.log» будет искать
    // каталог, названный «*.log», и не найдёт ничего.
    const std::size_t segmentCount = segments.size();
    const bool hasTailSegment = segmentCount >= 2;
    const std::string last = hasTailSegment ? segments.back() : std::string();
    const std::size_t dirSegmentCount = hasTailSegment ? segmentCount - 2 : 0;
    const bool lastIsDoubleStar = hasTailSegment && isDoubleStar(last);
    const bool lastIsLiteral = hasTailSegment && !lastIsDoubleStar && !hasWildcard(last);

    std::vector<Cursor> cursors;
    std::unordered_set<std::string> seen;
    cursors.push_back(Cursor{segments.front(), std::string(), false});
    seen.insert(toLowerAscii(segments.front()));

    std::size_t visitedDirs = 0;

    for (std::size_t i = 1; i <= dirSegmentCount && !cursors.empty(); ++i) {
        if (token.stop_requested()) return {};
        if (cursors.size() > limits.maxVisitedDirs) {
            truncated = true;
            break;
        }
        const std::string& segment = segments[i];
        std::vector<Cursor> next;
        if (isDoubleStar(segment)) {
            // Ветка «ноль уровней» — текущие курсоры идут дальше как есть.
            for (const Cursor& cursor : cursors) {
                next.push_back(cursor);
            }
            expandDoubleStar(probe, cursors, next, seen, limits, token, visitedDirs, truncated);
        } else if (hasWildcard(segment)) {
            for (const Cursor& cursor : cursors) {
                std::vector<ProbeEntry> entries;
                if (!probe.listDirectory(cursor.path, token, entries)) continue;
                for (const ProbeEntry& entry : entries) {
                    if (!entry.isDirectory || entry.reparsePoint) continue;  // FR-6
                    if (!core::matchPath(segment, entry.name)) continue;
                    // Путь приводим к форме модели: дальше он сравнивается с
                    // исключениями правила и с корнем, собранным нашими же
                    // разделителями.
                    const std::string path = normalizePath(entry.path);
                    if (!seen.insert(toLowerAscii(path)).second) continue;
                    const std::string profile = profileFrom(cursor, entry.name);
                    next.push_back(Cursor{path, profile, true});
                }
            }
        } else {
            for (const Cursor& cursor : cursors) {
                const std::string path = joinPath(cursor.path, segment);
                if (!seen.insert(toLowerAscii(path)).second) continue;
                next.push_back(Cursor{path, cursor.profile, cursor.hasProfile});
            }
        }
        cursors = std::move(next);
    }

    if (cursors.empty() || token.stop_requested()) return roots;

    // Предупреждение об обрезанном списке — одно на правило, а не на каждый
    // сработавший предел, иначе в логе окажется тысяча одинаковых строк.
    // truncated берётся по ссылке: предел может сработать уже после создания
    // этой лямбды.
    const auto logTruncation = [&rule, &limits, &roots, &truncated] {
        if (!truncated) return;
        core::LogFields fields;
        fields.push_back(core::logField("rule", rule.id));
        fields.push_back(core::logField("maxVisitedDirs", static_cast<std::uint64_t>(limits.maxVisitedDirs)));
        fields.push_back(core::logField("maxRootsPerRule", static_cast<std::uint64_t>(limits.maxRootsPerRule)));
        fields.push_back(core::logField("roots", static_cast<std::uint64_t>(roots.size())));
        core::logWarn("scan.collect.truncated", "разворот локатора упёрся в предел: список корней неполон",
                      std::move(fields));
    };

    // Локатор из одного сегмента («C:\», «\\server\share») — это сам корень:
    // приклеивать к нему нечего, а повторно добавлять сегмент нельзя.
    if (!hasTailSegment) {
        ProbeEntry rootEntry;
        if (probe.statPath(cursors.front().path, token, rootEntry)) {
            (void)addRoot(makeRoot(rule, cursors.front().path, std::string(), std::string(), false));
        }
        logTruncation();
        return roots;
    }

    // Последний сегмент: продолжает корень (литерал), становится фильтром
    // листьев (шаблон) или даёт отдельный корень на каждый подходящий каталог.
    // «**» в конце — «всё содержимое корня», отдельных корней не даёт.
    for (const Cursor& cursor : cursors) {
        if (token.stop_requested()) return {};
        if (roots.size() >= limits.maxRootsPerRule) {
            truncated = true;
            break;
        }
        if (lastIsDoubleStar) {
            (void)addRoot(makeRoot(rule, cursor.path, std::string(), cursor.profile, cursor.hasProfile));
            continue;
        }
        if (lastIsLiteral) {
            const std::string path = joinPath(cursor.path, last);
            // Каталог или файл, собранные из литералов, могут и не существовать
            // (правило опередило систему). Подтверждаем на диске — иначе обещание
            // «корень есть на диске» в заголовке было бы ложью, а UI показал бы
            // кандидат, под которым ничего нет.
            ProbeEntry probeEntry;
            if (probe.statPath(path, token, probeEntry)) {
                (void)addRoot(makeRoot(rule, path, std::string(), cursor.profile, cursor.hasProfile));
            }
            continue;
        }
        std::vector<ProbeEntry> entries;
        if (!probe.listDirectory(cursor.path, token, entries)) continue;
        bool anyFile = false;
        for (const ProbeEntry& entry : entries) {
            if (roots.size() >= limits.maxRootsPerRule) {
                truncated = true;
                break;
            }
            if (!core::matchPath(last, entry.name)) continue;
            if (entry.isDirectory) {
                (void)addRoot(
                    makeRoot(rule, normalizePath(entry.path), std::string(), profileFrom(cursor, entry.name), true));
            } else {
                anyFile = true;
            }
        }
        // Файлы верхнего уровня — листья самого каталога: «%USERPROFILE%\*.*».
        // Если под шаблон попали только каталоги, каталог остаётся корнем лишь
        // ради них, и в список не попадает.
        if (anyFile && !addRoot(makeRoot(rule, cursor.path, last, cursor.profile, cursor.hasProfile))) {
            break;  // список корней исчерпан
        }
    }

    logTruncation();

    return roots;
}

}  // namespace

// ---------------------------------------------------------------------------
// Разворот локатора
// ---------------------------------------------------------------------------

std::vector<LocatedRoot> expandRuleLocator(const core::Rule& rule, FileSystemProbe& probe,
                                           const ExpandLimits& limits, std::stop_token token, bool* truncatedOut) {
    bool truncated = false;
    std::vector<LocatedRoot> roots = collectRoots(rule, probe, limits, token, truncated);
    if (truncatedOut != nullptr) *truncatedOut = truncated;
    return roots;
}

// ---------------------------------------------------------------------------
// Сбор
// ---------------------------------------------------------------------------

CollectResult collectCandidates(const core::RuleSet& rules, FileSystemProbe& probe,
                                const CollectOptions& options, std::stop_token token) {
    CollectResult result;
    CollectStats& stats = result.stats;
    stats.rulesTotal = static_cast<std::uint64_t>(rules.rules.size());

    std::unordered_set<std::string> claimedPaths;

    for (const core::Rule& rule : rules.rules) {
        if (token.stop_requested()) {
            result.canceled = true;
            ++stats.canceled;
            break;
        }

        // Правило неактивно — не ошибка, а штатное состояние набора
        // (docs/rules-authoring.md §4.1, §5.4). Молча пропускать нельзя: «часть
        // правил неактивна» должно быть видно и в логе, и в отчёте.
        const std::string pattern =
            normalizePath(rule.resolvedLocator.empty() ? rule.locator : rule.resolvedLocator);
        if (pattern.empty()) {
            ++stats.rulesSkippedEmptyLocator;
            addNote(stats, options.maxNotes, "правило " + rule.id + ": пустой locator");
            continue;
        }
        if (hasUnresolvedVariable(pattern)) {
            ++stats.rulesSkippedUnresolved;
            addNote(stats, options.maxNotes, "правило " + rule.id + ": неразрешённая переменная окружения в locator");
            core::LogFields fields;
            fields.push_back(core::logField("rule", rule.id));
            fields.push_back(core::logField("locator", pattern));
            core::logWarn("scan.collect.unresolved", "правило неактивно: переменная окружения не раскрылась",
                          std::move(fields));
            continue;
        }
        {
            const std::vector<std::string> segments = splitSegments(pattern);
            if (segments.empty() || !isAbsolutePattern(segments.front())) {
                ++stats.rulesSkippedRelative;
                addNote(stats, options.maxNotes, "правило " + rule.id + ": locator без корня тома");
                continue;
            }
        }

        std::int64_t minAgeDays = options.minAgeDaysOverride >= 0 ? options.minAgeDaysOverride : rule.minAgeDays;
        if (minAgeDays < 0) minAgeDays = 0;
        if (minAgeDays > kMaxMinAgeDays) {
            addNote(stats, options.maxNotes, "правило " + rule.id + ": minAgeDays обрезан до " +
                                                 std::to_string(kMaxMinAgeDays));
            minAgeDays = kMaxMinAgeDays;
        }
        if (minAgeDays > 0 && options.now <= 0) {
            // Без часов возрастной фильтр не имеет смысла: «моложе порога» и
            // «не моложе порога» отличаются от отсечения всего и от отсечения
            // ничего, а выбирать это на глаз нельзя.
            ++stats.rulesSkippedNoClock;
            addNote(stats, options.maxNotes, "правило " + rule.id + ": minAgeDays > 0, а время не задано");
            continue;
        }

        bool rootsTruncated = false;
        const std::vector<LocatedRoot> roots = expandRuleLocator(rule, probe, options.limits, token, &rootsTruncated);
        if (rootsTruncated) {
            // Предел сработал: показываем то, что нашли, но говорим, что список
            // неполон (SPEC §8 Этап 2, отчёт FR-8). Молчаливый неполный список
            // хуже явного: по нему видно и дефект правила, и цену обхода.
            ++stats.rootsTruncated;
            addNote(stats, options.maxNotes, "правило " + rule.id + ": список корней обрезан пределом " +
                                                 std::to_string(options.limits.maxRootsPerRule) + " корней");
        }
        if (roots.empty()) {
            ++stats.rulesWithoutRoots;
            addNote(stats, options.maxNotes, "правило " + rule.id + ": подходящих каталогов на диске нет");
            continue;
        }
        ++stats.rulesExpanded;
        stats.rootsFound += static_cast<std::uint64_t>(roots.size());

        // Корни одного правила, чьё содержимое целиком уже обойдено более
        // внешним корнем. Курсоры строятся обходом в ширину, поэтому внешний
        // корень всегда приходит раньше вложенного, и «съесть» содержимое
        // вложенного корня его пропуском нельзя.
        std::vector<std::string> coveringRoots;

        for (const LocatedRoot& root : roots) {
            if (token.stop_requested()) {
                result.canceled = true;
                ++stats.canceled;
                break;
            }

            ProbeEntry rootEntry;
            if (!probe.statPath(root.path, token, rootEntry)) {
                ++stats.probeErrors;
                addNote(stats, options.maxNotes, "правило " + rule.id + ": корень не читается — " + root.path);
                continue;
            }
            if (!claimedPaths.insert(toLowerAscii(root.path)).second) {
                // Два правила на один каталог = один и тот же файл в двух
                // элементах плана, а при применении — двойной учёт объёма.
                // Набор правил за это отвечает (docs/rules-authoring.md §6.7), но
                // молчать об этом нельзя: дефект виден только здесь.
                ++stats.duplicatePaths;
                addNote(stats, options.maxNotes, "путь заявлен несколькими правилами: " + root.path);
            }

            if (rule.excluded(root.path)) {
                ++stats.rootsExcluded;
                continue;
            }

            // Путь, каким его знает файловая система, а не склеенный нами: так
            // кандидат, отчёт и последующее удаление говорят об одном и том же
            // каталоге. §4 FR-6: нормализация — дело платформенного слоя.
            const std::string rootPath = rootEntry.path.empty() ? root.path : rootEntry.path;

            // Вложенный корень того же правила пропускаем: его файлы уже внутри
            // обхода внешнего корня, и второй кандидат считал бы их повторно —
            // и в агрегатах, и в плане. Пропускаем только тогда, когда внешний
            // корень берёт содержимое целиком (пустой leafPattern): с фильтром
            // листьев внешний корень смотрит лишь на свой верхний уровень, и
            // вложенный корень тогда не избыточен.
            const bool redundant =
                root.leafPattern.empty() &&
                std::any_of(coveringRoots.begin(), coveringRoots.end(), [&rootPath](const std::string& outer) {
                    return rootPath.size() > outer.size() && isInsideRoot(outer, rootPath);
                });
            if (redundant) {
                ++stats.rootsNestedSkipped;
                addNote(stats, options.maxNotes,
                        "корень вложен в другой корень того же правила, пропущен: " + rootPath);
                continue;
            }
            if (root.leafPattern.empty()) coveringRoots.push_back(rootPath);

            RootScan scan;
            scan.rule = &rule;
            scan.root = rootPath;
            scan.leafPattern = root.leafPattern;
            scan.ageFilter = minAgeDays > 0;
            scan.oldestAllowed = options.now - minAgeDays * kSecondsPerDay;
            scan.includeReparse = options.includeReparsePoints;
            scan.stats = &stats;

            if (rootEntry.isDirectory) {
                const bool completed =
                    probe.walk(rootPath, token, [&scan](const ProbeEntry& entry) { return scan.visit(entry); });
                if (!completed) {
                    result.canceled = true;
                    ++stats.canceled;
                }
            } else {
                // Правило указывает на файл («C:\Windows\MEMORY.DMP»): кандидат
                // из одного элемента, те же фильтры, что и для дерева.
                (void)scan.visit(rootEntry);
            }

            if (scan.tally.files == 0) {
                ++stats.candidatesEmpty;
                if (options.skipEmpty) continue;
            }

            core::CleanupCandidate candidate;
            candidate.ruleId = rule.id;
            candidate.category = rule.category;
            candidate.path = rootPath;
            candidate.displayName = candidateDisplayName(rule, root);
            candidate.logicalBytes = scan.tally.logical;
            candidate.allocatedBytes = scan.tally.allocated;
            candidate.fileCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                scan.tally.files, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
            candidate.oldestWrite = scan.tally.oldestWrite;
            candidate.newestWrite = scan.tally.newestWrite;
            candidate.lastAccess = scan.tally.lastAccess;
            candidate.safety = rule.safety;
            // confidence и содержательные reasons дописывает scoring_bridge
            // (CollectOptions::finalize). Ниже — только то, что знает сборщик.

            if (root.groupedByProfile && !root.profile.empty()) {
                candidate.reasons.push_back("профиль «" + root.profile + "»");
            }
            candidate.reasons.push_back("корень правила: " + rootPath);
            if (scan.tally.tooYoung > 0) {
                candidate.reasons.push_back("по возрасту отсечено файлов: " + std::to_string(scan.tally.tooYoung));
            }
            if (scan.tally.unknownAge > 0) {
                candidate.reasons.push_back("нет времени записи у файлов: " + std::to_string(scan.tally.unknownAge) +
                                            " (в кандидат не вошли)");
            }
            if (scan.tally.excludedFiles + scan.tally.prunedDirs > 0) {
                candidate.reasons.push_back("исключено правилом элементов: " +
                                            std::to_string(scan.tally.excludedFiles + scan.tally.prunedDirs));
            }
            if (scan.tally.reparse > 0) {
                candidate.reasons.push_back("пропущено ссылок: " + std::to_string(scan.tally.reparse));
            }
            if (scan.tally.allocatedEstimated > 0) {
                candidate.reasons.push_back("у части файлов аллоцированный размер неизвестен, учтён логический");
            }
            if (scan.tally.sizeUnknown > 0) {
                candidate.reasons.push_back("размер части файлов прочитать не удалось — объём может быть занижен");
            }
            if (scan.tally.outsideRoot > 0) {
                candidate.reasons.push_back("часть элементов вне корня правила не учтена");
            }

            if (options.finalize) {
                options.finalize(candidate);
            }

            ++stats.candidatesEmitted;
            result.candidates.push_back(std::move(candidate));
        }

        if (result.canceled) break;
    }

    core::LogFields fields;
    fields.push_back(core::logField("rules", stats.rulesTotal));
    fields.push_back(core::logField("roots", stats.rootsFound));
    fields.push_back(core::logField("candidates", stats.candidatesEmitted));
    fields.push_back(core::logField("files", stats.filesSeen));
    fields.push_back(core::logField("tooYoung", stats.filesTooYoung));
    fields.push_back(core::logField("inactiveRules", stats.rulesSkippedUnresolved + stats.rulesSkippedRelative +
                                                     stats.rulesSkippedEmptyLocator + stats.rulesSkippedNoClock));
    fields.push_back(core::logField("canceled", result.canceled));
    core::logInfo("scan.collect", "сбор кандидатов завершён", std::move(fields));

    return result;
}

std::string candidateDisplayName(const core::Rule& rule, const LocatedRoot& root) {
    const std::string title = rule.title();
    const std::string base = title.empty() ? rule.id : title;
    if (root.groupedByProfile && !root.profile.empty()) {
        return base + " — профиль " + root.profile;
    }
    return base;
}

}  // namespace mrproper::engine
