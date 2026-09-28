# ---------------------------------------------------------------------------
# Служебные функции сборки MrProper. Владелец: W01/задача 01.
# Модуль не содержит настроек сам по себе — только функции, которые вызывают
# другие модули (warnings, sanitizers, install).
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

# Проверка возможностей тулчейна. Не фатальна: отсутствие <format> или
# <ranges> не ломает сборку, оно лишь означает, что код должен обойтись без них.
# Но узнать об этом лучше на этапе configure, чем в середине волны: SPEC §8
# Этап 0.0 отдельно требует убедиться, что доступны std::stop_source/jthread,
# а на машине владельца стоит MSVC 14.29, и граница известна заранее.
function(mrproper_check_toolchain)
    if(NOT MRPROPER_CHECK_TOOLCHAIN)
        return()
    endif()
    include(CheckCXXSourceCompiles)

    set(_features "")

    # Сниппеты записаны в скобочных аргументах CMake [[...]]: в обычных
    # кавычках обратные слэши и кавычки пришлось бы удваивать, и такой «тест»
    # проверяет экранирование, а не компилятор.
    #
    # §6.4: отмена скана и очистки через std::stop_token (волны W11-W12).
    check_cxx_source_compiles([[
        #include <stop_token>
        #include <thread>
        int main() {
            std::stop_source source;
            const std::stop_token token = source.get_token();
            std::jthread worker([token] { (void)token.stop_possible(); });
            source.request_stop();
            worker.join();
            return token.stop_requested() ? 0 : 1;
        }
    ]] MRPROPER_HAVE_STOP_TOKEN)
    list(APPEND _features "std::stop_token/jthread=${MRPROPER_HAVE_STOP_TOKEN}")

    # §5: локализация и отчёт печатают числа с разделителями и датами.
    check_cxx_source_compiles([[
        #include <format>
        #include <string>
        int main() { return std::format("{}", 42).empty() ? 1 : 0; }
    ]] MRPROPER_HAVE_FORMAT)
    list(APPEND _features "std::format=${MRPROPER_HAVE_FORMAT}")

    # §4 FR-6, §9: обход ФС, размеры, пути.
    check_cxx_source_compiles([[
        #include <filesystem>
        int main() {
            const std::filesystem::path path("MrProper/data");
            return std::filesystem::exists(path) ? 1 : 0;
        }
    ]] MRPROPER_HAVE_FILESYSTEM)
    list(APPEND _features "std::filesystem=${MRPROPER_HAVE_FILESYSTEM}")

    # Сортировка кандидатов и разбор JSON по диапазонам.
    check_cxx_source_compiles([[
        #include <algorithm>
        #include <ranges>
        #include <vector>
        int main() {
            std::vector<int> values{3, 1, 2};
            std::ranges::sort(values);
            return values.front() == 1 ? 0 : 1;
        }
    ]] MRPROPER_HAVE_RANGES)
    list(APPEND _features "std::ranges=${MRPROPER_HAVE_RANGES}")

    # check_cxx_source_compiles ставит 1 при успехе и пустую строку при неудаче,
    # поэтому надёжен только признак «равно 1».
    set(_missing "")
    foreach(_feature IN LISTS _features)
        if(NOT _feature MATCHES "=1$")
            list(APPEND _missing "${_feature}")
        endif()
    endforeach()

    string(REPLACE ";" ", " _features_text "${_features}")
    if(_missing)
        string(REPLACE ";" ", " _missing_text "${_missing}")
        message(WARNING
            "[MrProper] тулчейн ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
            "не поддерживает: ${_missing_text}. Сборка не прерывается, но код, которому это нужно, "
            "придётся обходить. SPEC §8 Этап 0.0 требует решать нехватку возможностей на старте, "
            "а не в середине волны")
    else()
        message(STATUS
            "[MrProper] тулчейн ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}: "
            "все нужные возможности C++20 есть (${_features_text})")
    endif()
endfunction()

# Печатает итоговую конфигурацию сборки одной таблицей: её читает разработчик
# в логе и CI-агент в выводе шага configure. Ничего не проверяет и ничего не
# меняет — только STATUS-сообщения.
function(mrproper_print_config_summary)
    if(TARGET mrproper_unit_tests)
        set(_tests "${MRPROPER_BUILD_TESTS} (mrproper_unit_tests собрана)")
    else()
        set(_tests "${MRPROPER_BUILD_TESTS} (mrproper_unit_tests нет)")
    endif()
    if(MRPROPER_WARNINGS_AS_ERRORS)
        set(_werror "да")
    else()
        set(_werror "нет")
    endif()
    set(_lines
        "проект:      ${PROJECT_NAME} ${PROJECT_VERSION}"
        "генератор:   ${CMAKE_GENERATOR}"
        "платформа:   ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}"
        "компилятор:  ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        "C++:         ${CMAKE_CXX_STANDARD} (обязательный: ${CMAKE_CXX_STANDARD_REQUIRED}, расширения: ${CMAKE_CXX_EXTENSIONS})"
        "тесты:       ${_tests}"
        "предупреждения как ошибки: ${_werror}, строгий набор: ${MRPROPER_STRICT_WARNINGS}"
        "санитайзеры: ${MRPROPER_SANITIZE}"
        "install:     префикс «${CMAKE_INSTALL_PREFIX}», тесты в составе: ${MRPROPER_INSTALL_TESTS}, CPack: ${MRPROPER_ENABLE_CPACK}"
    )
    message(STATUS "[MrProper] конфигурация сборки:")
    foreach(_line IN LISTS _lines)
        message(STATUS "[MrProper]   ${_line}")
    endforeach()
endfunction()

# Финальный проход по конфигурации. Выполняется через cmake_language(DEFER) в
# конце обработки верхнего уровня, то есть когда уже известны все цели:
# и ядра, и платформы, и тестов, и добавленные позже слои engine, cli, ui.
# Общие флаги к тому моменту уже разданы на уровне каталога, поэтому здесь
# остаётся то, что зависит от наличия цели: ресурсы приложения (манифест UAC и
# ресурс версии появляются в W01-05 и W14) и отчёт о конфигурации.
function(mrproper_finalize_configure)
    if(COMMAND mrproper_apply_app_resources)
        mrproper_apply_app_resources()
    endif()
    mrproper_print_config_summary()
endfunction()
