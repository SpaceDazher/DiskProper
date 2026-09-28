// Форматирование величин для интерфейса и отчётов (ru по умолчанию).
// Переносимый модуль: без Windows API.
#pragma once

#include <cstdint>
#include <string>

namespace mrproper::core {

// «1,2 ГБ» / «845 Б». binaryUnits=true даёт KiB/МиБ/ГиБ.
std::string formatBytes(std::uint64_t bytes, int decimals = 1, bool binaryUnits = false);

// «1 234 файла»
std::string formatCount(std::uint64_t count);

// «12,3 %»
std::string formatPercent(double fraction, int decimals = 1);

// «3 дн.» / «2 ч»
std::string formatAge(std::int64_t seconds);

}  // namespace mrproper::core
