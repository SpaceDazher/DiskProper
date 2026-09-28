#include "scoring.hpp"

#include "units.hpp"

namespace mrproper::core {

Score scoreCandidate(const ScoreInput& input) {
    Score score;
    score.safety = input.declaredSafety;
    int confidence = 100;

    // 1. Возраст: свежие файлы почти наверняка используются прямо сейчас.
    const std::int64_t minAge = input.minAgeDays * 86400;
    if (input.now > input.newestWrite) {
        const std::int64_t age = input.now - input.newestWrite;
        if (minAge > 0 && age < minAge) {
            const std::int64_t missing = minAge - age;
            // Ниже порога выбора: файл может использоваться прямо сейчас.
            confidence -= 60;
            score.reasons.push_back("часть файлов моложе порога правила: не хватает " +
                                    formatAge(missing) + " до безопасного возраста");
        } else {
            score.reasons.push_back("возраст: не моложе " + formatAge(minAge) +
                                    " (правило: minAgeDays=" + std::to_string(input.minAgeDays) + ")");
        }
    }

    // 2. Заблокированные файлы: удалить нельзя, значит и обещать нечего.
    if (input.lockedProcesses > 0) {
        confidence -= 30;
        score.reasons.push_back("файлы держат " + std::to_string(input.lockedProcesses) +
                                " работающих приложения — потребуется их закрыть");
    } else if (input.processClosedStateKnown) {
        score.reasons.push_back("приложения, которые используют эти файлы, закрыты");
    }

    // 3. Системный каталог: удаляем только по явному правилу с минимумом review.
    if (input.inSystemDirectory) {
        if (score.safety == SafetyLevel::Safe) score.safety = SafetyLevel::Review;
        confidence -= 10;
        score.reasons.push_back("системный каталог: уровень снижен до review");
    }

    // 4. Пользовательские данные: скоринг не имеет права считать их мусором «по умолчанию».
    if (input.insideUserProfile && input.declaredSafety == SafetyLevel::Safe) {
        confidence -= 15;
        score.reasons.push_back("объект внутри профиля пользователя — проверьте вручную");
    }

    // 5. Широкий шаблон: «что-то в Temp» вместо конкретного каталога.
    if (input.patternIsBroad) {
        confidence -= 20;
        score.reasons.push_back("шаблон правила широкий, совпадение может быть случайным");
    }

    // 6. Малый объём: цена риска не стоит выигрыша.
    if (input.allocatedBytes < 64ull * 1024) {
        confidence -= 15;
        score.reasons.push_back("слишком малый объём (" + formatBytes(input.allocatedBytes) +
                                ") — вклад в освобождение места незначим");
    }

    if (confidence > 100) confidence = 100;
    if (confidence < 0) confidence = 0;
    score.confidence = confidence;
    return score;
}

}  // namespace mrproper::core
