p = 'tests/integration/vfs_edge_tests.cpp'
d = open(p, 'rb').read().decode('utf-8')

old_start = '    // 4) Сумма размеров сходится с числом записанных байт: имена в юникоде'
old_end = '\n    // 6) Всё убирается одним проходом deleteTree, юникод не мешает.'
i = d.index(old_start)
j = d.index(old_end)
old = d[i:j]
assert 'CHECK(allocated.bytes >= item.bytes);' in old, old[-400:]

new = '''    // 4) Сумма размеров сходится с числом записанных байт: имена в юникоде
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
                    " (сжатие NTFS в %%TEMP%%, см. D-78)\\n",
                    created.size() - roundingSkipped, created.size());
    }
'''

d = d[:i] + new + d[j:]
open(p, 'wb').write(d.encode('utf-8'))
print('unicode site replaced')
