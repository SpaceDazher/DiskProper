// Граница «корень правила» в слое корзины FR-7.
//
// Задача 97, дефект ревью 01 F-01 (критический): обход дерева проверял признак
// reparse только в момент перечисления, а раскрывал каталог позже, и границы
// правила в модуле не было вовсе — «TrashOptions» её не содержал. Между
// перечислением и раскрытием проходит всё время обхода дерева, а этого времени
// хватает, чтобы подменить каталог на junction. Снос уходил за пределы корня
// правила, а отчёт писал «удалено успешно».
//
// Что здесь проверяется (свойства, а не реализация):
//
//   1. TrashOptions.allowedRootUtf8 — граница, и снос за её пределами не
//      начинается: элемент на месте, отказ с кодом и путём в result;
//   2. граница, которую не удалось открыть, — отказ, а не «границы нет»;
//   3. пустая граница (внутренние операции сервиса корзины: чистка корня
//      корзины, откат недокопированного) ведёт себя как раньше;
//   4. junction внутри корня не приводит к сносу того, что физически лежит за
//      корнем (инвариант класса, а не только проверка одной строки).
//
// Честность результата: тесты не требуют прав администратора (junction создаёт
// обычный пользователь), а предпосылки, которых может не быть на конкретной
// машине, печатаются как «[skip] <имя>: <причина>» и не выдают за успех.
//
// Спека: §4 FR-7 (корзина приложения и отмена), FR-6 («пропуск reparse points
// … — защита от петель и выхода за пределы пути», «проверка, что путь внутри
// ожидаемого корня правила»), §10 (критический риск «Повреждение системных
// данных при ошибке в правиле»), §12 («отсутствие изменений вне корней правил»).
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

#include "platform/vfs_trash.hpp"
#include "platform/win_error.hpp"
#include "platform/win_handle.hpp"

namespace {

namespace trash = mrproper::platform;

using ScopedHandle = mrproper::platform::unique_handle<mrproper::platform::KernelHandlePolicy>;

// Тег точки монтирования из winnt.h и смещение PathBuffer в REPARSE_DATA_BUFFER:
// DWORD тега + пять WORD. Значения зафиксированы архитектурой NTFS.
constexpr std::uint32_t kIoReparseTagMountPoint = 0xA0000003u;
constexpr std::size_t kReparseHeaderBytes = 16u;

// Порядковый номер фикстуры: два прогона тестов, случившиеся одновременно, не
// должны делить один каталог.
std::atomic<unsigned> g_fixtureCounter{0u};

void skip(const char* name, const std::string& why) {
    std::printf("  [skip] %s: %s\n", name, why.c_str());
}

// Расширенная форма пути создаётся здесь, а не через vfs_trash: фикстуру делает
// сам тест, иначе поломка нормализации в модуле была бы неотличима от «тест не
// смог создать фикстуру».
[[nodiscard]] std::wstring extended(std::wstring_view path) {
    static const std::wstring prefix = L"\\\\?\\";
    if (path.rfind(prefix, 0) == 0) return std::wstring(path);
    if (path.size() >= 2u && path[1] == L':') return prefix + std::wstring(path);
    return std::wstring(path);
}

[[nodiscard]] std::wstring tempRoot() {
    wchar_t buffer[MAX_PATH + 1] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length > MAX_PATH) return {};
    std::wstring base(buffer);
    if (base.empty() || base.back() != L'\\') base.push_back(L'\\');
    base += L"mrproper-trash-boundary-" + std::to_wstring(g_fixtureCounter.fetch_add(1u)) + L"-" +
            std::to_wstring(::GetCurrentProcessId());
    return base;
}

[[nodiscard]] bool makeDirectories(const std::wstring& path, std::string& why) {
    // Родитель создаётся раньше потомка: «C:\Temp\a\b» требует, чтобы
    // «C:\Temp\a» уже существовал. Путь режется в его исходном виде, без
    // разбора префикса «\\?\» обратно в сегменты, — иначе легко сбиться
    // на букве тома.
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 3u) {
        if (!makeDirectories(path.substr(0, slash), why)) return false;
    }
    if (::CreateDirectoryW(extended(path).c_str(), nullptr) == FALSE) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            why = "не удалось создать каталог " + trash::toUtf8(path) + ": " + trash::win32ErrorText(error) + " (" +
                  std::to_string(error) + ")";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool writeFile(const std::wstring& path, std::string_view content, std::string& why) {
    ScopedHandle file(::CreateFileW(extended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD error = ::GetLastError();
        why = "не удалось создать файл " + trash::toUtf8(path) + ": " + trash::win32ErrorText(error) + " (" +
              std::to_string(error) + ")";
        return false;
    }
    std::size_t written = 0;
    while (written < content.size()) {
        DWORD done = 0;
        if (::WriteFile(file.get(), content.data() + written, static_cast<DWORD>(content.size() - written), &done,
                        nullptr) == FALSE ||
            done == 0) {
            why = "не удалось записать файл фикстуры";
            return false;
        }
        written += done;
    }
    return true;
}

[[nodiscard]] bool exists(const std::wstring& path) {
    return ::GetFileAttributesW(extended(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

[[nodiscard]] std::uint32_t attributesOf(const std::wstring& path) {
    const DWORD value = ::GetFileAttributesW(extended(path).c_str());
    return value == INVALID_FILE_ATTRIBUTES ? 0u : value;
}

// Точка монтирования (junction) на каталог target. Прав создания не требует —
// в отличие от символьной ссылки, junction доступен обычному пользователю, и
// тест не превращается в вечно зелёный пропуск. Буфер reparse собирается
// байтами, а не структурой REPARSE_DATA_BUFFER: объявление этой структуры
// зависит от версии SDK, а формат буфера задан архитектурой NTFS.
[[nodiscard]] bool createJunction(const std::wstring& linkPath, const std::wstring& targetDir, std::string& why) {
    const std::wstring ext = extended(linkPath);
    if (::CreateDirectoryW(ext.c_str(), nullptr) == FALSE && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        why = "не удалось создать каталог для junction";
        return false;
    }

    // SubstituteName — NT-путь («\??\C:\…»), PrintName — то, что видит
    // пользователь. Хвостовой разделитель у цели запрещён: целью монтирования
    // иначе становится каталог, который нельзя открыть.
    std::wstring substitute = L"\\??\\" + targetDir;
    while (!substitute.empty() && substitute.back() == L'\\') substitute.pop_back();
    std::wstring printable = targetDir;
    while (printable.size() > 3u && printable.back() == L'\\') printable.pop_back();

    const auto substituteBytes = static_cast<std::size_t>(substitute.size() * sizeof(wchar_t));
    const auto printableBytes = static_cast<std::size_t>(printable.size() * sizeof(wchar_t));
    const std::size_t payloadBytes = substituteBytes + sizeof(wchar_t) + printableBytes + sizeof(wchar_t);
    std::vector<char> buffer(kReparseHeaderBytes + payloadBytes, 0);

    const auto put16 = [&buffer](std::size_t offset, WORD value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    };
    const DWORD tag = static_cast<DWORD>(kIoReparseTagMountPoint);
    std::memcpy(buffer.data(), &tag, sizeof(tag));                    // ReparseTag
    put16(4u, static_cast<WORD>(payloadBytes));                        // ReparseDataLength
    put16(6u, static_cast<WORD>(0));                                   // Reserved
    put16(8u, static_cast<WORD>(0));                                   // SubstituteNameOffset
    put16(10u, static_cast<WORD>(substituteBytes));                   // SubstituteNameLength
    put16(12u, static_cast<WORD>(substituteBytes + sizeof(wchar_t)));  // PrintNameOffset
    put16(14u, static_cast<WORD>(printableBytes));                    // PrintNameLength
    std::memcpy(buffer.data() + kReparseHeaderBytes, substitute.c_str(), substituteBytes + sizeof(wchar_t));
    std::memcpy(buffer.data() + kReparseHeaderBytes + substituteBytes + sizeof(wchar_t), printable.c_str(),
                printableBytes + sizeof(wchar_t));

    // FILE_FLAG_OPEN_REPARSE_POINT обязателен: без него CreateFileW идёт по ссылке.
    ScopedHandle link(::CreateFileW(ext.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!link) {
        why = "не удалось открыть каталог для junction";
        return false;
    }
    DWORD returned = 0;
    if (::DeviceIoControl(link.get(), FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                          nullptr, 0, &returned, nullptr) == FALSE) {
        const DWORD error = ::GetLastError();
        why = "точка монтирования не создана: " + trash::win32ErrorText(error) + " (" + std::to_string(error) + ")";
        return false;
    }
    return true;
}

void removeJunction(const std::wstring& linkPath) {
    if (attributesOf(linkPath) != 0u) {
        ::RemoveDirectoryW(extended(linkPath).c_str());
    }
}

// Фикстура: правило (корень), данные рядом с ним и «дальний» каталог, на который
// в тестах смотрит junction. Уборка идёт в правильном порядке: junction снимается
// раньше дерева, потому что обход по FR-6 точку перехода не раскрывает и не
// сносит.
class TrashFixture {
public:
    TrashFixture() {
        base_ = tempRoot();
        rule_ = base_ + L"\\rule";
        sub_ = rule_ + L"\\sub";
        outside_ = base_ + L"\\outside";
        payload_ = sub_ + L"\\payload.txt";
        outsideFile_ = outside_ + L"\\documents.txt";
        junction_ = sub_ + L"\\shortcut";
    }

    bool create(std::string& why) {
        if (base_.empty()) {
            why = "не найден %TEMP%";
            return false;
        }
        if (!makeDirectories(sub_, why)) return false;
        if (!makeDirectories(outside_, why)) return false;
        if (!writeFile(payload_, "inside the rule root", why)) return false;
        if (!writeFile(outsideFile_, "outside the rule root", why)) return false;
        return true;
    }

    // Содержимое правила сносится, «дальний» каталог остаётся нетронутым.
    void cleanup() {
        removeJunction(junction_);
        if (!base_.empty()) {
            removeTreeW(base_);
        }
    }

    [[nodiscard]] const std::wstring& base() const { return base_; }
    [[nodiscard]] const std::wstring& ruleRoot() const { return rule_; }
    [[nodiscard]] const std::wstring& outsideDir() const { return outside_; }
    [[nodiscard]] const std::wstring& junction() const { return junction_; }
    [[nodiscard]] const std::wstring& payload() const { return payload_; }
    [[nodiscard]] const std::wstring& outsideFile() const { return outsideFile_; }

private:
    static void removeTreeW(const std::wstring& path) {
        const std::wstring ext = extended(path);
        WIN32_FIND_DATAW data{};
        std::wstring pattern = ext + L"\\*";
        ScopedHandle find(::FindFirstFileW(pattern.c_str(), &data));
        if (find) {
            do {
                const std::wstring name(data.cFileName);
                if (name == L"." || name == L"..") continue;
                const std::wstring child = ext + L"\\" + name;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    removeTreeW(child);
                } else {
                    ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    ::DeleteFileW(child.c_str());
                }
            } while (::FindNextFileW(find.get(), &data) != FALSE);
        }
        ::RemoveDirectoryW(ext.c_str());
    }

    std::wstring base_;
    std::wstring rule_;
    std::wstring sub_;
    std::wstring outside_;
    std::wstring payload_;
    std::wstring outsideFile_;
    std::wstring junction_;
};

// Опции очистки корзины без журналирования: тест проверяет код возврата и
// состояние диска, а не текст лога.
[[nodiscard]] trash::TrashOptions quietOptions(std::wstring_view allowedRoot) {
    trash::TrashOptions options;
    options.logFailures = false;
    options.applyTimestamps = false;
    options.applyAcl = false;
    if (!allowedRoot.empty()) options.allowedRootUtf8 = trash::toUtf8(allowedRoot);
    return options;
}

TEST(trashBoundary_refusesPurgeOutsideRuleRoot) {
    TrashFixture fixture;
    std::string why;
    if (!fixture.create(why)) {
        skip("trash_boundary_refusesPurgeOutsideRuleRoot", why);
        return;
    }

    // Соседний с корнем каталог: физически он рядом, но в корень правила не
    // входит. Слой корзины обязан отказать, а не снести.
    const trash::TrashOptions options = quietOptions(fixture.ruleRoot());
    const trash::TrashPurgeResult result =
        trash::purgePath(trash::toUtf8(fixture.outsideDir()), options);

    CHECK(!result.ok());  // purgePath должен отказать для пути вне корня правила
    CHECK(result.status != trash::TrashStatus::Ok);  // статус должен быть не Ok
    CHECK(exists(fixture.outsideFile()));  // файл вне корня правила обязан остаться на месте
    CHECK(exists(fixture.payload()));  // содержимое корня правила не должно пострадать

    fixture.cleanup();
    CHECK(!exists(fixture.outsideFile()));  // уборка фикстуры не удалила файл
}

TEST(trashBoundary_allowsPurgeInsideRuleRoot) {
    TrashFixture fixture;
    std::string why;
    if (!fixture.create(why)) {
        skip("trash_boundary_allowsPurgeInsideRuleRoot", why);
        return;
    }

    // Контроль к предыдущему тесту: та же операция внутри границы обязана
    // сработать, иначе проверка на отказ ничего не значила бы.
    const trash::TrashOptions options = quietOptions(fixture.ruleRoot());
    const trash::TrashPurgeResult result = trash::purgePath(trash::toUtf8(fixture.ruleRoot()), options);

    if (!result.ok()) {
        std::printf("         [diag] status=%s win32=%u failed=%s files=%u dirs=%u skipped=%u\n",
                    trash::toString(result.status), result.win32Error, result.failedPath.c_str(),
                    result.removedFiles, result.removedDirs, result.skippedReparsePoints);
    }
    CHECK(result.ok());  // purgePath внутри корня правила обязан отработать
    CHECK(!exists(fixture.payload()));  // файл внутри корня правила обязан быть снесён
    CHECK(exists(fixture.outsideFile()));  // соседний каталог не должен пострадать

    fixture.cleanup();
}

TEST(trashBoundary_junctionInsideRootLeavesOutsideDataIntact) {
    TrashFixture fixture;
    std::string why;
    if (!fixture.create(why)) {
        skip("trash_boundary_junctionInsideRootLeavesOutsideDataIntact", why);
        return;
    }
    if (!createJunction(fixture.junction(), fixture.outsideDir(), why)) {
        // Том без поддержки точек монтирования (FAT, сетевой): пропуск виден в
        // выводе и не выдаётся за успех.
        skip("trash_boundary_junctionInsideRootLeavesOutsideDataIntact", why);
        fixture.cleanup();
        return;
    }

    // Снос всего корня правила, внутри которого лежит junction на каталог с
    // пользовательскими данными. Что бы ни случилось с обходом, «documents.txt»
    // обязан уцелеть: это и есть инвариант «изменений вне корней правил нет».
    const trash::TrashOptions options = quietOptions(fixture.ruleRoot());
    const trash::TrashPurgeResult result = trash::purgePath(trash::toUtf8(fixture.ruleRoot()), options);
    (void)result;

    CHECK(exists(fixture.outsideFile()));  // файл под junction не должен быть снесён

    fixture.cleanup();
    CHECK(!exists(fixture.outsideFile()));  // уборка фикстуры не удалила файл
}

TEST(trashBoundary_emptyRootKeepsServiceBehaviour) {
    TrashFixture fixture;
    std::string why;
    if (!fixture.create(why)) {
        skip("trash_boundary_emptyRootKeepsServiceBehaviour", why);
        return;
    }

    // Пустая граница — внутренние операции сервиса корзины (чистка корня
    // корзины, откат недокопированного): корень задавать нечем, и снос
    // остаётся прежним. Если эта проверка падает, граница начала ломать
    // обслуживание корзины.
    const trash::TrashOptions options = quietOptions({});
    const trash::TrashPurgeResult result = trash::purgePath(trash::toUtf8(fixture.ruleRoot()), options);

    CHECK(result.ok());  // снос без заданной границы обязан отработать
    CHECK(!exists(fixture.payload()));  // файл внутри снесённого каталога обязан исчезнуть

    fixture.cleanup();
}

TEST(trashBoundary_unresolvableRootFailsClosed) {
    TrashFixture fixture;
    std::string why;
    if (!fixture.create(why)) {
        skip("trash_boundary_unresolvableRootFailsClosed", why);
        return;
    }

    // Корень, которого нет, — это «граница недоступна», а не «границы нет».
    // Снос под невыясненной границей недопустим: иначе опечатка в корне или
    // недоступный каталог тихо снимают данные.
    const std::wstring missing = fixture.base() + L"\\no-such-rule-root";
    const trash::TrashOptions options = quietOptions(missing);
    const trash::TrashPurgeResult result =
        trash::purgePath(trash::toUtf8(fixture.ruleRoot()), options);

    CHECK(!result.ok());  // недоступный корень обязан дать отказ, а не тихий снос
    CHECK(exists(fixture.payload()));  // файл обязан остаться на месте

    fixture.cleanup();
}

}  // namespace
