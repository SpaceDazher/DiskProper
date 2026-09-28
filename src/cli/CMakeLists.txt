# ---------------------------------------------------------------------------
# Слой cli — программа mrproper-cli. Спека: §6.1 (слои), §6.2 (строка `cli`:
# «mrproper-cli scan --json, --plan, --apply — headless-режим для CI и e2e»),
# §8 Этап 0 («mrproper-cli — пустой, но собираемый») и §8 Этапы 1-4 (состав
# команд), §9.1 ADR-004.
#
# ВЛАДЕЛЕЦ ЭТОГО ФАЙЛА. Как и в src/ui/CMakeLists.txt: ни одна задача волн
# W13 в списке работ не закреплена за src/cli/CMakeLists.txt, а верхний уровень
# подключает подкаталог только при наличии собственного CMakeLists.txt. Без
# этого файла слой молча не попадает в сборку: задачи 61-65 компилируются, ничего
# не проверяя, а цели mrproper-cli нет в дереве вообще. Файл написан по образцу
# src/ui/CMakeLists.txt и правки других агентов не требует — каждый новый .cpp в
# src/cli подхватывается сам.
#
# Слой лежит НАД platform, core и engine: команды печатают то, что собрали
# platform::inventory и engine, и считают через core. Обратных зависимостей у
# cli нет: ни один файл этого каталога не должен включать <windows.h> вне
# .cpp-мостов к платформе (в src/cli таких мостов ровно один — cmd_disks.cpp с
# makePlatformDisksServices; остальные команды получают машину снаружи через
# структуры *Services, см. cmd_scan.hpp).
# ---------------------------------------------------------------------------

# Исходники появляются по одному .cpp на модуль (args, main, cmd_scan,
# cmd_disks, cmd_apply, cmd_rules, cmd_report), и каждый агент владеет только
# своим файлом — поэтому перечень берётся каталогом. CONFIGURE_DEPENDS
# заставляет CMake перечитывать каталог (шаг «Checking File Globs»), а
# tools\build.bat переконфигурирует проект при каждом запуске.
file(GLOB MRPROPER_CLI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# Точка входа. Объявлена отдельно от перечня каталога, потому что она решает,
# существует ли вообще программа: до задачи 61 (main.cpp) в каталоге лежат
# одни команды, и add_executable без точки входа упал бы на компоновке.
set(MRPROPER_CLI_ENTRY "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

if(NOT TARGET mrproper_cli)
    if(EXISTS "${MRPROPER_CLI_ENTRY}")
        add_executable(mrproper_cli ${MRPROPER_CLI_SOURCES})
    else()
        # Точки входа ещё нет. Молча объявлять цель нельзя — cmake при генерации
        # упал бы на отсутствии main, и падение выглядело бы дефектом чужой
        # команды. Сообщение прямо называет, что и кто должен появиться.
        message(STATUS
            "[MrProper] src/cli: нет main.cpp (задача 61), цель mrproper_cli не объявляется; "
            "модули слоя пока не компилируются")
    endif()
endif()

# Идентификатор цели в install-списке cmake/MrProperInstall.cmake — mrproper_cli,
# и именно под этим именем файл собирается и запускается как mrproper-cli.exe
# Отдельной цели «mrproper-cli» не нужно: список кандидатов install проверяет
# на существование, лишнего имени там нет.
#
# Идемпотентность — как в src/core, src/platform, src/engine и src/ui: если цель
# уже объявлена (страховка верхнего уровня или повторное подключение каталога),
# add_executable упал бы с «target already exists».
if(TARGET mrproper_cli)
    # Корень src — тот же контракт, что у ядра, платформы и движка. Верхний
    # уровень задаёт MRPROPER_SRC_DIR сам; запасной путь — случай, когда
    # каталог подключает сторонний проект.
    if(NOT DEFINED MRPROPER_SRC_DIR)
        get_filename_component(MRPROPER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    endif()

    # PUBLIC ради включений внутри слоя: короткая форма #include "cmd_disks.hpp"
    # и полная #include "core/model.hpp" из файлов каталога.
    target_include_directories(mrproper_cli PUBLIC
        "${MRPROPER_SRC_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(mrproper_cli PUBLIC cxx_std_20)

    # Зависимости — только вниз (SPEC §6.2: `cli` зависит от `engine`,
    # `platform` и `core`). mrproper_platform тянет за собой системные библиотеки
    # Windows (см. свой список в src/platform/CMakeLists.txt), поэтому отдельно
    # они здесь не перечисляются: список, который надо обновлять руками вместе
    # с платформой, — это список, который однажды разъедется с ней.
    target_link_libraries(mrproper_cli PUBLIC
        mrproper_core
        mrproper_platform
        mrproper_engine
    )

    # Конфигурация сборки в макросе: --version должен отличать Debug от Release,
    # иначе баг-репорт из CI не отличить от прогона разработчика (args.cpp читает
    # MRPROPER_BUILD_TYPE). При генераторе Visual Studio CMAKE_BUILD_TYPE пуст, а
    # конфигурацию выбирает --config, поэтому значение берётся генераторным
    # выражением.
    target_compile_definitions(mrproper_cli PRIVATE
        MRPROPER_BUILD_TYPE="$<CONFIG>"
    )

    # Выход в корень каталога сборки: проверка ищет mrproper-cli.exe по
    # фиксированному пути build\<cfg>\ (то же, что сделано для
    # mrproper_unit_tests в tests/unit/CMakeLists.txt). Без этого файл оказался
    # бы в build\<cfg>\src\cli\<cfg>\ и команда проверки его не нашла бы.
    set_target_properties(mrproper_cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )

    # Планка предупреждений — та же, что у ядра (SPEC §9.1, ADR-001). Флаги
    # придут из cmake/MrProperWarnings.cmake на уровне каталога, а он
    # объявляет MRPROPER_WARNINGS_READY; профиль из cmake/warnings.cmake —
    # следующий источник, ручной набор — последний запасной путь.
    if(NOT MRPROPER_WARNINGS_READY)
        if(COMMAND mrproper_enable_warnings)
            mrproper_enable_warnings(mrproper_cli WIN32)
        elseif(MSVC)
            target_compile_options(mrproper_cli PRIVATE /W4 /WX /permissive- /utf-8)
        else()
            target_compile_options(mrproper_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()

    list(LENGTH MRPROPER_CLI_SOURCES _mrproper_cli_count)
    message(STATUS
        "[MrProper] цель mrproper_cli: ${_mrproper_cli_count} модулей "
        "(слой CLI, зависит от mrproper_core, mrproper_platform и mrproper_engine)")
endif()

# ---------------------------------------------------------------------------
# Проверочный прогон команды disks
# ---------------------------------------------------------------------------
#
# Команда disks подключается к каркасу в args.cpp (задача 61: реестр команд и
# dispatchCommand). Пока эта строка в чужом файле не появилась, единственный
# способ прогнать disks из слоя cli — мимо каркаса. Чтобы проверка задачи не
# зависела от чужого файла и не требовала править его, здесь есть необязательная
# цель: она собирается ТОЛЬКО когда в окружении задан MRPROPER_CLI_SELFTEST, то
# её нет в обычной сборке ни у кого.
#
#   set MRPROPER_CLI_SELFTEST=1 && tools\build.bat Debug a62
#   build\a62\Debug\mrproper_disks_selftest.exe disks --json
#
# Точка входа пишется в каталог сборки, а не в src/cli: в дереве исходников
# появляться нечему — файл нужен только на время прогона, и держать его в
# репозитории незачем. В установку цель не идёт (см. список кандидатов в
# cmake/MrProperInstall.cmake — там только имена целей верхнего уровня).
if(TARGET mrproper_cli AND DEFINED ENV{MRPROPER_CLI_SELFTEST})
    set(_mrproper_selftest_dir "${CMAKE_CURRENT_BINARY_DIR}/selftest")
    set(_mrproper_selftest_main "${_mrproper_selftest_dir}/disks_selftest_main.cpp")
    file(WRITE "${_mrproper_selftest_main}" [==[
// Точка входа проверочного прогона. Пишется файлом src/cli/CMakeLists.txt при
// включённом MRPROPER_CLI_SELFTEST, в исходниках репозитория её нет.
//
// Два режима:
//   disks [опции]           — настоящий прогон: обход инвентаризации и печать
//                              дампа, ровно как это делает каркас;
//   --selftest-fixture      — печать дампа по СИНТЕТИЧЕСКОЙ инвентаризации.
//                              Нужен там, где \\.\PhysicalDriveN не открывается
//                              (нет прав админа, виртуальный контроллер): на
//                              такой машине настоящий обход честно вернёт
//                              «нет данных», и формат дампа проверить было бы
//                              нечем. Фикстура — только проверка формы, не
//                              результат измерения.
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "cmd_disks.hpp"

namespace {

mrproper::core::PhysicalDisk makeFixtureDisk() {
    using mrproper::core::BusType;
    using mrproper::core::PartitionKind;
    using mrproper::core::PhysicalDisk;

    PhysicalDisk disk;
    disk.number = 0;
    disk.devicePath = "\\\\?\\X#&selftest&0";
    disk.model = "MrProper selftest SSD";
    disk.serial = "SELFTEST0123456789";
    disk.firmware = "FW1";
    disk.bus = BusType::Nvme;
    disk.sizeBytes = 1024203640320ull;
    disk.trimSupported = true;
    disk.smartAvailable = true;

    PartitionKind kinds[3] = {PartitionKind::Efi, PartitionKind::Msr, PartitionKind::BasicData};
    const std::uint64_t lengths[3] = {104857600ull, 16777216ull, 1000000000000ull};
    const std::uint64_t offsets[3] = {1048576ull, 209715200ull, 230686720ull};
    // GUID системного раздела EFI (c12a7328-f81f-11d2-ba4b-00a0c93ec93b) в том
    // порядке байт, в каком его пишет дискioctls: перестановку делает владелец
    // данных, и дамп печатает ровно то, что пришло.
    const std::uint8_t efiType[16] = {0xc1, 0x2a, 0x73, 0x28, 0xf8, 0x1f, 0x11, 0xd2,
                                      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
    for (std::size_t index = 0; index < 3; ++index) {
        mrproper::core::Partition part;
        part.index = static_cast<std::uint32_t>(index);
        part.offsetBytes = offsets[index];
        part.lengthBytes = lengths[index];
        part.kind = kinds[index];
        part.gptName = index == 2 ? "Windows" : "";
        part.boot = index == 0;
        if (index == 0) {
            part.hasGptType = true;
            for (std::size_t byte = 0; byte < 16; ++byte) part.gptType[byte] = efiType[byte];
        }
        if (index == 2) {
            part.hasVolume = true;
            part.volume.volumeGuidPath = "\\\\?\\Volume{11111111-2222-3333-4444-555555555555}";
            part.volume.label = "Windows";
            part.volume.fileSystem = "NTFS";
            part.volume.totalBytes = 1000000000000ull;
            part.volume.freeBytes = 250000000000ull;
            part.volume.encrypted = true;
            part.volume.mountPoints.push_back("C:\\");
            part.volume.diskExtents.push_back(std::make_pair(0, offsets[index]));
        }
        disk.partitions.push_back(part);
    }
    return disk;
}

int runFixture() {
    mrproper::cli::DisksOptions options;
    std::string error;
    if (!mrproper::cli::parseDisksOptions({"--show-serials"}, options, error)) {
        std::cerr << "[selftest] разбор опций не удался: " << error << "\n";
        return 2;
    }
    mrproper::cli::DisksInventory inventory;
    inventory.disks.push_back(makeFixtureDisk());
    inventory.disksComplete = true;
    inventory.volumesComplete = true;
    inventory.durationMs = 42;
    inventory.collectedAtUnix = 1700000000;
    std::cout << mrproper::cli::formatDisksJson(inventory, options);
    std::cout << "\n--- текст ---\n";
    std::cout << mrproper::cli::formatDisksText(inventory, options);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0u);
    for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);

    if (args.size() == 1 && args.front() == "--selftest-fixture") return runFixture();

    // Каркас (args.cpp) отдаёт команде хвост ПОСЛЕ имени команды, поэтому первое
    // слово здесь отбрасывается — так же, как это делает parseCommandLine.
    if (!args.empty() && args.front() == "disks") args.erase(args.begin());

    mrproper::cli::DisksServices services = mrproper::cli::makePlatformDisksServices();
    services.appVersion = "selftest";
    return mrproper::cli::runDisks(args, services, std::cout, std::cerr);
}
]==])
    add_executable(mrproper_disks_selftest
        "${_mrproper_selftest_main}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmd_disks.cpp"
    )
    target_include_directories(mrproper_disks_selftest PRIVATE
        "${MRPROPER_SRC_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mrproper_disks_selftest PRIVATE
        mrproper_core
        mrproper_platform
    )
    set_target_properties(mrproper_disks_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
    message(STATUS
        "[MrProper] цель mrproper_disks_selftest: прогон команды disks мимо каркаса "
        "(включена переменной MRPROPER_CLI_SELFTEST)")
endif()
