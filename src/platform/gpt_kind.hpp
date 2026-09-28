// Тип раздела GPT (PartitionType) → core::PartitionKind: что за GUID, кто его
// придумал и как его показывать. Спека: §6.3 (Partition::gptType и
// Partition::kind), §4 FR-2 (карта разделов показывает «системный, MSR,
// Recovery, OEM, EFI, Basic Data»), §4 FR-8 (то же самое в отчёте).
//
// Модуль — это таблица и ничего кроме таблицы. Он не читает разметку, не знает
// про атрибуты GPT и не обращается к WinAPI: GUID приходит уже разобранным
// (core::Guid из заголовка раздела), на выходе — вид раздела, имя типа для UI и
// признак «служебный / пользовательские данные». Поэтому проверяется он на
// любом хосте, а Windows этому файлу не нужна: переносимая логика не должна
// тащить windows.h (SPEC §6.1, §6.2 — единственная зависимость слоя вниз, на
// core).
//
// Про порядок байт в core::Guid. Это 16 байт в порядке записи из строки
// «xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx» (RFC 4122), а НЕ little-endian
// раскладка структуры GUID из Windows. Разница спрятана у вызывающего:
// приведение Win32 GUID → core::Guid делает platform::layout при чтении
// IOCTL_DISK_GET_DRIVE_LAYOUT_EX, и наружу она не выходит. Здесь важно, что
// таблица записана в том же порядке, что и строковые GUID в комментариях, —
// иначе GUID «повернётся» и перестанет совпадать с тем, что прислал драйвер.
//
// Границы модуля (что он сознательно НЕ делает):
//   * не читает разметку и не разбирает атрибуты GPT — это platform::layout;
//   * не разбирает байт типа MBR — это тоже platform::layout;
//   * не переводит Win32 GUID в core::Guid (у layout это приватная деталь);
//   * не локализует названия типов: перевод вида раздела живёт в UI и отчёте
//     (core::report_html, §4 FR-8), а здесь имена из спецификации и из SDK.
//
// Про соседний модуль — важно для ревью. platform::layout держит собственный
// сокращённый список типов для своего разбора и объявляет kindFromGptType,
// gptTypeName и gptTypeLabel. Это дублирование таблиц, а не два разных
// ответа: на общих записях (Basic Data, ESP, MSR, WinRE, LDM, Storage
// Spaces, Windows System / Main OS / OS Data, Pre-installed / BSP / DPP /
// Patch, Legacy boot) классификация у нас совпадает. Функции здесь названы
// иначе намеренно: одно и то же определение в одной статической библиотеке
// дало бы LNK2005. Сведение таблиц в одну — правка владельца layout, не моя
// (в этом прогоне чужие файлы не трогаю).
//
// Про правила очистки. FR-4 оценивает кандидата — то есть путь файла, а не
// раздел, поэтому gptKindMayHoldUserData ничего не разрешает удалять: это
// консервативная подсказка для карты разделов и отчёта и опора для будущей
// проверки «правило не должно выходить за пределы пользовательского раздела».
// Unknown считается служебным: «не знаю» — не повод предлагать раздел.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "core/model.hpp"

namespace mrproper::platform {

// Чей это GUID. Нужен не для красоты: по семейству видно, откуда взялась
// метка в отчёте — системный раздел Windows, дистрибутив Linux или раздел
// производителя ноутбука, — и это же семейство объясняет, почему GUID может
// встретиться на машине без Windows.
enum class GptTypeFamily : std::uint8_t {
    Unknown = 0,  // GUID не в таблице либо семейство не определяется
    Uefi,         // спецификация UEFI/PI: ESP, зарезервированные типы
    Microsoft,    // Windows Kits shared/diskguid.h
    Linux,        // util-linux (gdisk/blkid), fdisk
    Apple,        // типы разделов macOS/iOS
    Vmware,       // VMFS и прочее оборудование ESXi
    Other,        // известный тип без выраженного семейства
};

// Известный тип раздела. kind — ответ для модели (§6.3), name — имя из
// спецификации или из SDK (не локализуется: его показывают как есть, потому
// что по нему ищут в диспетчере дисков), family — источник имени.
struct GptTypeInfo {
    core::PartitionKind kind{core::PartitionKind::Unknown};
    GptTypeFamily family{GptTypeFamily::Unknown};
    const char* name{};  // nullptr, если GUID в таблице нет
};

// Основной вход модуля: PartitionType из заголовка раздела → вид раздела.
// Неизвестный GUID даёт core::PartitionKind::Unknown — «не знаю», а не
// «первый похожий» и не «для пользователя это просто данные».
[[nodiscard]] core::PartitionKind gptKindFromType(const core::Guid& type) noexcept;

// Полная запись таблицы или std::nullopt для неизвестного GUID. Вызывающему,
// которому нужен ещё и текст типа, не приходится искать дважды.
[[nodiscard]] std::optional<GptTypeInfo> gptTypeInfo(const core::Guid& type) noexcept;

// Есть ли GUID в таблице. Годится для отчёта: «тип известен» и «тип не узнан»
// печатаются по-разному, чтобы расхождение с Get-Disk было видно глазом.
[[nodiscard]] bool isKnownGptType(const core::Guid& type) noexcept;

// Имя типа для UI и отчёта; nullptr для неизвестного GUID.
[[nodiscard]] const char* gptKindName(const core::Guid& type) noexcept;

// То же, но всегда строка: имя типа, а для незнакомого GUID — его каноническая
// запись. Отчёт от этого остаётся полным: «не знаю» не должно выглядеть как
// «ничего не было».
[[nodiscard]] std::string gptKindLabel(const core::Guid& type);

// Имя семейства (для отчёта и лога). Не локализуется.
[[nodiscard]] const char* gptFamilyName(GptTypeFamily family) noexcept;

// Служебный раздел: данных пользователя на нём не ждём (MSR, служебные
// области динамических дисков, Storage Spaces, swap). Такой раздел не
// предлагают к очистке и не показывают как «свободное место».
[[nodiscard]] bool gptKindIsService(core::PartitionKind kind) noexcept;

// На разделе могут лежать пользовательские файлы. True для Basic Data, OEM и
// системного раздела ОС; Recovery — это WinRE.wim, пользовательских файлов там
// нет. Unknown и Reserved — false.
[[nodiscard]] bool gptKindMayHoldUserData(core::PartitionKind kind) noexcept;

}  // namespace mrproper::platform
