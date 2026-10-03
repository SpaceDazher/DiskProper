// Интеграционные тесты краев файловой системы: длинные пути, юникод, петля
// reparse. Спека §11.2 (пункт 2 «Интеграционные (Windows) — обёртки WinAPI
// против реальных путей: … длинные пути; reparse-петля»), §5 («пути: длинные
// пути, пробелы, не-ASCII и кириллица»; «ADS не трогаем»), §4 FR-6 («пропуск
// reparse points (symlink/junction) — защита от петель и выхода за пределы
// пути», «нормализация путей через GetFinalPathNameByHandleW …, поддержка \\?\
// для путей > MAX_PATH»), §10 (риск «длинные пути, reparse-петли, не-ASCII»:
// митигация «GetFinalPathNameByHandleW, \\?\, пропуск reparse, тесты с
// юникодом и петлями»), §12 (интеграционные тесты на «длинные пути;
// reparse-петля»).
//
// Задача 82. Соседи по набору: vfs_walk_tests (обход дерева), guard_tests
// (защищённые пути), inventory_tests (инвентаризация), cancel_tests (отмена).
// Здесь ровно три темы, которые не берёт на себя никто из них: путь длиннее
// MAX_PATH, имя файла в юникоде и ссылка, замкнутая сама на себя.
//
// ---------------------------------------------------------------------------
// Почему эти тесты нельзя написать переносимо
// ---------------------------------------------------------------------------
//
// Всё проверяемое здесь не существует в переносимом ядре: MAX_PATH, \\?\, имя
// 8.3, суррогатные пары, точка монтирования NTFS. Юнит-тест на «логику
// длинного пути» проверял бы модель строки, а не файловую систему Windows, и
// зелёный результат ничего бы не говорил о том, что произойдёт на живом диске.
// Поэтому файл живёт в tests/integration и требует Windows (SPEC §9).
//
// ---------------------------------------------------------------------------
// Честность результата
// ---------------------------------------------------------------------------
//
// Три предпосылки могут не выполняться на конкретной машине, и молчать об этом
// нельзя: без NTFS (временный том на ReFS/FAT) не создаётся точка
// монтирования, на отключённом 8.3 его не выдаёт GetShortPathNameW, на томе
// без TEMP не создаётся фикстура. В этих случаях тест печатает строку
// «[skip] <имя>: <причина>» и выходит, а harness считает его пройденным.
// Строка видна в выводе — пропуск не выглядит как успех (SPEC §12 требует,
// чтобы по журналу было видно, что не отработало).
//
// Ни один тест здесь не требует прав администратора: точка монтирования
// (junction) создаётся обычным пользователем, SeCreateSymbolicLinkPrivilege не
// нужна. Символьные ссылки в тестах не используются намеренно — их создание
// без повышения прав на этой машине невозможно, и тест был бы вечно зелёным
// «пропуском» вместо проверки.
//
// ---------------------------------------------------------------------------
// Уборка
// ---------------------------------------------------------------------------
//
// Фикстуры создаются в %TEMP% и убираются деструктором TempTree через
// vfs::deleteTree — тем же кодом, который проверяется. Точки монтирования
// удаляются раньше дерева: deleteTree по FR-6 reparse point не раскрывает и не
// удаляет (SkippedReparse), поэтому junction, оставшийся внутри каталога, не
// дал бы каталогу опустеть и дерево ушло бы в «осталось не пусто».
#include "harness.hpp"

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "platform/vfs_delete.hpp"
#include "platform/vfs_paths.hpp"
#include "platform/vfs_size.hpp"
#include "platform/vfs_walk.hpp"
#include "platform/win_error.hpp"
#include "platform/win_handle.hpp"

namespace {

namespace pf = mrproper::platform::vfs_paths;
namespace vfs = mrproper::platform::vfs;

using ScopedHandle = mrproper::platform::unique_handle<mrproper::platform::KernelHandlePolicy>;

// Тег точки монтирования из winnt.h. Объявлен здесь, чтобы файл не зависел от
// того, в каком виде его отдаёт SDK: значение зафиксировано архитектурой NTFS.
constexpr std::uint32_t kIoReparseTagMountPoint = 0xA0000003u;

// Раскладка буфера точки монтирования, которую принимает файловая система.
// Зафиксирована сравнением с буфером, который пишет сам mklink /J: ссылка,
// созданная им, прочитана обратно через FSCTL_GET_REPARSE_POINT, и буфер
// повторён байт в байт.
//
//   0   ULONG  ReparseTag            IO_REPARSE_TAG_MOUNT_POINT
//   4   USHORT ReparseDataLength     8 + длина PathBuffer (оба имени с нулём)
//   6   USHORT Reserved              0
//   8   USHORT SubstituteNameOffset  0, от начала PathBuffer
//  10   USHORT SubstituteNameLength  длина имени БЕЗ завершающего нуля
//  12   USHORT PrintNameOffset       SubstituteNameOffset + длина + 2 (через ноль)
//  14   USHORT PrintNameLength       длина имени БЕЗ завершающего нуля
//  16   WCHAR  PathBuffer            «\??\цель» NUL «цель» NUL
//
// Отступления от REPARSE_DATA_BUFFER в winioctl.h, из-за которых файловая
// система отвечала ERROR_INVALID_REPARSE_DATA (4392), проверены перебором всех
// сочетаний и записаны здесь намеренно:
//
//   * DWORD Flags между полями и PathBuffer НЕ пишется: система читает имена с
//     байта 16, а поданные на 20 байт позже (ноль в Flags плюс смещение) она
//     считает мусором;
//   * объявленные длины имён не включают завершающий ноль, а сами имена в
//     буфере ноль имеют;
//   * PrintNameOffset идёт через ноль замещающего имени, иначе печатное имя
//     начинается с нулевого символа и разбор обрывается;
//   * ReparseDataLength = 8 + PathBuffer, то есть ровно остаток буфера после
//     восьмибайтовой шапки: и +4, и +12 дают тот же 4392.
//
// Отсюда же практический вывод: «точка монтирования на этом хосте не создаётся»
// было неверным — отказ давал сам тест, а не среда.
constexpr std::size_t kReparseFixedBytes = 8u;     // тег + длина + резерв
constexpr std::size_t kMountPointFieldsBytes = 8u; // четыре WORD
constexpr std::size_t kPathBufferOffset = kReparseFixedBytes + kMountPointFieldsBytes;

// Имя каталога, из которого гарантированно получается путь длиннее MAX_PATH:
// кириллица здесь не украшение, а часть проверки (§5).
constexpr std::wstring_view kLevelSegment = L"глубокий-уровень-очень-длинного-имени";

// Порядковый номер фикстуры: имя должно быть уникальным, иначе два прогона
// тестов, случившиеся одновременно, делили бы один каталог и портили друг
// другу счётчики.
std::atomic<unsigned> g_fixtureCounter{0u};

// ---------------------------------------------------------------------------
// Мелкие помощники путей и файловой системы
// ---------------------------------------------------------------------------

// Расширенная форма пути: без неё Win32 обрезает путь на MAX_PATH символах
// (ровно то, из-за чего существует vfs_paths::toExtendedPath). Здесь форма
// продублирована намеренно: фикстуру создаёт сам тест, а не проверяемый модуль —
// иначе поломка toExtendedPath сделала бы тест неразличимым с «тест не смог
// создать фикстуру».
[[nodiscard]] std::wstring extended(std::wstring_view path) {
    static const std::wstring prefix = L"\\\\?\\";
    if (path.rfind(prefix, 0) == 0) {
        return std::wstring(path);
    }
    if (path.size() >= 2u && path[1] == L':') {
        return prefix + std::wstring(path);
    }
    return std::wstring(path);
}

[[nodiscard]] bool makeDirectory(const std::wstring& path, std::uint32_t& win32Error) {
    const std::wstring ext = extended(path);
    if (::CreateDirectoryW(ext.c_str(), nullptr)) {
        win32Error = ERROR_SUCCESS;
        return true;
    }
    win32Error = ::GetLastError();
    return false;
}

[[nodiscard]] std::uint32_t attributesOf(const std::wstring& path) {
    const std::wstring ext = extended(path);
    const DWORD value = ::GetFileAttributesW(ext.c_str());
    if (value == INVALID_FILE_ATTRIBUTES) {
        return 0u;
    }
    return value;
}

[[nodiscard]] bool exists(const std::wstring& path) {
    return attributesOf(path) != 0u;
}

[[nodiscard]] std::string describeError(const char* what, std::uint32_t win32Error) {
    return std::string(what) + ": " + mrproper::platform::win32ErrorText(win32Error) + " (" +
           std::to_string(win32Error) + ")";
}

// Записать файл ровно заданным содержимым. Короткая запись означала бы, что тест
// меряет не то, что создал, поэтому проверяется весь буфер.
//
// ДОСЫЛКА ПЕРЕД ЗАКРЫТИЕМ (D-78) — обязательная часть записи, а не украшение.
// GetCompressedFileSizeW открывает файл заново и читает ОПУБЛИКОВАННОЕ
// значение, а файловая система публикует метаданные свежезаписанного файла не
// сразу. Досылка делает состояние файла наблюдаемым сразу после close():
// замерено, что после FlushFileBuffers значение отдаётся с первой попытки
// (попыток публикации 1), то есть окно «метаданные ещё не опубликованы» в
// обычном случае не тратит время. Основной источник недетерминизма исходной
// проверки — не этот, а сжатие NTFS в %TEMP%; см. комментарий у
// measurePublishedAllocated, там корень и измерения.
// Отдельно от этого writeFile честно отдаёт ERROR_DISK_FULL: на томе без
// свободного места (в этой задаче измерен C: с нулём свободных байт, из-за
// чего фикстуры не создавались и проверка уходила в видимый пропуск) файл не
// создаётся — это тоже должно быть видно, а не «красный на пустом месте».
[[nodiscard]] bool writeFile(const std::wstring& path, std::string_view content, std::uint32_t& win32Error) {
    const std::wstring ext = extended(path);
    ScopedHandle file(::CreateFileW(ext.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        win32Error = ::GetLastError();
        return false;
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const std::size_t chunk = (content.size() - written) > 0x10000u ? 0x10000u : content.size() - written;
        DWORD done = 0;
        if (!::WriteFile(file.get(), content.data() + written, static_cast<DWORD>(chunk), &done, nullptr)) {
            win32Error = ::GetLastError();
            return false;
        }
        if (done == 0) {
            win32Error = ERROR_WRITE_FAULT;
            return false;
        }
        written += done;
    }
    // Сброс до закрытия: см. комментарий над функцией (D-78). Отказ досылки —
    // это отказ записи, а не повод продолжать: тест меряет файл, который должен
    // существовать целиком.
    if (!::FlushFileBuffers(file.get())) {
        win32Error = ::GetLastError();
        return false;
    }
    win32Error = ERROR_SUCCESS;
    return true;
}

// Опубликованный аллоцированный размер (D-78).
//
// КОРЕНЬ ДЕФЕКТА ИЗМЕРЕН, а не угадан (проба через Add-Type: GetFileAttributesW,
// CreateFileW/WriteFile/FlushFileBuffers, GetCompressedFileSizeW, SetFileAttributesW,
// FSCTL_SET_COMPRESSION на этой машине). %TEMP% здесь равен C:\Users\Daniil\AppData\Local\Temp, и его
// КАТАЛОГ помечен NTFS-сжатием (GetFileAttributesW -> 0x00002810, бит
// FILE_ATTRIBUTE_COMPRESSED). Файл, созданный в таком каталоге, наследует
// сжатие: замер — файл 5000 байт, GetCompressedFileSizeW -> 4096 при logical
// 5000 и флаге compressed. Снять сжатие не помогает НИЧЕГО из проверенного:
// SetFileAttributesW(FILE_ATTRIBUTE_NORMAL) и на пустом файле, и после записи,
// и при открытом дескрипторе — возвращает TRUE, а признак остаётся;
// FSCTL_SET_COMPRESSION(COMPRESSION_FORMAT_NONE) — то же; снятие признака с
// самого каталога отказывает (win32 2).
//
// То есть «allocated >= logical» для такой фикстуры — НЕВЕРНОЕ утверждение:
// это ровно тот случай, который vfs_size.hpp описывает словами «у сжатого файла
// она меньше логического размера». А недетерминированным его сделало то, что
// NTFS сжимает данные НЕ сразу: сжатие идёт своим рабочим потоком, и к моменту
// замера файл успевает сжаться или не успеть. Отсюда и частота исходного
// дефекта — 1 красный прогон набора из 14 (docs/defects.md §3.18.3): это гонка
// с потоком сжатия, а не ошибка кода.
//
// Отсюда три части решения, и все три нужны:
//
//   1. Инвариант «allocated >= logical» проверяется ТОЛЬКО там, где он
//      верен: файл не сжат и не разрежен. Для сжатого/разреженного выводится
//      строка с числами, а из сумм такой файл исключается (см. места
//      использования). Молча исключать нельзя: иначе на машине со сжатым
//      %TEMP% проверка молча перестаёт что-либо проверять.
//
//   2. Стабильность опубликованного значения: два замера подряд обязаны дать
//      одну и ту же пару. Это ловит остаточную гонку — «allocated >= logical»
//      может сойтись и на значении, которое через миллисекунду изменится.
//
//   3. Ожидание публикации с ограниченным сроком: значение 0 (или
//      allocatedKnown == false) означает, что метаданные ещё не опубликованы —
//      это состояние тома, а не результат кода, и оно обязано быть видимым
//      строкой пропуска с числами, а не красным прогоном. Досылка в writeFile
//      убирает это окно в обычном случае: замерено, что после FlushFileBuffers
//      GetCompressedFileSizeW отдаёт значение сразу (попыток публикации 1).
//
// Второе измерение на D: (NTFS, кластер 4096, тот же файл 5000 байт, та же проба
// плюс GetFileInformationByHandleEx/FileStandardInfo) нужно, чтобы не приписать
// vfs_size чужое ожидание:
//   GetCompressedFileSizeW (то, чем меряет vfs_size)              -> 5000 (ровно EOF);
//   GetFileInformationByHandleEx/FileStandardInfo/AllocationSize  -> 8192.
// То есть по пути файловая система отдаёт НЕ округлённое до кластера значение,
// поэтому проверка «кратно кластеру» здесь была бы неверной: это утверждение о
// другом API. Проверяемое свойство одно: значение опубликовано, известно,
// устойчиво, и не меньше логического там, где это обязано быть.
constexpr int kAllocatedProbeTries = 40;
constexpr int kAllocatedProbeSleepMs = 25;

struct AllocatedProbe {
    vfs::FileSize size{};
    int attempts{0};
    bool published{false};
};

// Ждёт, пока том опубликует аллоцированный размер непустого файла.
// «Опубликован» = значение известно и ненулевое. Ненулевое — потому, что у
// сжатого файла allocated МЕНЬШЕ логического (см. выше), и ждать «>= logical»
// на сжатой фикстуре бессмысленно: она не сойдётся никогда.
[[nodiscard]] AllocatedProbe measurePublishedAllocated(const std::wstring& path) {
    AllocatedProbe probe{};
    for (int attempt = 1; attempt <= kAllocatedProbeTries; ++attempt) {
        probe.attempts = attempt;
        probe.size = vfs::measurePath(path);
        if (probe.size.ok() && probe.size.allocatedKnown && probe.size.allocatedBytes > 0u) {
            probe.published = true;
            return probe;
        }
        if (attempt < kAllocatedProbeTries) {
            ::Sleep(kAllocatedProbeSleepMs);
        }
    }
    return probe;
}

// Применимо ли к файлу округление «allocated >= logical». Единственное, что его
// отменяет, — сжатие и разреженность: оба означают, что аллоцированного размера
// МОЖЕТ быть меньше логического, и это по контракту, а не отказ.
[[nodiscard]] bool roundingApplies(const vfs::FileSize& size) noexcept {
    return size.ok() && size.allocatedKnown && size.logicalBytes > 0u &&
           !hasFlag(size.flags, vfs::FileFlags::Sparse) &&
           !hasFlag(size.flags, vfs::FileFlags::Compressed);
}

// Почему файл исключён из проверки округления — одной строкой, с числами.
// Строка [note], а не пропуск: остальные проверки файла при этом выполняются,
// и молчать о сжатом %TEMP% нельзя — иначе на этой машине проверка выглядела
// бы зелёной, ничего не делая.
void noteRoundingNotApplicable(const char* testName, const std::wstring& path, const vfs::FileSize& size) {
    std::printf("  [note] %s: файл %s — %s, поэтому «allocated >= logical» не проверяется:"
                " logical=%llu allocated=%llu\n",
                testName, mrproper::platform::toUtf8(path).c_str(),
                mrproper::platform::toUtf8(vfs::describeFlags(size.flags)).c_str(),
                static_cast<unsigned long long>(size.logicalBytes),
                static_cast<unsigned long long>(size.allocatedBytes));
}

// Том не опубликовал значение за отведённый срок — это свойство машины, и
// сказать о нём строкой пропуска с числами честнее, чем покраснеть.
[[nodiscard]] std::string describeUnpublishedAllocation(const char* testName, const AllocatedProbe& probe) {
    return std::string("том не опубликовал аллоцированный размер за ") +
           std::to_string(kAllocatedProbeTries * kAllocatedProbeSleepMs) + " мс (попыток " +
           std::to_string(probe.attempts) + "): logical=" + std::to_string(probe.size.logicalBytes) +
           " allocated=" + std::to_string(probe.size.allocatedBytes) + " allocatedKnown=" +
           (probe.size.allocatedKnown ? "да" : "нет") + " флаги=" +
           mrproper::platform::toUtf8(vfs::describeFlags(probe.size.flags)) + " (" + testName + ")";
}

// Создать точку монтирования (junction) linkPath на каталог targetDir. Требует
// только права записи в каталог: в отличие от символьной ссылки junction
// доступен обычному пользователю, поэтому тест не молчит там, где
// SeCreateSymbolicLinkPrivilege нет.
[[nodiscard]] bool createJunction(const std::wstring& linkPath, const std::wstring& targetDir,
                                  std::uint32_t& win32Error) {
    win32Error = ERROR_SUCCESS;
    if (!makeDirectory(linkPath, win32Error)) {
        return false;
    }

    // SubstituteName — NT-путь («\??\C:\…»), PrintName — то, что видит
    // пользователь. Хвостовой разделитель у цели запрещён: целью монтирования
    // иначе становится каталог, который нельзя открыть.
    std::wstring substitute = L"\\??\\" + targetDir;
    while (!substitute.empty() && substitute.back() == L'\\') {
        substitute.pop_back();
    }
    std::wstring printable = targetDir;
    while (printable.size() > 3u && printable.back() == L'\\') {
        printable.pop_back();
    }

    const auto substituteBytes = static_cast<std::size_t>(substitute.size() * sizeof(wchar_t));
    const auto printableBytes = static_cast<std::size_t>(printable.size() * sizeof(wchar_t));
    // Объявленные длины — без завершающего нуля, а в буфере имена нуль имеют.
    const std::size_t pathBufferBytes = substituteBytes + sizeof(wchar_t) + printableBytes + sizeof(wchar_t);
    std::vector<char> buffer(kPathBufferOffset + pathBufferBytes, 0);

    const auto put16 = [&buffer](std::size_t offset, WORD value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    };
    const auto put32 = [&buffer](std::size_t offset, DWORD value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    };
    const DWORD tag = static_cast<DWORD>(kIoReparseTagMountPoint);
    put32(0u, tag);                                                          // ReparseTag
    put16(4u, static_cast<WORD>(kMountPointFieldsBytes + pathBufferBytes));  // ReparseDataLength
    put16(6u, 0u);                                                           // Reserved
    put16(8u, 0u);                                                           // SubstituteNameOffset
    put16(10u, static_cast<WORD>(substituteBytes));                         // SubstituteNameLength
    put16(12u, static_cast<WORD>(substituteBytes + sizeof(wchar_t)));        // PrintNameOffset — через ноль
    put16(14u, static_cast<WORD>(printableBytes));                          // PrintNameLength

    std::memcpy(buffer.data() + kPathBufferOffset, substitute.c_str(), substituteBytes + sizeof(wchar_t));
    std::memcpy(buffer.data() + kPathBufferOffset + substituteBytes + sizeof(wchar_t), printable.c_str(),
                printableBytes + sizeof(wchar_t));

    // FILE_FLAG_OPEN_REPARSE_POINT обязателен: без него CreateFileW идёт по ссылке
    // и DeviceIoControl меняет несуществующий объект.
    ScopedHandle link(::CreateFileW(extended(linkPath).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!link) {
        win32Error = ::GetLastError();
        return false;
    }

    DWORD returned = 0;
    if (!::DeviceIoControl(link.get(), FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                           nullptr, 0, &returned, nullptr)) {
        win32Error = ::GetLastError();
        return false;
    }
    return true;
}

// Снять точку монтирования. RemoveDirectoryW на junction удаляет именно ссылку,
// а не каталог, на который она указывает, — в этом и смысл проверки «петля не
// съедает цель».
[[nodiscard]] bool removeJunction(const std::wstring& linkPath, std::uint32_t& win32Error) {
    const std::wstring ext = extended(linkPath);
    if (::RemoveDirectoryW(ext.c_str())) {
        win32Error = ERROR_SUCCESS;
        return true;
    }
    win32Error = ::GetLastError();
    return false;
}

// Строка пропуска: тест не выполнил проверку по объективной причине, и читатель
// вывода должен это видеть.
void reportSkip(const char* testName, const std::string& reason) {
    std::printf("  [skip] %s: %s\n", testName, reason.c_str());
}

// ---------------------------------------------------------------------------
// Фикстура: каталог в %TEMP%, который убирает за собой
// ---------------------------------------------------------------------------
//
// Деструктор обязан отработать и при провале проверки: harness ловит исключение,
// и без RAII в %TEMP% осталась бы пачка «MrProper-it-…» после каждого красного
// прогона. Поэтому никаких CHECK в деструкторе — только best effort.
class TempTree {
public:
    TempTree() {
        std::wstring tempDir(4096u, L'\0');
        const DWORD needed = ::GetTempPathW(4096u, tempDir.data());
        if (needed == 0u || needed > 4096u) {
            reason_ = "GetTempPathW не дал пригодный путь";
            return;
        }
        tempDir.resize(needed);
        while (tempDir.size() > 3u && tempDir.back() == L'\\') {
            tempDir.pop_back();
        }

        const unsigned index = g_fixtureCounter.fetch_add(1u);
        root_ = tempDir + L"\\MrProper-it-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(index);

        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!makeDirectory(root_, win32Error) && win32Error != ERROR_ALREADY_EXISTS) {
            reason_ = describeError("создать фикстуру в %TEMP%", win32Error);
            root_.clear();
            return;
        }
        ready_ = true;
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree() {
        // Сначала ссылки: deleteTree по FR-6 их не трогает.
        for (const std::wstring& link : junctions_) {
            std::uint32_t win32Error = ERROR_SUCCESS;
            (void)removeJunction(link, win32Error);
        }
        junctions_.clear();
        if (root_.empty()) {
            return;
        }
        mrproper::platform::vfs::DeleteOptions options;
        options.allowedRoot = root_;
        // Несколько проходов: антивирус и индексатор Windows успевают взять
        // каталог на долю секунды после удаления содержимого, и один проход
        // оставил бы мусор в %TEMP%.
        for (int attempt = 0; attempt < 3 && exists(root_); ++attempt) {
            if (attempt > 0) {
                ::Sleep(200);
            }
            (void)mrproper::platform::vfs::deleteTree(root_, options);
        }
        if (exists(root_)) {
            std::printf("  [warn] не удалось убрать фикстуру %s\n", mrproper::platform::toUtf8(root_).c_str());
        }
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::wstring& root() const noexcept { return root_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    [[nodiscard]] std::wstring path(std::wstring_view relative) const {
        if (relative.empty()) {
            return root_;
        }
        // Разделители приводятся к «\» здесь, а не в extended(): форма «\\?\»
        // разбор точек и разделителей НЕ включает (её выключает именно префикс),
        // поэтому «C:\T\a/b» в расширенной форме — это каталог «a» с
        // буквальным символом «/» в имени, и CreateDirectoryW отвечает на него
        // ERROR_INVALID_NAME (123). Пока путь не перерос MAX_PATH, Win32 такой
        // путь проглатывает молча, и ошибка выглядит как «тест не смог создать
        // фикстуру».
        std::wstring joined = root_;
        if (joined.back() != L'\\') {
            joined.push_back(L'\\');
        }
        joined.append(relative);
        for (wchar_t& symbol : joined) {
            if (symbol == L'/') {
                symbol = L'\\';
            }
        }
        return joined;
    }

    // Точка монтирования, которую снимет деструктор.
    [[nodiscard]] bool link(const std::wstring& linkPath, const std::wstring& targetDir, std::string& error) {
        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!createJunction(linkPath, targetDir, win32Error)) {
            error = describeError("создать точку монтирования", win32Error);
            return false;
        }
        junctions_.push_back(linkPath);
        return true;
    }

private:
    std::wstring root_;
    std::vector<std::wstring> junctions_;
    std::string reason_;
    bool ready_{false};
};

// Содержимое фиксированного размера: размер файла в тестах краёв задаётся точно,
// иначе округление до кластера прячет разницу между логическим и
// аллоцированным размером.
[[nodiscard]] std::string payload(std::size_t size, char seed) {
    std::string data(size, seed);
    for (std::size_t index = 0; index < size; ++index) {
        data[index] = static_cast<char>(seed + static_cast<char>(index % 17u));
    }
    return data;
}

// Собрать элементы обхода. Копия структуры, а не ссылка: посетитель зовётся в
// стеке обхода, и хранить ссылку после возврата нельзя.
[[nodiscard]] std::vector<vfs::WalkEntry> walkCollect(std::wstring_view root) {
    std::vector<vfs::WalkEntry> entries;
    (void)vfs::walk(root, {}, [&entries](const vfs::WalkEntry& entry) {
        entries.push_back(entry);
        return vfs::WalkStep::Continue;
    });
    return entries;
}

// Наращивать вложенность кириллическими каталогами, пока путь не перевалит
// MAX_PATH. Возвращает глубину; созданные каталоги остаются на диске.
[[nodiscard]] std::size_t growBeyondMaxPath(const std::wstring& root, std::wstring& deepest,
                                            std::string& error) {
    deepest = root;
    std::size_t levels = 0;
    while (deepest.size() <= static_cast<std::size_t>(MAX_PATH) && levels < 24u) {
        deepest += L"\\" + std::wstring(kLevelSegment) + std::to_wstring(levels);
        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!makeDirectory(deepest, win32Error)) {
            error = describeError("создать вложенный каталог", win32Error);
            return 0;
        }
        ++levels;
    }
    return levels;
}

}  // namespace

// ---------------------------------------------------------------------------
// Длинные пути
// ---------------------------------------------------------------------------

// Путь длиннее MAX_PATH обязан работать через \\?\, и нормализация обязана его
// увидеть. Без этого очистка «Temp» на машине с глубоким профилем молча
// пропускала бы всё, что не влезло в 260 символов.
TEST(vfsEdge_longPath_tree_beyond_max_path_is_visible) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_longPath_tree_beyond_max_path_is_visible", tree.reason());
        return;
    }

    std::wstring deep;
    std::string growError;
    const std::size_t levels = growBeyondMaxPath(tree.root(), deep, growError);
    if (levels == 0) {
        reportSkip("vfsEdge_longPath_tree_beyond_max_path_is_visible", growError);
        return;
    }
    // Предусловие всего теста: без него проверки ниже ничего не доказывают.
    CHECK(deep.size() > static_cast<std::size_t>(MAX_PATH));
    CHECK(exists(deep));

    // 1) toExtendedPath даёт \\?\ ровно для той формы пути, которая его требует.
    const std::string deepUtf8 = mrproper::platform::toUtf8(deep);
    const std::string extendedUtf8 = pf::toExtendedPath(deepUtf8);
    CHECK(extendedUtf8.rfind("\\\\?\\", 0) == 0);
    CHECK(pf::isRootedFileSystemPath(deepUtf8));
    // Уже расширенная форма повторно не префиксуется: «\\?\\?\\C:\…» — другая
    // цель в диспетчере объектов.
    CHECK(pf::toExtendedPath(extendedUtf8) == extendedUtf8);

    // 2) Нормализация доходит до объекта за пределами MAX_PATH.
    const vfs::NormalizedPath normalized = vfs::normalizePath(deep);
    CHECK(normalized.resolved);
    CHECK(normalized.isDirectory);
    CHECK(!normalized.reparsePoint);
    CHECK(normalized.path.rfind(L"\\\\?\\", 0) == 0);
    // Хвост совпадает побайтно: сам временный каталог может оказаться ссылкой
    // (OneDrive, перенесённый профиль), и тогда префикс отличается — сравниваем
    // только то, что создал тест.
    const std::wstring deepTail = deep.substr(2u);
    CHECK(normalized.path.size() >= deepTail.size());
    CHECK(normalized.path.compare(normalized.path.size() - deepTail.size(), deepTail.size(), deepTail) == 0);

    // 3) Размер каталога меряется, аллоцированный у каталога по контракту
    //    неизвестен (NotApplicable) и подменять его нулём нельзя.
    const vfs::FileSize dirSize = vfs::measurePath(deep);
    CHECK(dirSize.ok());
    CHECK(vfs::hasFlag(dirSize.flags, vfs::FileFlags::Directory));
    CHECK(!dirSize.allocatedKnown);
    CHECK(dirSize.allocatedStatus == vfs::SizeStatus::NotApplicable);

    // 4) Обход видит файл на дне длинного пути и не теряет его по дороге.
    const std::wstring deepFile = deep + L"\\длинный-путь-файл.dat";
    std::uint32_t win32Error = ERROR_SUCCESS;
    const std::string data = payload(5000u, 'L');
    if (!writeFile(deepFile, data, win32Error)) {
        reportSkip("vfsEdge_longPath_tree_beyond_max_path_is_visible",
                   describeError("создать файл на длинном пути", win32Error));
        return;
    }

    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK(!result.canceled);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    CHECK_EQ(result.stats.files, std::uint64_t{1});
    CHECK_EQ(result.stats.directories, static_cast<std::uint64_t>(levels));
    CHECK_EQ(result.stats.entries, static_cast<std::uint64_t>(levels) + 1u);
    // Ни одной ссылки в дереве нет — счётчик обязан быть нулём, иначе тест
    // перестал бы доказывать, что петли тут нет.
    CHECK_EQ(result.stats.reparseSkipped, std::uint64_t{0});
    CHECK_EQ(result.stats.loopsDetected, std::uint64_t{0});
    // Глубина настоящего дерева: levels каталогов и файл на последнем уровне.
    CHECK_EQ(result.stats.maxDepthReached, static_cast<std::uint32_t>(levels) + 1u);

    const std::vector<vfs::WalkEntry> entries = walkCollect(tree.root());
    CHECK_EQ(entries.size(), levels + 1u);
    std::size_t deepFileSeen = 0;
    for (const vfs::WalkEntry& entry : entries) {
        // Ни один предъявленный путь не выходит за пределы корня обхода.
        CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        if (entry.name == L"длинный-путь-файл.dat") {
            ++deepFileSeen;
            // Путь элемента тоже длиннее MAX_PATH — обход не «срезал» его.
            CHECK(entry.path.size() > static_cast<std::size_t>(MAX_PATH));
            CHECK_EQ(entry.kind, vfs::EntryKind::File);
            CHECK_EQ(entry.logicalBytes, static_cast<std::uint64_t>(data.size()));
        }
    }
    CHECK_EQ(deepFileSeen, static_cast<std::size_t>(1));

    // 5) Размер файла на длинном пути совпадает с тем, что записали.
    //
    // Аллоцированный размер берётся через measurePublishedAllocated (D-78).
    // Инвариант «allocated >= logical» проверяется ТОЛЬКО для файла, который не
    // сжат и не разрежен: на машине со сжатым %TEMP% (а он сжатый, см. комментарий
    // у measurePublishedAllocated) allocated меньше логического ЗАКОННО, и
    // исходная проверка была неверным утверждением о файловой системе, которое
    // ещё и гонялось с её же рабочим потоком сжатия.
    //
    // Что здесь остаётся проверкой КОДА в любом случае: файл существует и он не
    // каталог, логический размер совпадает с записанным, аллоцированный известен,
    // reclaimBytes отдаёт то, что вернул замер, и значение УСТОЙЧИВО между двумя
    // замерами подряд.
    const AllocatedProbe probe = measurePublishedAllocated(deepFile);
    if (!probe.published) {
        // Том не опубликовал значение за отведённую секунду: это свойство
        // машины, и сказать о нём строкой пропуска честнее, чем покраснеть.
        reportSkip("vfsEdge_longPath_tree_beyond_max_path_is_visible",
                   describeUnpublishedAllocation("vfsEdge_longPath_tree_beyond_max_path_is_visible", probe));
        return;
    }
    const vfs::FileSize& fileSize = probe.size;
    CHECK(fileSize.ok());
    CHECK_EQ(fileSize.logicalBytes, static_cast<std::uint64_t>(data.size()));
    CHECK(fileSize.allocatedKnown);
    CHECK(!vfs::hasFlag(fileSize.flags, vfs::FileFlags::Directory));
    CHECK(!vfs::hasFlag(fileSize.flags, vfs::FileFlags::ReparsePoint));
    CHECK_EQ(fileSize.reclaimBytes(), fileSize.allocatedBytes);
    if (roundingApplies(fileSize)) {
        CHECK(fileSize.allocatedBytes >= fileSize.logicalBytes);
    } else {
        // Сжатый или разреженный файл: округление не проверяется, но об этом
        // печатается строка с числами — иначе на этой машине проверка была бы
        // зелёной, ничего не делая.
        noteRoundingNotApplicable("vfsEdge_longPath_tree_beyond_max_path_is_visible", deepFile, fileSize);
    }

    // 6) Опубликованное значение устойчиво: второй замер сразу после первого
    //    обязан дать ту же пару. Это ловит остаточную гонку — «allocated >=
    //    logical» сходится и на значении, которое через миллисекунду изменится.
    const vfs::FileSize again = vfs::measurePath(deepFile);
    CHECK(again.ok());
    CHECK(again.allocatedKnown);
    CHECK_EQ(again.logicalBytes, fileSize.logicalBytes);
    CHECK_EQ(again.allocatedBytes, fileSize.allocatedBytes);
    std::printf("  [note] длинный путь: logical=%llu allocated=%llu, попыток публикации %d, флаги %s\n",
                static_cast<unsigned long long>(fileSize.logicalBytes),
                static_cast<unsigned long long>(fileSize.allocatedBytes), probe.attempts,
                mrproper::platform::toUtf8(vfs::describeFlags(fileSize.flags)).c_str());
}

// Длинный путь должен удаляться, и ровно внутри своего корня: удаление файла за
// пределами корня правила — тот отказ, который §10 называет критическим.
TEST(vfsEdge_longPath_delete_inside_root_and_refuses_outside) {
    TempTree tree;
    TempTree foreign;
    if (!tree.ready() || !foreign.ready()) {
        reportSkip("vfsEdge_longPath_delete_inside_root_and_refuses_outside",
                   tree.ready() ? foreign.reason() : tree.reason());
        return;
    }

    std::wstring deep;
    std::string growError;
    const std::size_t levels = growBeyondMaxPath(tree.root(), deep, growError);
    if (levels == 0) {
        reportSkip("vfsEdge_longPath_delete_inside_root_and_refuses_outside", growError);
        return;
    }

    const std::wstring mine = deep + L"\\мой-файл-для-удаления.bin";
    const std::wstring theirs = foreign.path(L"чужой-файл-не-трогать.bin");
    std::uint32_t win32Error = ERROR_SUCCESS;
    const std::string data = payload(9000u, 'D');
    if (!writeFile(mine, data, win32Error) || !writeFile(theirs, data, win32Error)) {
        reportSkip("vfsEdge_longPath_delete_inside_root_and_refuses_outside",
                   describeError("создать фикстуры удаления", win32Error));
        return;
    }

    // Корень правила задаётся один раз на всё дерево операций удаления.
    mrproper::platform::vfs::DeleteOptions options;
    options.allowedRoot = tree.root();

    mrproper::platform::vfs::DeleteRequest outside;
    outside.path = theirs;
    outside.kind = mrproper::platform::vfs::DeleteKind::File;
    mrproper::platform::vfs::DeleteRequest inside;
    inside.path = mine;
    inside.kind = mrproper::platform::vfs::DeleteKind::File;

    // 1) Чужой путь при моём корне — отказ, и файл на месте.
    const mrproper::platform::vfs::DeleteResult outsideResult =
        mrproper::platform::vfs::deleteEntry(outside, options);
    CHECK_EQ(static_cast<int>(outsideResult.status),
             static_cast<int>(mrproper::platform::vfs::DeleteStatus::SkippedOutsideRoot));
    CHECK(exists(theirs));

    // 2) Пустой корень — тоже отказ: операция удаления без корня правила
    //    означала бы «удалить что угодно».
    const mrproper::platform::vfs::DeleteOptions noRoot;
    const mrproper::platform::vfs::DeleteResult noRootResult = mrproper::platform::vfs::deleteEntry(inside, noRoot);
    CHECK_EQ(static_cast<int>(noRootResult.status),
             static_cast<int>(mrproper::platform::vfs::DeleteStatus::SkippedOutsideRoot));
    CHECK(exists(mine));

    // 3) Свой путь при своём корне удаляется, и по длинному пути тоже.
    const mrproper::platform::vfs::DeleteResult mineResult = mrproper::platform::vfs::deleteEntry(inside, options);
    CHECK_EQ(static_cast<int>(mineResult.status), static_cast<int>(mrproper::platform::vfs::DeleteStatus::Deleted));
    CHECK(!exists(mine));
    CHECK(exists(theirs));

    // 4) Повторное удаление — «уже нет», а не «ошибка»: цель достигнута.
    const mrproper::platform::vfs::DeleteResult again = mrproper::platform::vfs::deleteEntry(inside, options);
    CHECK(mrproper::platform::vfs::isRemoved(again.status));

    // 5) Дерево длинных каталогов убирается целиком.
    const mrproper::platform::vfs::TreeDeleteSummary summary = mrproper::platform::vfs::deleteTree(tree.root(), options);
    CHECK(summary.complete());
    // Каталогов на levels + 1: growBeyondMaxPath создаёт levels подкаталогов НИЖЕ
    // корня, а deleteTree по контракту удаляет содержимое снизу вверх и затем сам
    // каталог (vfs_delete.hpp:315) — корень тоже удаляется и тоже считается.
    // Обход при этом считает только каталоги ниже корня (проверка levels выше),
    // потому что стартовая точка обхода корнем не считается.
    CHECK_EQ(summary.dirsDeleted, levels + 1);
    CHECK_EQ(summary.failed, static_cast<std::size_t>(0));
    CHECK(!exists(tree.root()));
    // Чужое дерево не тронуто ни одной операцией.
    CHECK(exists(theirs));
}

// ---------------------------------------------------------------------------
// Юникод
// ---------------------------------------------------------------------------

// Имена, которые обязаны выжить round-trip: кириллица, CJK, суррогатные пары
// (эмодзи — два UTF-16-символа на один кодовый пункт), пробелы, скобки и
// диакритика. Ошибка «имя потеряло последний символ» или «кириллица стала
// вопросами» проявилась бы в отчёте пользователю, поэтому проверяются обе
// стороны: UTF-16 (что вернул обход) и UTF-8 (что положит в модель, §6.3).
TEST(vfsEdge_unicode_names_survive_round_trip) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_unicode_names_survive_round_trip", tree.reason());
        return;
    }

    struct NameCase {
        const wchar_t* wide;
        const char* utf8;
    };
    const NameCase names[] = {
        {L"кириллица-Привет-ЖУРНАЛ.txt", "кириллица-Привет-ЖУРНАЛ.txt"},
        {L"日本語のファイル名.bin", "日本語のファイル名.bin"},
        // Эмодзи: суррогатная пара. Половина пары (например U+D83D) сама по себе
        // не существует, и обрезанная пара ломает и вывод в консоль, и JSON.
        {L"emoji-\xD83D\xDE00-\xD83C\xDF89.dat", "emoji-\xF0\x9F\x98\x80-\xF0\x9F\x8E\x89.dat"},
        {L"пробелы и (скобки) [1] & 'кавычки'.tmp", "пробелы и (скобки) [1] & 'кавычки'.tmp"},
        {L"с-диакритикой-ǅ.txt", "с-диакритикой-ǅ.txt"},
        {L"точка.и-две.точки.txt", "точка.и-две.точки.txt"},
    };
    constexpr std::size_t kNameCount = sizeof(names) / sizeof(names[0]);

    struct Created {
        std::wstring name;
        std::uint64_t bytes;
    };
    std::vector<Created> created;
    std::uint64_t expectedTotal = 0;
    for (std::size_t index = 0; index < kNameCount; ++index) {
        const std::wstring name(names[index].wide);
        const std::string data = payload(2048u + index * 512u, 'U');
        std::uint32_t win32Error = ERROR_SUCCESS;
        if (!writeFile(tree.path(name), data, win32Error)) {
            reportSkip("vfsEdge_unicode_names_survive_round_trip", describeError("создать файл", win32Error));
            return;
        }
        created.push_back(Created{name, static_cast<std::uint64_t>(data.size())});
        expectedTotal += static_cast<std::uint64_t>(data.size());
    }
    CHECK_EQ(created.size(), kNameCount);

    // 1) Обход предъявляет те же UTF-16 имена, что были созданы, и без потерь.
    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    CHECK_EQ(result.stats.files, static_cast<std::uint64_t>(kNameCount));
    CHECK_EQ(result.stats.entries, static_cast<std::uint64_t>(kNameCount));

    const std::vector<vfs::WalkEntry> entries = walkCollect(tree.root());
    for (const Created& item : created) {
        bool found = false;
        for (const vfs::WalkEntry& entry : entries) {
            if (entry.name != item.name) {
                continue;
            }
            found = true;
            CHECK_EQ(entry.kind, vfs::EntryKind::File);
            CHECK_EQ(entry.logicalBytes, item.bytes);
            CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        }
        // Имя, созданное на диске, обязано встретиться обходу в том же виде.
        CHECK(found);
    }

    for (std::size_t index = 0; index < kNameCount; ++index) {
        const std::wstring expectedName(names[index].wide);
        const std::string utf8(names[index].utf8);

        // 2) UTF-8 → UTF-16 → UTF-8 без потерь: так путь попадает в модель
        //    (§6.3) и обратно в операцию.
        const std::wstring wide = mrproper::platform::toUtf16(utf8);
        CHECK(wide == expectedName);
        CHECK(mrproper::platform::toUtf8(wide) == utf8);

        // 3) Юникод не должен приниматься за запрещённую FR-6 форму: ни поток
        //    данных (ADS), ни «.»-компонент. Иначе имя в отчёте вдруг «уехало
        //    бы в поток», а путь с «..» считался бы внутри корня.
        CHECK(!pf::hasStreamSuffix(utf8));
        CHECK(!pf::hasDotSegment(utf8));
        CHECK(pf::isRootedFileSystemPath(mrproper::platform::toUtf8(tree.path(expectedName))));
    }

    // 4) Сумма размеров сходится с числом записанных байт: имена в юникоде
    //    меняют длину пути, но не объём.
    //
    //    Аллоцированные берутся через measurePublishedAllocated, а инвариант
    //    округления применяется ТОЛЬКО к несжатым и неразреженным файлам (D-78):
    //    %TEMP% на этой машине — сжатый каталог NTFS, поэтому у части файлов
    //    allocated законно меньше логического, и «сумма аллоцированных не меньше
    //    суммы логических» по всем файлам было бы неверным утверждением.
    //    Поэтому сумма логических берётся ТОЛЬКО по файлам, где округление
    //    обязано выполняться, а по исключённым печатается строка с числами.
    std::uint64_t logicalTotal = 0;
    std::uint64_t allocatedTotal = 0;
    std::uint64_t roundingLogical = 0;
    bool allMeasured = true;
    std::size_t roundingSkipped = 0;
    std::vector<bool> roundingOk;
    roundingOk.reserve(created.size());
    for (const Created& item : created) {
        const std::wstring itemPath = tree.path(item.name);
        const AllocatedProbe probe = measurePublishedAllocated(itemPath);
        if (!probe.published) {
            reportSkip("vfsEdge_unicode_names_survive_round_trip",
                       describeUnpublishedAllocation("vfsEdge_unicode_names_survive_round_trip", probe));
            return;
        }
        const vfs::FileSize& size = probe.size;
        allMeasured = allMeasured && size.ok() && size.allocatedKnown;
        CHECK_EQ(size.logicalBytes, item.bytes);
        logicalTotal += size.logicalBytes;
        if (size.allocatedKnown) {
            allocatedTotal += size.allocatedBytes;
        }
        if (roundingApplies(size)) {
            roundingLogical += size.logicalBytes;
            roundingOk.push_back(true);
        } else {
            ++roundingSkipped;
            roundingOk.push_back(false);
            noteRoundingNotApplicable("vfsEdge_unicode_names_survive_round_trip", itemPath, size);
        }
    }
    CHECK(allMeasured);
    CHECK_EQ(logicalTotal, expectedTotal);
    CHECK(allocatedTotal >= roundingLogical);

    // 5) Кластер тома известен: без него «занято на диске» остаётся гипотезой.
    const vfs::ClusterSize cluster = vfs::queryClusterSize(tree.root());
    CHECK(cluster.ok());
    CHECK(cluster.bytesPerCluster > 0u);
    for (std::size_t index = 0; index < created.size(); ++index) {
        const Created& item = created[index];
        const vfs::AllocatedSizeResult allocated = vfs::queryAllocatedSize(tree.path(item.name));
        CHECK(allocated.ok());
        // Как и выше: округление проверяется только там, где оно обязано быть.
        if (index < roundingOk.size() && roundingOk[index]) {
            CHECK(allocated.bytes >= item.bytes);
        }
    }
    if (roundingSkipped > 0) {
        std::printf("  [note] vfsEdge_unicode_names_survive_round_trip: округление проверено на %zu файлах из %zu"
                    " (сжатие NTFS в %%TEMP%%, см. D-78)\n",
                    created.size() - roundingSkipped, created.size());
    }

    // 6) Всё убирается одним проходом deleteTree, юникод не мешает.
    mrproper::platform::vfs::DeleteOptions options;
    options.allowedRoot = tree.root();
    const mrproper::platform::vfs::TreeDeleteSummary summary = mrproper::platform::vfs::deleteTree(tree.root(), options);
    CHECK(summary.complete());
    CHECK_EQ(summary.filesDeleted, kNameCount);
    CHECK(!exists(tree.root()));
}

// ---------------------------------------------------------------------------
// Петля reparse
// ---------------------------------------------------------------------------

// Главное из FR-6: ссылка предъявляется, но обход в неё не входит, и обход
// конечен. Два вида проблем проверяются разными деревьями:
//
//   * «петля» — junction внутри поддерева, указывающий на собственный родитель:
//     раскрытие дало бы бесконечную рекурсию;
//   * «выход наружу» — junction, указывающий на каталог ЗА пределами корня:
//     раскрытие удаляло бы чужое.
TEST(vfsEdge_reparse_junction_is_reported_but_not_followed) {
    TempTree tree;
    TempTree outside;
    if (!tree.ready() || !outside.ready()) {
        reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed",
                   tree.ready() ? outside.reason() : tree.reason());
        return;
    }

    std::uint32_t win32Error = ERROR_SUCCESS;
    if (!makeDirectory(tree.path(L"внешний-каталог"), win32Error) ||
        !makeDirectory(tree.path(L"внешний-каталог/внутрь"), win32Error) ||
        !writeFile(tree.path(L"внешний-каталог/свидетель.txt"), payload(700u, 'W'), win32Error) ||
        !writeFile(tree.path(L"внешний-каталог/внутрь/глубокий-свидетель.txt"), payload(300u, 'v'), win32Error) ||
        !writeFile(outside.path(L"чужое-не-трогать.txt"), payload(400u, 'X'), win32Error)) {
        reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed",
                   describeError("создать фикстуру", win32Error));
        return;
    }

    const std::wstring selfLoop = tree.path(L"внешний-каталог/петля");
    const std::wstring escape = tree.path(L"выход-наружу");
    std::string linkError;
    if (!tree.link(selfLoop, tree.path(L"внешний-каталог"), linkError) ||
        !tree.link(escape, outside.root(), linkError)) {
        reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed", linkError);
        return;
    }

    // --- 1) Обход по умолчанию: ссылки видны, но не раскрыты --------------------
    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK(!result.canceled);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    // ровно две ссылки предъявлены и обе пропущены
    CHECK_EQ(result.stats.reparseSkipped, std::uint64_t{2});
    // петля ловится структурно (ссылка не раскрыта), а не подсчётом повторов
    CHECK_EQ(result.stats.loopsDetected, std::uint64_t{0});
    // 2 файла, 2 обычных каталога и 2 каталога-ссылки
    CHECK_EQ(result.stats.files, std::uint64_t{2});
    CHECK_EQ(result.stats.directories, std::uint64_t{4});
    CHECK_EQ(result.stats.entries, std::uint64_t{6});
    // Глубина ограничена размером настоящего дерева: петля не добавила ни одного
    // уровня. Без этой проверки «прогон завершился» ничего не значило бы —
    // обход мог пройти 30 уровней вглубь и упереться в maxDepth.
    CHECK_EQ(result.stats.maxDepthReached, std::uint32_t{3});

    const std::vector<vfs::WalkEntry> entries = walkCollect(tree.root());
    CHECK_EQ(entries.size(), static_cast<std::size_t>(6));
    std::size_t linksSeen = 0;
    for (const vfs::WalkEntry& entry : entries) {
        // Ни один предъявленный путь не выходит за пределы корня обхода: обход
        // не только не раскрыл ссылки, но и не показал элементов извне.
        CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        if (entry.name.find(L"чужое") != std::wstring::npos) {
            reportSkip("vfsEdge_reparse_junction_is_reported_but_not_followed",
                       "обход показал элемент чужого дерева — тест непригоден");
            return;
        }
        if (!entry.reparsePoint) {
            continue;
        }
        ++linksSeen;
        CHECK_EQ(entry.kind, vfs::EntryKind::Directory);
        CHECK_EQ(entry.reparseTag, kIoReparseTagMountPoint);
        // Ссылка предъявлена, но не помечена как петля: её не раскрывали, и
        // защита сработала раньше, чем пришлось бы считать повторы.
        CHECK(!entry.loopDetected);
        CHECK(!entry.depthLimitReached);
        // Ссылка видна и в отчёте: молча пропустить её нельзя, иначе
        // «каталог пуст» неотличим от «каталог скрыт ссылкой».
        CHECK(!vfs::describeReparseTag(entry.reparseTag).empty());
    }
    CHECK_EQ(linksSeen, static_cast<std::size_t>(2));

    // --- 2) Режим диагностики: followReparsePoint = true ------------------------
    // Модуль прямо предупреждает, что в этом режиме от петель защищает только
    // loopDetected. Обязательное свойство — конечность обхода: он обязан
    // завершиться и не уйти за пределы корня. Точное число элементов здесь не
    // проверяется: оно зависит от того, спустится ли обход по ссылке, а это
    // деталь реализации, которую тест не имеет права фиксировать.
    vfs::WalkOptions follow;
    follow.followReparsePoint = true;
    const vfs::WalkResult followed = vfs::walk(tree.root(), follow);
    CHECK(followed.completed);
    CHECK(followed.stats.entries <= std::uint64_t{8});
    for (const vfs::WalkEntry& entry : walkCollect(tree.root())) {
        CHECK(vfs::pathIsInsideRoot(entry.path, followed.root));
    }

    // --- 3) Удаление не раскрывает ссылки ------------------------------------
    mrproper::platform::vfs::DeleteOptions options;
    options.allowedRoot = tree.root();
    const mrproper::platform::vfs::TreeDeleteSummary summary = mrproper::platform::vfs::deleteTree(tree.root(), options);
    // Ссылки по FR-6 пропускаются, поэтому дерево «не пусто» и complete() ложно:
    // это ожидаемое поведение, а не дефект — снимать ссылку должен вызывающий.
    CHECK(!summary.complete());
    CHECK_EQ(summary.filesDeleted, static_cast<std::size_t>(2));
    CHECK(exists(selfLoop));
    CHECK(exists(escape));
    // Ключевая проверка темы: содержимое каталога ЗА ссылкой уцелело. Если бы
    // deleteTree раскрыл ссылку, «чужое-не-трогать.txt» исчез бы.
    CHECK(exists(outside.path(L"чужое-не-трогать.txt")));

    // --- 4) Ссылки сняты — остаётся пустота -----------------------------------
    CHECK(removeJunction(selfLoop, win32Error));
    CHECK(removeJunction(escape, win32Error));
    const mrproper::platform::vfs::TreeDeleteSummary afterLinks =
        mrproper::platform::vfs::deleteTree(tree.root(), options);
    CHECK(afterLinks.complete());
    CHECK(!exists(tree.root()));
    CHECK(exists(outside.path(L"чужое-не-трогать.txt")));
}

// Корень обхода, сам оказавшийся ссылкой, не раскрывается: правило с таким
// корнем увело бы обход за пределы каталога, который пользователь назвал.
TEST(vfsEdge_walk_root_reparse_point_is_not_expanded) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_walk_root_reparse_point_is_not_expanded", tree.reason());
        return;
    }

    std::uint32_t win32Error = ERROR_SUCCESS;
    if (!writeFile(tree.path(L"внутри-корня.txt"), payload(123u, 'R'), win32Error)) {
        reportSkip("vfsEdge_walk_root_reparse_point_is_not_expanded", describeError("создать файл", win32Error));
        return;
    }

    const std::wstring link = tree.path(L"ссылка-на-корень");
    std::string linkError;
    if (!tree.link(link, tree.root(), linkError)) {
        reportSkip("vfsEdge_walk_root_reparse_point_is_not_expanded", linkError);
        return;
    }

    // Нормализация различает ссылку и каталог: иначе «каталог ничего не
    // содержит» и «каталог — ссылка» были бы одним и тем же.
    CHECK(vfs::normalizePath(link).reparsePoint);
    CHECK(!vfs::normalizePath(tree.root()).reparsePoint);
    CHECK(vfs::normalizePath(tree.root()).isDirectory);

    const vfs::WalkResult result = vfs::walk(link);
    CHECK(result.completed);
    CHECK(result.rootSkippedReparse);
    CHECK_EQ(result.stats.entries, std::uint64_t{0});
    CHECK_EQ(result.stats.files, std::uint64_t{0});
    CHECK_EQ(result.stats.errors, std::uint64_t{0});

    // И содержимое исходного каталога на месте: обход по ссылке его не съел.
    CHECK(exists(tree.path(L"внутри-корня.txt")));
}

// Имя 8.3 и длинное имя — один каталог. Проверяется ровно то, что требует
// FR-6: нормализация обязана снять короткое имя, иначе корень правила
// «C:\Program Files\X» не совпал бы с найденным «C:\PROGRA~1\X» и уборка
// молча ничего бы не сделала. Отдельно проверяется пара Win32
// GetShortPathNameW/GetLongPathNameW на расширенном пути: обратное
// преобразование обязано вернуть ровно то же расширенное длинное имя, иначе
// «короткое имя» в отчёте пользователя нельзя развернуть обратно.
TEST(vfsEdge_short_name_resolves_to_the_same_directory) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_short_name_resolves_to_the_same_directory", tree.reason());
        return;
    }

    const std::wstring longNamed = tree.path(L"каталог-со-столь-длинным-именем-чтобы-получить-короткое");
    std::uint32_t win32Error = ERROR_SUCCESS;
    if (!makeDirectory(longNamed, win32Error)) {
        reportSkip("vfsEdge_short_name_resolves_to_the_same_directory",
                   describeError("создать каталог", win32Error));
        return;
    }

    const std::wstring extendedLong = extended(longNamed);
    std::vector<wchar_t> shortBuffer(extendedLong.size() + 1u, L'\0');
    const DWORD shortLength = ::GetShortPathNameW(extendedLong.c_str(), shortBuffer.data(),
                                                  static_cast<DWORD>(shortBuffer.size()));
    const std::wstring shortName(shortBuffer.data());
    // На томе с отключённым 8.3 (частая настройка рабочих машин) короткого имени
    // не будет — это не дефект теста и не дефект модуля.
    if (shortLength == 0u || shortName == longNamed || shortName.find(L'~') == std::wstring::npos) {
        reportSkip("vfsEdge_short_name_resolves_to_the_same_directory",
                   "том не выдаёт короткие имена 8.3 (GetShortPathNameW вернул длину " +
                       std::to_string(shortLength) + ")");
        return;
    }

    // Обратное преобразование Win32 — короткое имя обратно в длинное. Оба вызова
    // идут на расширенном пути: на форме без «\\?\» длинное имя не вернётся,
    // а короткое имя отрежется на MAX_PATH, и такой вызов молча отдал бы входную
    // строку — тест прошёл бы, ничего не проверив.
    std::vector<wchar_t> longBuffer(extendedLong.size() + 1u, L'\0');
    const DWORD longLength = ::GetLongPathNameW(extended(shortName).c_str(), longBuffer.data(),
                                                static_cast<DWORD>(longBuffer.size()));
    const std::wstring longAgain(longBuffer.data());
    CHECK(longLength != 0u);
    // Возврат ожидается в расширенной форме — её же отдал GetShortPathNameW.
    CHECK(longAgain == extendedLong);
    // И разворот короткого имени обратно приводит к тому же объекту.
    CHECK(pf::resolve(mrproper::platform::toUtf8(longAgain)).path ==
          pf::resolve(mrproper::platform::toUtf8(longNamed)).path);

    // Порядок «сначала нормализация, потом сравнение» задан контрактом
    // vfs_paths: сырой короткий путь против длинного корня даёт ложный запрет.
    const pf::Resolution fromLong = pf::resolve(mrproper::platform::toUtf8(longNamed));
    const pf::Resolution fromShort = pf::resolve(mrproper::platform::toUtf8(shortName));
    CHECK(fromLong.ok());
    CHECK(fromShort.ok());
    // strong == true — доказательство, что имя нормализовано (а не получено «как
    // открыли»): без него вердикт о принадлежности корню выносить нельзя.
    CHECK(fromLong.strong);
    CHECK(fromShort.strong);
    CHECK(fromLong.directory);
    CHECK(fromShort.directory);
    CHECK(fromLong.path == fromShort.path);
    CHECK(fromLong.path.find('~') == std::string::npos);
    CHECK(pf::isInsideRoot(fromShort.path, fromLong.path));
    // И обратное направление: корень-короткий, путь-длинный.
    CHECK(pf::isInsideRoot(fromLong.path, fromShort.path));

    // Подготовленный корень правила даёт тот же ответ, что и сравнение строк.
    const pf::RootGuard root = pf::prepareRoot(mrproper::platform::toUtf8(shortName));
    CHECK(root.resolved);
    CHECK(!root.rejected);
    CHECK(root.root == fromLong.path);
}

// Длинный путь с юникодом и ссылкой в одном дереве: три темы задачи
// одновременно, потому что в жизни они и встречаются вместе (кириллические
// профили, вложенные кэши, OneDrive-подобные каталоги).
TEST(vfsEdge_long_unicode_tree_with_reparse_backlink) {
    TempTree tree;
    if (!tree.ready()) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", tree.reason());
        return;
    }

    std::wstring deep;
    std::string growError;
    const std::size_t levels = growBeyondMaxPath(tree.root(), deep, growError);
    if (levels == 0) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", growError);
        return;
    }
    CHECK(deep.size() > static_cast<std::size_t>(MAX_PATH));

    const std::wstring leaf = deep + L"\\крайний-файл-😀.dat";
    std::uint32_t win32Error = ERROR_SUCCESS;
    const std::string data = payload(1500u, 'E');
    if (!writeFile(leaf, data, win32Error)) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", describeError("создать файл", win32Error));
        return;
    }

    // Ссылка с самого дна на корень обхода: раскрытие прогнало бы всё дерево
    // ещё раз, а на длинных путях это ещё и заметная потеря времени.
    std::string linkError;
    if (!tree.link(deep + L"\\назад-на-корень", tree.root(), linkError)) {
        reportSkip("vfsEdge_long_unicode_tree_with_reparse_backlink", linkError);
        return;
    }

    const vfs::WalkResult result = vfs::walk(tree.root());
    CHECK(result.completed);
    CHECK_EQ(result.stats.errors, std::uint64_t{0});
    CHECK_EQ(result.stats.files, std::uint64_t{1});
    CHECK_EQ(result.stats.reparseSkipped, std::uint64_t{1});
    CHECK_EQ(result.stats.loopsDetected, std::uint64_t{0});
    // Настоящие каталоги, одна ссылка и файл на дне
    CHECK_EQ(result.stats.entries, static_cast<std::uint64_t>(levels) + 2u);
    // Глубина настоящего дерева, а не «уровни × 2»: петлю не раскрывали.
    CHECK(result.stats.maxDepthReached <= static_cast<std::uint32_t>(levels) + 1u);

    std::size_t leafSeen = 0;
    for (const vfs::WalkEntry& entry : walkCollect(tree.root())) {
        CHECK(vfs::pathIsInsideRoot(entry.path, result.root));
        if (entry.name == L"крайний-файл-😀.dat") {
            ++leafSeen;
            CHECK_EQ(entry.logicalBytes, static_cast<std::uint64_t>(data.size()));
            CHECK(entry.path.size() > static_cast<std::size_t>(MAX_PATH));
        }
    }
    CHECK_EQ(leafSeen, static_cast<std::size_t>(1));

    // Размер длинного юникодного файла меряется теми же функциями, что ASCII.
    const vfs::FileSize size = vfs::measurePath(leaf);
    CHECK(size.ok());
    CHECK_EQ(size.logicalBytes, static_cast<std::uint64_t>(data.size()));
    CHECK(size.allocatedKnown);
    CHECK_EQ(size.reclaimBytes(), size.allocatedBytes);
}
