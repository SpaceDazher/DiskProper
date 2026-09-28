// Оценка кандидатов в очистку: уровень риска, уверенность и объяснения (SPEC §4 FR-4).
// Чистая функция от входных данных — вся логика тестируется без диска.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model.hpp"

namespace mrproper::core {

struct ScoreInput {
    std::string ruleId;
    SafetyLevel declaredSafety{SafetyLevel::Review};
    std::int64_t minAgeDays{};
    std::int64_t newestWrite{};    // unix-секунды
    std::int64_t now{};            // unix-секунды
    std::uint64_t allocatedBytes{};
    bool insideUserProfile{true};
    bool inSystemDirectory{false};
    std::uint32_t lockedProcesses{};
    bool processClosedStateKnown{false};
    bool patternIsBroad{false};    // правило без точного имени каталога
};

struct Score {
    SafetyLevel safety{SafetyLevel::Review};
    int confidence{};
    std::vector<std::string> reasons;
};

// Никогда не понижает объявленный в правиле уровень Safe: правило — это решение автора,
// а движок может только ужесточить (Review/Risky остаются как есть или повышаются).
Score scoreCandidate(const ScoreInput& input);

// Порог, ниже которого элемент по умолчанию не выбирается (пункт 12 «не удаляется без объяснения»).
inline constexpr int kDefaultConfidenceThreshold = 50;

}  // namespace mrproper::core
