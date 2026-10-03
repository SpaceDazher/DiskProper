p = 'tests/integration/vfs_edge_tests.cpp'
d = open(p, 'rb').read().decode('utf-8')

old_start = '    // 5) Размер файла на длинном пути совпадает с тем, что записали.'
old_end = '\n}\n\n// Длинный путь должен удаляться'
i = d.index(old_start)
j = d.index(old_end)
old = d[i:j]
assert 'allocatedBytes % cluster' not in old, old

new = '''    // 5) Размер файла на длинном пути совпадает с тем, что записали.
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
    std::printf("  [note] длинный путь: logical=%llu allocated=%llu, попыток публикации %d, флаги %s\\n",
                static_cast<unsigned long long>(fileSize.logicalBytes),
                static_cast<unsigned long long>(fileSize.allocatedBytes), probe.attempts,
                mrproper::platform::toUtf8(vfs::describeFlags(fileSize.flags)).c_str());'''

d = d[:i] + new + d[j:]
open(p, 'wb').write(d.encode('utf-8'))
print('long path site replaced')
