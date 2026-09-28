# Сборка MrProper: контракт для агентов и разработчиков

Владелец файлов: W01, задача 01. Спека: SPEC §8 Этап 0, §9.1 ADR-001, §11.

## Правило проверки

Сборка и тесты — **только** через обёртки над MSVC:

```bat
tools\build.bat Debug      && tools\test.bat Debug
tools\build.bat Release    && tools\test.bat Release
```

Прямой вызов `cl.exe`, `cmake` из WSL и `g++` для Windows-кода доказательством
не считается: такой результат не воспроизводится у другого человека. Ядро
(`src/core`) — единственный слой без Windows API, и он по SPEC §6.1 / ADR-004
собирается на любом хосте, но проверка проекта по-прежнему идёт через build.bat.

## Что делает верхний уровень

| Что | Где | Зачем |
|---|---|---|
| C++20 без расширений, экспорт `compile_commands.json` | `CMakeLists.txt` | нужен clang-tidy и IDE |
| `/utf-8` для всех целей | `CMakeLists.txt` | в исходниках кириллица; без флага MSVC даёт C4819, а с `/WX` это ошибка |
| предупреждения как ошибки | `cmake/MrProperWarnings.cmake` | SPEC §9.1 ADR-001 |
| санитайзеры | `cmake/MrProperSanitizers.cmake` | SPEC §11 |
| install-цели и CPack | `cmake/MrProperInstall.cmake` | установщик — следствие Этапа 0 |
| отчёт о конфигурации | `cmake/MrProperHelpers.cmake` | читается в логе и в CI |

Общие флаги раздаются **на уровне каталога** и наследуются подкаталогами, а
`MRPROPER_WARNINGS_READY` объявляет, что предупреждения уже настроены. Поэтому
новый слой (`src/engine`, `src/cli`, `src/ui`) получает ту же планку, не
правив собственный `CMakeLists.txt`, и не дублирует флаги. Подкаталог, у
которого есть собственный `CMakeLists.txt`, подключается сам, как только файл
появился: `src/*/CMakeLists.txt` и `tests/CMakeLists.txt` просматриваются
каталогом, жёсткий список означал бы, что модуль молча не попадёт в сборку.

Списки исходников тоже берутся каталогом (`CONFIGURE_DEPENDS`): каждый агент
владеет одним своим `.cpp`, и дописывать чужой список он не имеет права.

## Опции

| Опция | По умолчанию | Смысл |
|---|---|---|
| `MRPROPER_BUILD_TESTS` | `ON` | тесты и реестр CTest |
| `MRPROPER_CHECK_TOOLCHAIN` | `ON` | проверка возможностей C++20 на этапе configure |
| `MRPROPER_WARNINGS_AS_ERRORS` | `ON` | `/WX`, `-Werror` |
| `MRPROPER_STRICT_WARNINGS` | `OFF` | сверх `/W4`: `/w44242…/w44272`, `-Wshadow -Wconversion -Wold-style-cast` |
| `MRPROPER_SANITIZE` | `OFF` | `OFF` / `address` / `undefined` / `address,undefined` |
| `MRPROPER_INSTALL_TESTS` | `OFF` | ставить ли `mrproper_unit_tests` |
| `MRPROPER_ENABLE_CPACK` | `ON` | генераторы установщика NSIS и ZIP |
| `MRPROPER_INSTALL_PREFIX` | пусто | префикс установки; пусто = per-user `%LOCALAPPDATA%\Programs\MrProper` |
| `MRPROPER_APP_TARGET` | `mrproper_app` | цель, к которой подключаются манифест и ресурс версии |

`MRPROPER_SANITIZE` задаётся тремя способами: `-DMRPROPER_SANITIZE=...` в
командной строке, переменной окружения `MRPROPRO_SANITIZE` (нужно, чтобы
включить санитайзеры через `tools\build.bat`, который передаёт только
`-DCMAKE_BUILD_TYPE`) или в пресете. Пробелы, регистр и `+` вместо `,`
нормализуются; недопустимое значение останавливает конфигурацию, а не
молча отключает проверки.

## Пресеты

`CMakePresets.json` написан для CMake 3.20 (единственная версия, которая
есть в Visual Studio 2019 Build Tools на этой машине), поэтому использует
схему версии 2: без `installDir`, `workflowPresets` и `outputErrors`.

| Пресет | Каталог | Назначение |
|---|---|---|
| `dev` | `build/Debug` | **проверка Этапа 0 из SPEC §8**: `cmake --build --preset dev && ctest --preset dev` |
| `vs2022-dev`, `vs2022-asan-debug` | `build/vs2022-*` | целевой тулчейн спецификации (MSVC 2022, VS 2022 Build Tools) |
| `debug` | `build/Debug` | то же дерево, что у `tools\build.bat Debug` |
| `release` | `build/Release` | то же дерево, что у `tools\build.bat Release`; префикс установки per-user |
| `asan-debug` | `build/asan-debug` | Debug + AddressSanitizer, отдельное дерево, чтобы ASan не закрепился в кэше обычной сборки |
| `ninja-debug`, `ninja-release` | `build/ninja-*` | генератор Ninja: быстрая сборка и `compile_commands.json` для clang-tidy |
| `wsl-debug` / `wsl-release` | `build/wsl-*` | ядро на хосте с GCC/Clang, `address,undefined` |

```bat
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Пресеты `ninja-*`, `asan-*` и `vs2022-*` — вспомогательные: канонической
сборкой проекта остаётся `tools\build.bat`. Им нужно окружение MSVC
(`vcvars64.bat`, то есть Developer Command Prompt), тогда как `tools\build.bat`
находит тулчейн сам.

### Отклонения от буквы SPEC §8, и почему

1. **Сананитайзеры в debug.** Спека требует «sanitizers в debug». У пресета
   `dev` они выключены: на MSVC 19.29 (Visual Studio 2019 16.11) рантайм
   AddressSanitizer не инициализируется, и `ctest --preset dev` падал бы на
   пустом выводе программы с кодом `0xC0000142`. Проверяемый путь — пресет
   `vs2022-asan-debug` (MSVC 2022) и `wsl-debug` (GCC/Clang).
2. **Генератор.** Спека называет MSVC 2022. На машине VS 2022 стоит без C++
   workload, единственный компилятор — MSVC 14.29 из VS 2019 Build Tools
   (это же описано в SPEC §8 Этап 0.0), поэтому рабочая база пресетов —
   `Visual Studio 16 2019`, а под MSVC 2022 заведена отдельная база
   `vs2022-base`. На CI (`windows-latest`) пресеты `vs2022-*` работают сразу.
3. **vcpkg / FetchContent и Catch2** (SPEC §8 Этап 0) не тронуты: это чужие
   файлы (тесты и ядро), а §6.1 / ADR-004 требуют, чтобы ядро собиралось без
   сети и без внешних зависимостей. Точка подключения зависимостей — этот же
   каталог: `include(MrProperDependencies)` в верхнем уровне, когда решение
   будет принято.
4. **Проверка возможностей тулчейна.** SPEC §8 Этап 0.0 утверждает, что на
   MSVC 14.29 нет `std::stop_token`/`std::jthread`. Проверено на этой машине:
   доступны `std::stop_token`/`std::jthread`, `std::format`, `std::filesystem`
   и `std::ranges` (см. вывод `build.bat` ниже). Спецификацию стоит уточнить —
   от этого зависит, нужна ли собственная обёртка отмены в волнах W11-W12.

## Про `compile_commands.json`

`CMAKE_EXPORT_COMPILE_COMMANDS` включён, но CMake 3.20 реализует его только для
генераторов Makefiles и Ninja, а на генераторе Visual Studio **молча**
игнорирует. Поэтому конфигурация предупреждает об этом сразу, а рабочий путь
для clang-tidy — пресеты `ninja-*`: `ninja.exe` входит в поставку Visual
Studio, генерируется `build/ninja-debug/compile_commands.json`. Если CI
(windows-latest) получит CMake новее, достаточно снять предупреждение.

## Что проверено на этой машине, а что нет

Проверено через `tools\build.bat` и `tools\test.bat`:

* Debug и Release собираются с нуля, тесты проходят (32 проверки);
* флаги санитайзера доходят до командной строки компилятора и компоновщика
  (`/fsanitize=address` в `AdditionalOptions` сгенерированного `.vcxproj`),
  рантайм ASan линкуется в бинарь, проект собирается под ASan целиком;
* сгенерированный `build/Debug/cmake_install.cmake` содержит правила установки
  библиотеки, заголовков, набора правил и префикс per-user;
* `CPackConfig.cmake` содержит `set(CPACK_GENERATOR "NSIS;ZIP")`;
* `add_test(core_unit)` попал в `build/Debug/tests/unit/CTestTestfile.cmake`,
  то есть `ctest` (и CI) увидит тесты, а не только `tools\test.bat`;
* проверка возможностей тулчейна: MSVC 19.29.30159.0 умеет `std::stop_token` /
  `std::jthread`, `std::format`, `std::filesystem`, `std::ranges` — все четыре
  проверки зелёные (строка «тулчейн … все нужные возможности C++20 есть» в логе
  `build.bat`).

Не проверено (и почему):

* **Прогон под ASan на MSVC.** В Visual Studio 2019 16.11 (MSVC 19.29) и
  отладочный, и динамический рантайм ASan не инициализируются: процесс
  завершается с `0xC0000142` без вывода, даже если `clang_rt.asan*.dll`
  положить рядом с exe. Это ограничение тулчейна, а не конфигурации: при
  конфигурации выдаётся предупреждение с этим текстом. Надёжный путь —
  пресеты `wsl-*` (GCC/Clang) или Visual Studio 2022 17.x, как в CI.
* **Пресеты `ninja-*` и `wsl-*`.** Пресеты не вызываются ни `build.bat`, ни
  `test.bat`, а в WSL нет `cmake`; в этой среде их прогнать нечем. Их поля
  сверены с локальной документацией CMake 3.20
  (`Help/manual/cmake-presets.7.rst`) — в частности, убраны `outputErrors` и
  добавлен `configuration` у тестовых пресетов, иначе CMake 3.20 отверг бы файл.
* **`cmake --install` и `cpack`.** `tools\build.bat` и `tools\test.bat` их не
  вызывают, а правила установки, как показано выше, генерируются корректно.

## Известные особенности MSVC, уже учтённые

* `CMAKE_BUILD_TYPE` в генераторе Visual Studio не влияет ни на что: конфигурацию
  выбирает `--config`. `tools\build.bat` передаёт `-DCMAKE_BUILD_TYPE`, проект
  читает и показывает это значение в отчёте, иначе CMake ругается на
  «неиспользованный» параметр командной строки.
* `/INCREMENTAL:NO` в командной строке генератор Visual Studio отбрасывает
  (он управляет свойством `LinkIncremental`), поэтому для ASan отключение
  инкрементальной компоновки задаётся свойством каталога `LINK_INCREMENTAL`.
* C5072 («встраивание отключено из-за ASan») при `/WX` становится ошибкой
  сборки, поэтому при санитайзерах диагностика гасится через `/wd5072`.
* Имя цели `mrproper_unit_tests` и её путь `build\<Config>\mrproper_unit_tests.exe`
  закреплены за `tools\test.bat` — переименовывать нельзя.
