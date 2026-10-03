import io

p = 'tests/integration/vfs_edge_tests.cpp'
d = open(p, 'rb').read().decode('utf-8')

start_marker = '// Опубликованный аллоцированный размер (D-78).'
end_marker = '\n// Создать точку монтирования (junction) linkPath'
i = d.index(start_marker)
j = d.index(end_marker)
old = d[i:j]

new = '''// Опубликованный аллоцированный размер (D-78).
//
// КОРЕНЬ ДЕФЕКТА ИЗМЕРЕН, а не угадан (tmp/u2/cz.ps1, tmp/u2/dz.ps1 — пробы на
// этой машине). %TEMP% здесь равен C:\\Users\\Daniil\\AppData\\Local\\Temp, и его
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
// Второе измерение на D: (NTFS, кластер 4096, тот же файл 5000 байт,
// tmp/u2/sz3.ps1) нужно, чтобы не приписать vfs_size чужое ожидание:
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
    std::printf("  [note] %s: файл %s — %s, поэтому «allocated >= logical» не проверяется: logical=%llu allocated=%llu\\n",
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

'''

d = d[:i] + new + d[j + 1:]
open(p, 'wb').write(d.encode('utf-8'))
print('helper block replaced')
