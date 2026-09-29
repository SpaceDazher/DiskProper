// mrproper-cli scan — прогон сканирования, отчёт в stdout, прогресс в stderr.
//
// Спека: §6.2 («cli: mrproper-cli scan --json, --plan, --apply — headless-режим
// для CI и e2e»), §8 Этап 2 («Проверка: … mrproper-cli scan --json возвращает все
// 6 категорий, сумма allocatedBytes отличается от эталонного размера фикстур не
// более чем на 0.5 %; повторный скан идемпотентен»), §4 FR-4 (оценка
// кандидатов), §4 FR-8 (отчёт), §6.4 (прогресс и кооперативная отмена).
//
// ---------------------------------------------------------------------------
// Разделение каналов — то, ради чего команда и написана
// ---------------------------------------------------------------------------
//
//   stdout  ТОЛЬКО JSON отчёта: один документ, ни одной служебной строки.
//          Иначе `mrproper-cli scan --json | jq` в CI и в e2e-сценариях
//          (SPEC §11.0) падает на первой же строке прогресса, а «прогресс в
//          stdout» — это молчаливо испорченный канал данных: скрипт либо
//          разваливается, либо хуже — молча читает мусор.
//   stderr  Всё человеческое: ход работы, итог, предупреждения, разбор
//          аргументов, ошибки. Машина отсюда не читает ничего, поэтому
//          переписывание этих строк не ломает потребителя stdout.
//   код     Различает «готово», «ошибка аргументов», «нет набора правил»,
//          «скан упал», «прервано»: CI обязан отличать чистый ноль от пустого
//          результата, иначе упавший скан выглядит как «мусора нет».
//
// ---------------------------------------------------------------------------
// Слой
// ---------------------------------------------------------------------------
//
// Файл переносимый: ни windows.h, ни COM (SPEC §6.1, ADR-004). Всё, что знает
// про машину, приходит снаружи через ScanServices — обход ФС, часы, карта
// разделов, окружение для подстановки в локаторы правил. Отсюда два
// следствия, и оба полезны:
//
//   * разбор аргументов, сбор отчёта и печать прогресса покрываются обычными
//     юнит-тестами на любом хосте (SPEC §11.1), без диска и без Windows;
//   * CLI не решает, что удалять. Правила разворачивает и обходит
//     engine::collectCandidates, оценивает engine::scoring_bridge, отчёт пишет
//     core::report_json. Здесь только оркестрация команды и разделение каналов.
//
// Значение по умолчанию у ScanServices::createProbe — makePlatformScanProbe(),
// то есть мост из engine (engine/file_system_probe). Это единственное, что
// слой cli знает про платформу: функция объявлена здесь, определена в .cpp и не
// тащит за собой ни одного заголовка platform.
//
// Почему engine::FileSystemProbe не объявлен вперёд, а включён
// engine/candidate_collector.hpp: поле createProbe — это
// std::function<std::unique_ptr<FileSystemProbe>()> со значением по умолчанию,
// и std::function проверяет вызываемость возвращаемого значения при объявлении
// поля. Для std::unique_ptr это означает требование ПОЛНОГО типа в каждой
// единице трансляции, которая видит ScanServices (args.cpp в том числе):
// с объявлением вперёд такая единица не собирается («can't delete an
// incomplete type»). Заголовок при этом остаётся переносимым — сам
// candidate_collector.hpp не включает windows.h (ADR-004).
//
// Свойства вывода, на которые опирается e2e (SPEC §8 Этап 2, §11.4):
//
//   * JSON детерминирован: тот же вход — тот же байт в байт. Порядок
//     кандидатов задаётся сортировкой (category, ruleId, path), а не порядком
//     завершения задач в пуле: иначе golden-тест мигает от запуска к запуску;
//   * в JSON попадают только целые числа и строки — формат байтов остаётся
//     числом, а «1,2 ГБ» уходит в stderr;
//   * Risky-кандидаты в отчёте остаются: JSON — артефакт для CI и e2e, где
//     решение о чистке принимает потребитель, а «скрыть по умолчанию» —
//     требование FR-4 к интерфейсу, а не к машинному отчёту.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "engine/candidate_collector.hpp"

namespace mrproper::engine {
struct ProgressSnapshot;
struct ScanRunReport;
class ScanCoordinator;
}  // namespace mrproper::engine

namespace mrproper::cli {

// Коды возврата процесса. Числа выбраны так, чтобы скрипт CI отличал их без
// справочника: 0 — успех, 2 — ошибка аргументов (как у grep), 3 — нет набора
// правил, 4 — прогон не дал результата, 130 — прервано пользователем
// (128 + SIGINT, как у grep и git).
enum class ScanExit : int {
    Ok = 0,               // отчёт напечатан в stdout
    Usage = 2,            // неизвестный или неверный аргумент
    RulesUnavailable = 3, // набор правил не найден, не прочитан или невалиден
    ScanFailed = 4,       // прогон не дал ни одной завершённой задачи
    Interrupted = 130,    // прервано: Ctrl+C или --timeout
};

// Имя кода для текста в stderr и для журнала.
const char* toString(ScanExit code) noexcept;

// Адаптер обхода ФС по умолчанию: engine::FileSystemProbe над platform::vfs
// (engine/file_system_probe.hpp). Объявлен здесь, а определён в .cpp, чтобы слой
// cli не включал заголовки platform напрямую: команда остаётся переносимой и
// проверяется без диска, а мост — один на всю программу и живёт в engine.
[[nodiscard]] std::unique_ptr<engine::FileSystemProbe> makePlatformScanProbe();

// Строка «degraded» по накопленным счётчикам адаптера; пустая — условия полные.
[[nodiscard]] std::string platformScanProbeDiagnostics();

// Что нужно от окружения машины. Все поля необязательны, и у каждого есть
// осмысленный default: адаптер обхода ФС и диагностика его работы берутся из
// engine (file_system_probe), карта разделов — только по --with-disks, часы —
// системные. Явно присвоенное поле всегда сильнее default: вызывающий, который
// хочет прогнать команду без адаптера (тест «нет адаптера → ScanFailed»), пишет
// `services.createProbe = nullptr;`.
struct ScanServices {
    // Адаптер engine::FileSystemProbe над platform::vfs (обход, размеры,
    // нормализация путей). Вызывается по одному разу на задачу пула: обход
    // идёт в нескольких потоках, и адаптер не обязан быть потокобезопасным.
    // По умолчанию — makePlatformScanProbe: мост живёт в engine, а команда
    // остаётся переносимой и проверяемой без диска (§11.1). Пусто — сканировать
    // нечем, и команда скажет об этом в stderr и вернёт ScanFailed, а не упадёт.
    std::function<std::unique_ptr<engine::FileSystemProbe>()> createProbe = &makePlatformScanProbe;

    // Состояние адаптера обхода одной строкой: «degraded» с причиной (не
    // читаются корни, элемент исчез, аллоцированный размер не отдался) или
    // пустая строка, когда условия полные. Скан без прав админа читает не всё —
    // это повод сказать об этом в stderr и в notes отчёта, а не повод упасть
    // (§4 FR-1 «приложение не падает»). Не задана — строки не будет.
    std::function<std::string()> probeDiagnostics = &platformScanProbeDiagnostics;

    // Карта разделов для раздела «disks» отчёта (FR-2/FR-8). Не задана —
    // раздел остаётся пустым, структура отчёта не меняется. Запрашивается
    // флагом --with-disks: инвентаризация дисков стоит отдельного обхода
    // IOCTL, а `scan` по §6.2 занимается кандидатами.
    std::function<std::vector<core::PhysicalDisk>()> listDisks;

    // «Сейчас» в unix-секундах. Обязателен, если у правил есть minAgeDays:
    // без часов возрастной фильтр отсёк бы всё или ничего, а выдумывать время
    // нельзя (docs/rules-authoring.md §5.4). Не задан — system_clock.
    std::function<std::int64_t()> nowUnix;

    // Дамп окружения «NAME=value\n…» для подстановки в локаторы правил
    // (core::loadRuleFiles). Не задан — берётся процессное окружение по
    // списку переменных, которые используют правила.
    std::function<std::string()> environmentDump;

    // Версии для раздела environment отчёта (FR-8: «время, версии ОС и
    // приложения»). Пустые строки допустимы: неизвестное не выдумывается.
    std::string appVersion;
    std::string osCaption;
    std::string osVersion;
    std::string architecture;
    std::uint32_t osBuild{};

    // Данные для оценки кандидатов (engine::scoring_bridge::ScoringContext):
    // корень профиля пользователя и системные корни. Пустые значения означают
    // «не проверено» — мост тогда осторожничает в оценке, а не выдумывает.
    std::string userProfileRoot;
    std::vector<std::string> systemRoots;
};

// Разобранные аргументы команды. Структура отделена от запуска, чтобы разбор
// тестировался без диска, без набора правил и без координатора.
struct ScanOptions {
    bool help{};                    // -h / --help
    bool pretty{true};              // --compact выключает отступы
    bool quiet{};                   // --quiet: не печатать прогресс в stderr
    bool withDisks{};               // --with-disks: добавить карту разделов
    std::string rulesPath{"rules"}; // --rules <каталог>
    std::vector<std::string> categories;  // --category <id>, повторяемый; пусто — все
    std::int64_t minConfidence{-1};       // --min-confidence N; -1 — без фильтра
    std::int64_t minAgeDays{-1};          // --min-age N; -1 — как объявлено в правиле
    std::size_t workers{};                // --workers N; 0 — выбрать автоматически
    std::chrono::milliseconds progressInterval{1000};  // --progress-interval <длительность>
    std::chrono::milliseconds timeout{0};                // --timeout <длительность>; 0 — без предела
    std::string note;                                     // --note <текст> → отчёт.notes
};

// Справка по команде (печатается в stderr и в общий --help).
[[nodiscard]] std::string scanUsageText();

// Разобрать аргументы команды (без имени программы и без слова «scan»).
// false — с текстом в error; ScanOptions в этом случае не определены.
[[nodiscard]] bool parseScanOptions(const std::vector<std::string>& args, ScanOptions& options,
                                    std::string& error);

// Одна строка прогресса для stderr — без завершающего перевода строки,
// чтобы вызывающий сам решал, как её завершить.
[[nodiscard]] std::string formatScanProgress(const engine::ProgressSnapshot& snapshot);

// Команда целиком. args — аргументы после слова «scan». out — машинный stdout
// (сюда уходит только JSON), err — человеческий stderr. Возвращает код
// возврата процесса; int, а не ScanExit, чтобы main.cpp не зависел от
// перечисления (нулевой код возврата в C — это всё равно int).
[[nodiscard]] int runScan(const std::vector<std::string>& args, const ScanServices& services, std::ostream& out,
                          std::ostream& err);

}  // namespace mrproper::cli
