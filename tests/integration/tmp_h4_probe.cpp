// ВРЕМЕННЫЙ доказательный зонд задачи H4. Удаляется вместе с выводом.
//
// Зеркалит тело CleanupExecutor::deleteAllowedList (src/engine/executor.cpp,
// строки 1155-1210) один в один и переключает единственную разницу: живой
// std::wstring под DeleteOptions::allowedRoot (строка 1044, deleteItem) против
// временного объекта, умершего в конце полного выражения (строка 1157).
// Одинаковый код платформы, одинаковый файл, одинаковая последовательность
// выделений — различается только время жизни корня.
#include "harness.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/vfs_delete.hpp"
#include "platform/win_error.hpp"

namespace vfs = mrproper::platform::vfs;

namespace {

std::wstring w(std::string_view utf8) {
    return mrproper::platform::toUtf16(utf8);
}

std::wstring makeSandbox(std::size_t n) {
    wchar_t buffer[MAX_PATH + 1] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
    std::wstring base(buffer);
    if (base.empty() || base.back() != L'\\') base.push_back(L'\\');
    return base + L"MrProper-probe-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(n);
}

void makeFile(const std::wstring& path) {
    HANDLE h = ::CreateFileW((L"\\\\?\\" + path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        static const char payload[64] = {};
        DWORD written = 0;
        ::WriteFile(h, payload, sizeof(payload), &written, nullptr);
        ::CloseHandle(h);
    }
}

const char* statusName(vfs::DeleteStatus status) {
    switch (status) {
        case vfs::DeleteStatus::Deleted:
            return "Deleted";
        case vfs::DeleteStatus::AlreadyGone:
            return "AlreadyGone";
        case vfs::DeleteStatus::SkippedBusy:
            return "SkippedBusy";
        case vfs::DeleteStatus::SkippedProtected:
            return "SkippedProtected";
        case vfs::DeleteStatus::SkippedOutsideRoot:
            return "SkippedOutsideRoot";
        case vfs::DeleteStatus::SkippedCancelled:
            return "SkippedCancelled";
        default:
            return "прочее";
    }
}

struct Mirror {
    std::uint32_t removed{};
    std::uint32_t skipped{};
    std::string firstDetail;
    std::int32_t firstHr{};
};

// Тело deleteAllowedList, дословно, с единственной правкой: корень либо живёт
// до конца вызова (deleteItem), либо снят копией с временного wstring
// (deleteAllowedList). Ничего больше не отличается.
Mirror runMirror(const std::vector<std::string>& allowedUtf8, vfs::DeleteOptions& deleteOptions) {
    Mirror mirror;
    std::vector<std::string> parents;
    for (std::size_t index = 0; index < allowedUtf8.size(); ++index) {
        const std::string& entry = allowedUtf8[index];
        vfs::DeleteRequest request;
        request.path = w(entry);
        request.kind = vfs::DeleteKind::File;
        const vfs::DeleteResult result = vfs::deleteEntry(request, deleteOptions, {});
        if (result.status == vfs::DeleteStatus::Deleted) {
            ++mirror.removed;
            parents.push_back(entry.substr(0, entry.find_last_of("\\/")));
        } else if (result.status != vfs::DeleteStatus::AlreadyGone) {
            ++mirror.skipped;
            if (mirror.firstHr == 0) mirror.firstHr = result.hr;
            if (mirror.firstDetail.empty()) mirror.firstDetail = mrproper::platform::toUtf8(result.detail);
        }
    }
    return mirror;
}

}  // namespace

TEST(probe_h4_deleteAllowedListLifetime) {
    const std::wstring root = makeSandbox(0);
    const std::wstring file = root + L"\\a-listed.txt";
    ::CreateDirectoryW((L"\\\\?\\" + root).c_str(), nullptr);
    makeFile(file);
    const std::string rootUtf8 = mrproper::platform::toUtf8(root);
    const std::vector<std::string> allowed = {mrproper::platform::toUtf8(file)};

    // (1) Живой корень — как в deleteItem, строки 1043-1048.
    {
        const std::wstring rootWide = w(rootUtf8);
        vfs::DeleteOptions options;
        options.allowedRoot = rootWide;
        const Mirror mirror = runMirror(allowed, options);
        std::printf("  [probe] (1) живой корень: удалено %u, пропущено %u, hr=%d «%s»\n", mirror.removed,
                    mirror.skipped, mirror.firstHr, mirror.firstDetail.c_str());
    }
    std::printf("  [probe] (1) файл на месте = %d (0 = удалён)\n",
                ::GetFileAttributesW((L"\\\\?\\" + file).c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0);

    // (2) Висячий корень — ровно строка 1157: deleteOptions.allowedRoot = wide(...)
    //     в правой части присваивания, временный wstring умер в этой же строке.
    //     Исполнитель работает в потоке пула, поэтому и проверка идёт в потоке:
    //     состояние кучи после освобождения временного объекта отличается от
    //     главного потока, и именно на этом строится воспроизведение.
    makeFile(file);
    {
        Mirror mirror;
        std::thread worker([&] {
            vfs::DeleteOptions options;
            options.allowedRoot = w(rootUtf8);
            mirror = runMirror(allowed, options);
        });
        worker.join();
        std::printf("  [probe] (2) висячий корень (поток пула): удалено %u, пропущено %u, hr=%d «%s»\n",
                    mirror.removed, mirror.skipped, mirror.firstHr, mirror.firstDetail.c_str());
    }
    std::printf("  [probe] (2) файл на месте = %d (1 = НЕ удалён)\n",
                ::GetFileAttributesW((L"\\\\?\\" + file).c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0);

    ::DeleteFileW((L"\\\\?\\" + file).c_str());
    ::RemoveDirectoryW((L"\\\\?\\" + root).c_str());
}
