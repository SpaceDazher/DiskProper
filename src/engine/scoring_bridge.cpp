// Реализация моста «кандидат → core::scoreCandidate» (SPEC §4 FR-4).
// Контракт, границы, инварианты и список «чего модуль не делает» — в
// scoring_bridge.hpp; здесь только код.
//
// Порядок чтения: путь и шаблон (isPathUnderRoot, isBroadLocator) →
// подстановка (makeScoreInput) → оценка ядра (core::scoreCandidate) →
// решения моста и текст вердикта (evaluate) → запись в кандидата (applyScore) →
// массовая оценка скана (scoreCandidates).
//
// Слой: файл не включает windows.h — подстановка данных не зависит от
// Windows, и её покрывают обычные юнит-тесты на хосте (SPEC §11 п.1).
#include "scoring_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mrproper::engine::scoring_bridge {
namespace {

// ---------------------------------------------------------------------------
// Текстовые мелочи, ради которых не нужен отдельный модуль
// ---------------------------------------------------------------------------

// Расширенная форма пути (§5, пути длиннее MAX_PATH): нормализованные пути
// приходят из platform::vfs_paths именно в этом виде, а корень профиля или
// системный корень платформа может отдать и в обычном. Снимаем префикс с обеих
// сторон — иначе один и тот же каталог дважды не совпадёт.
constexpr std::string_view kExtendedUncPrefix = "\\\\?\\UNC\\";
constexpr std::string_view kExtendedPrefix = "\\\\?\\";

char asciiLower(char c) noexcept {
    // Только ASCII: сворачивание регистра в полном Unicode требует таблицы
    // Unicode, а этот модуль сознательно её не тащит — сравнение регистра для
    // скоринга приблизительное, авторитетное сравнение живёт в
    // platform::vfs_paths (CompareStringOrdinal).
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreAsciiCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (asciiLower(left[i]) != asciiLower(right[i])) return false;
    }
    return true;
}

bool startsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool isSeparator(char c) noexcept { return c == '\\' || c == '/'; }

std::string_view stripExtendedPrefix(std::string_view path) noexcept {
    if (startsWith(path, kExtendedUncPrefix)) return path.substr(kExtendedUncPrefix.size());
    if (startsWith(path, kExtendedPrefix)) return path.substr(kExtendedPrefix.size());
    return path;
}

std::string_view trimTrailingSeparators(std::string_view path) noexcept {
    while (!path.empty() && isSeparator(path.back())) path.remove_suffix(1);
    return path;
}

bool isWildcardOnly(std::string_view segment) noexcept { return segment == "*" || segment == "**"; }

bool hasWildcard(std::string_view segment) noexcept {
    return segment.find_first_of("*?[") != std::string_view::npos;
}

// Локатор, по которому судим о «широкости». Развёрнутый приоритетнее: после
// подстановки переменных окружения в нём видно настоящие имена каталогов, а в
// сыром «%LOCALAPPDATA%\Temp\**» их нет вовсе. Если правило не развернулось
// (переменная не задана) — смотрим на исходный шаблон.
std::string_view ruleLocator(const core::Rule& rule) noexcept {
    if (!rule.resolvedLocator.empty()) return rule.resolvedLocator;
    return rule.locator;
}

// Граница между причинами сборщика и причинами моста. Список причин у
// кандидата один (SPEC §6.3), и переоценка (FR-9: пользователь сменил уровень
// правила) приходит на уже оценённого кандидата, поэтому мостовой блок должен
// узнаваться однозначно: он начинается со строки вердикта с префиксом
// kVerdictPrefix. Причины до неё принадлежат сборщику и сохраняются как есть.
std::vector<std::string> takeCollectorReasons(const std::vector<std::string>& reasons) {
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        if (startsWith(reasons[i], kVerdictPrefix)) {
            return std::vector<std::string>(reasons.begin(), reasons.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return reasons;
}

int clampConfidence(int confidence) noexcept {
    if (confidence < 0) return 0;
    if (confidence > 100) return 100;
    return confidence;
}

// Список имён через запятую: «msedge, chrome». Тот же приём, что у причин
// core::scoreCandidate («файлы держат 2 работающих приложения»), — просто
// перечислить, а не разбирать.
std::string joinNames(const std::vector<std::string>& names) {
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) joined += ", ";
        joined += names[i];
    }
    return joined;
}

// Одна строка вердикта: по ней человек видит, почему элемент выбран или
// пропущен (SPEC §12 — 0 кандидатов без объяснения).
std::string makeVerdict(const CandidateScore& score) {
    std::string verdict(kVerdictPrefix);
    verdict += std::string("уровень ") + core::toString(score.safety) + ", уверенность " +
               std::to_string(score.confidence) + " из 100 при пороге " + std::to_string(score.threshold);
    verdict += score.selectableByDefault ? " — выбирается по умолчанию"
                                         : " — по умолчанию не выбирается, только вручную";
    return verdict;
}

}  // namespace

// ---------------------------------------------------------------------------
// Проверки пути и шаблона
// ---------------------------------------------------------------------------

bool isPathUnderRoot(std::string_view path, std::string_view root) noexcept {
    // Завершающие разделители снимаются с обеих сторон: «C:\Windows» и
    // «C:\Windows\» — один корень. После этого «C:\Windows\Logs» внутри
    // «C:\Windows» определяется разделителем сразу после корня, а не
    // сравнением строк целиком (иначе «C:\Users\Ev» влезал бы в «C:\Users\Evil»).
    const std::string_view left = trimTrailingSeparators(stripExtendedPrefix(path));
    const std::string_view right = trimTrailingSeparators(stripExtendedPrefix(root));

    if (right.empty()) return false;  // корень не задан — границы нет
    if (left.size() < right.size()) return false;
    if (!equalsIgnoreAsciiCase(left.substr(0, right.size()), right)) return false;
    if (left.size() == right.size()) return true;  // путь и есть корень
    return isSeparator(left[right.size()]);
}

bool isBroadLocator(std::string_view locator) noexcept {
    if (locator.empty()) return true;

    // Идём справа налево и отбрасываем завершающие сегменты-подстановки:
    // «**» и «*» не называют каталог, а означают «всё, что ниже», поэтому
    // «...\Temp\**» широким не считается, а вот «%USERPROFILE%\*.*» — считается:
    // за последним значащим сегментом стоит подстановка, а не имя.
    std::string_view view = locator;
    std::string_view last;
    while (!view.empty()) {
        view = trimTrailingSeparators(view);
        if (view.empty()) break;
        const std::size_t slash = view.find_last_of("\\/");
        const std::string_view segment = (slash == std::string_view::npos) ? view : view.substr(slash + 1);
        if (isWildcardOnly(segment)) {
            view = (slash == std::string_view::npos) ? std::string_view{} : view.substr(0, slash);
            continue;
        }
        last = segment;
        break;
    }

    if (last.empty()) return true;  // весь шаблон — подстановка
    return hasWildcard(last);
}

bool isInsideUserProfile(std::string_view path, std::string_view userProfileRoot) noexcept {
    // Корень не передан — «не доказано, что это не пользовательские данные».
    // ScoreInput::insideUserProfile по умолчанию тоже true: дешевле снять 15
    // баллов уверенности, чем пометить пользовательский кэш как безопасный.
    if (trimTrailingSeparators(userProfileRoot).empty()) return true;
    return isPathUnderRoot(path, userProfileRoot);
}

bool isInSystemDirectory(std::string_view path, const std::vector<std::string>& systemRoots) noexcept {
    for (const std::string& root : systemRoots) {
        if (isPathUnderRoot(path, root)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Порог отбора
// ---------------------------------------------------------------------------

int selectionThreshold(const ScoringContext& context) noexcept {
    if (context.confidenceThreshold < 0) return 0;
    if (context.confidenceThreshold > 100) return 100;
    return context.confidenceThreshold;
}

bool passesThreshold(int confidence, int threshold) noexcept {
    return confidence >= threshold;  // порог включительный: ровно 50 при пороге 50 проходит
}

// ---------------------------------------------------------------------------
// Подстановка данных
// ---------------------------------------------------------------------------

core::ScoreInput makeScoreInput(const core::CleanupCandidate& candidate, const core::Rule* rule,
                                const ScoringContext& context) {
    core::ScoreInput input;
    input.ruleId = candidate.ruleId;
    input.declaredSafety = (rule != nullptr) ? rule->safety : core::SafetyLevel::Review;
    input.minAgeDays = (rule != nullptr) ? rule->minAgeDays : 0;
    input.now = context.now;
    // Неизвестное время записи приравниваем к «сейчас»: возраст не выдумывается,
    // но и не зачитывается как «достаточно старый» — при minAgeDays > 0 ядро
    // честно срежет уверенность за «моложе порога», а не повысит её за неизвестное.
    input.newestWrite = (candidate.newestWrite > 0) ? candidate.newestWrite : context.now;
    input.allocatedBytes = candidate.allocatedBytes;
    input.processClosedStateKnown = context.processClosedStateKnown;
    input.insideUserProfile = isInsideUserProfile(candidate.path, context.userProfileRoot);
    input.inSystemDirectory = isInSystemDirectory(candidate.path, context.systemRoots);
    // Правила нет — шаблон неизвестен, то есть заведомо широкий: путь не описан
    // ни одной строкой набора, и совпадение ничем не подтверждено.
    input.patternIsBroad = (rule == nullptr) || isBroadLocator(ruleLocator(*rule));

    const std::size_t locks = candidate.lockedBy.size();
    const std::size_t maxLocks = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    input.lockedProcesses = static_cast<std::uint32_t>(locks < maxLocks ? locks : maxLocks);
    return input;
}

// ---------------------------------------------------------------------------
// Оценка кандидата
// ---------------------------------------------------------------------------

CandidateScore evaluate(const core::CleanupCandidate& candidate, const core::Rule* rule, const ScoringContext& context) {
    // Счёт сам — у ядра (core::scoreCandidate). Мост отвечает только за то,
    // чего ядро не знает по определению: за набор правил и за окружение прогона.
    const core::Score scored = core::scoreCandidate(makeScoreInput(candidate, rule, context));

    CandidateScore result;
    result.ruleId = candidate.ruleId;
    result.ruleKnown = (rule != nullptr);
    result.threshold = selectionThreshold(context);
    result.safety = scored.safety;
    result.confidence = scored.confidence;
    // Не весь список: строки моста прошлой оценки отбрасываются, иначе
    // переоценка (FR-9) дописала бы вторую копию каждой причины.
    result.preservedReasons = takeCollectorReasons(candidate.reasons);

    // Сначала то, о чём человек обязан узнать, потом — причины ядра.
    std::vector<std::string> notes;
    if (!result.ruleKnown) {
        notes.push_back("правило «" + candidate.ruleId +
                        "» в наборе нет — путь не описан ни одной строкой правил, совпадение ничем не подтверждено");
        // Ядро не опускает уровень ниже объявленного правилом, а правила нет —
        // нет и объявления, поэтому мост ставит Review сам.
        result.safety = core::SafetyLevel::Review;
        result.confidence = clampConfidence(result.confidence - kUndescribedPathPenalty);
    }
    if (candidate.newestWrite <= 0) {
        notes.push_back("время последней записи неизвестно — возраст кандидата не оценивался");
    }
    if (!context.processClosedStateKnown && rule != nullptr && !rule->requiresProcessesClosed.empty()) {
        notes.push_back("состояние приложений не проверялось (Restart Manager не дал данных), правило требует закрытия: " +
                        joinNames(rule->requiresProcessesClosed));
    }
    notes.insert(notes.end(), scored.reasons.begin(), scored.reasons.end());
    result.reasons = std::move(notes);

    result.selectableByDefault = passesThreshold(result.confidence, result.threshold);
    result.verdict = makeVerdict(result);
    return result;
}

void applyScore(const CandidateScore& score, core::CleanupCandidate& candidate) {
    candidate.safety = score.safety;
    candidate.confidence = score.confidence;

    // Список пересобирается, а не дописывается: переоценка (FR-9 — пользователь
    // сменил уровень правила) не должна удваивать каждую строку объяснения в
    // плане, отчёте и UI.
    std::vector<std::string> reasons;
    reasons.reserve(score.preservedReasons.size() + score.reasons.size() + 1);
    reasons.insert(reasons.end(), score.preservedReasons.begin(), score.preservedReasons.end());
    if (!score.verdict.empty()) reasons.push_back(score.verdict);
    reasons.insert(reasons.end(), score.reasons.begin(), score.reasons.end());
    candidate.reasons = std::move(reasons);
}

// ---------------------------------------------------------------------------
// Массовая оценка
// ---------------------------------------------------------------------------

RuleLookup makeRuleLookup(const core::RuleSet& rules) {
    // Указатели берутся на правила самого набора: вызывающий обязан держать
    // rules живыми дольше, чем таблица.
    RuleLookup lookup;
    lookup.reserve(rules.rules.size() * 2 + 1);
    for (const core::Rule& rule : rules.rules) {
        if (rule.id.empty()) continue;
        lookup.insert_or_assign(rule.id, &rule);
    }
    return lookup;
}

std::size_t scoreCandidates(std::vector<core::CleanupCandidate>& candidates, const RuleLookup& lookup,
                            const ScoringContext& context) {
    std::size_t scored = 0;
    for (core::CleanupCandidate& candidate : candidates) {
        const auto found = lookup.find(candidate.ruleId);
        const core::Rule* rule = (found == lookup.end()) ? nullptr : found->second;
        applyScore(evaluate(candidate, rule, context), candidate);
        ++scored;
    }
    return scored;
}

std::size_t scoreCandidates(std::vector<core::CleanupCandidate>& candidates, const core::RuleSet& rules,
                            const ScoringContext& context) {
    const RuleLookup lookup = makeRuleLookup(rules);
    return scoreCandidates(candidates, lookup, context);
}

}  // namespace mrproper::engine::scoring_bridge
