# Сборка MrProper: контракт для агентов и разработчиков

Владелец: W01, задача 01 (кроме `cmake/warnings.cmake` — он принадлежит задаче 03).
Спека: SPEC §8 Этап 0, §9.1 ADR-001, §11.

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

Побочный эффект этого правила: пресеты (`cmake --preset …`) и цели
`mrproper-format` / `mrproper-tidy` **не проверялись запуском** — для этого
понадобилось бы звать `cmake` напрямую. Всё, что о них сказано ниже, сверено
по сгенерированным файлам (`build/Debug/**/*.vcxproj`, `CTestTestfile.cmake`,
`cmake_install.cmake`, `CPackConfig.cmake`), а не по факту запуска. Честная
отметка об этом стоит в разделе «Что проверено, а что нет».

## Что делает верхний уровень

| Что | Где | Зачем |
|---|---|---|
| C++20 без расширений, экспорт `compile_commands.json` | `CMakeLists.txt` | нужен clang-tidy и IDE |
| `/utf-8` для всех целей | `CMakeLists.txt` | в исходниках кириллица; без флага MSVC даёт C4819, а с `/WX` это ошибка |
| опции `MRPROPER_*` | `cmake/MrProperOptions.cmake` | единая точка настройки |
| предупреждения как ошибки | `cmake/MrProperWarnings.cmake` | SPEC §9.1 ADR-001 |
| санитайзеры | `cmake/MrProperSanitizers.cmake` | SPEC §11 |
| vcpkg / FetchContent | `cmake/MrProperDependencies.cmake` | SPEC §8 Этап 0 |
| install-цели и CPack | `cmake/MrProperInstall.cmake` | установщик — следствие Этапа 0 |
| цели формата и анализа | `cmake/MrProperStyle.cmake` | SPEC §8 Этап 0 («clang-format + clang-tidy») |
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
| `MRPROPER_CHECK_TOOLCHAIN` | `ON` | проверка `stop_token`, `<format>`, `<filesystem>`, `<ranges>` на configure |
| `MRPROPER_WARNINGS_AS_ERRORS` | `ON` | `/WX`, `-Werror` |
| `MRPROPER_STRICT_WARNINGS` | `OFF` | сверх `/W4`: `/w44242…/w44272`, `-Wshadow -Wconversion -Wold-style-cast` |
| `MRPROPER_SANITIZE` | `OFF` | `OFF` / `address` / `undefined` / `address,undefined` |
| `MRPROPER_DEPS_PROVIDER` | `auto` | `auto` / `vcpkg` / `fetchcontent` / `none` |
| `MRPROPER_ALLOW_NETWORK` | `OFF` | разрешать FetchContent качать зависимости |
| `MRPROPER_INSTALL_TESTS` | `OFF` | ставить ли `mrproper_unit_tests` |
| `MRPROPER_ENABLE_CPACK` | `ON` | генераторы установщика NSIS и ZIP |
| `MRPROPER_INSTALL_PREFIX` | пусто | префикс установки; пусто = per-user `%LOCALAPPDATA%\Programs\MrProper` |
| `MRPROPER_APP_TARGET` | `mrproper_app` | цель, к которой подключаются манифест и ресурс версии |
| `MRPROPER_COMPILE_COMMANDS` | пусто | каталог с `compile_commands.json` для цели `mrproper-tidy` |

### `MRPROPER_SANITIZE`

Задаётся тремя способами, по убыванию приоритета:

1. переменная окружения `MRPROPER_SANITIZE` — единственный способ включить
   санитайзеры через `tools\build.bat`, который передаёт только
   `-DCMAKE_BUILD_TYPE`:
   ```bat
   set MRPROPER_SANITIZE=address && tools\build.bat Debug
   ```
2. `-DMRPROPER_SANITIZE=…` в командной строке, в том числе из пресета;
3. пусто, то есть выключено.

**Значение из переменной окружения не пишется в кэш.** Это сделано намеренно:
при старой схеме (`set(MRPROPER_SANITIZE "$ENV{…}" CACHE STRING …)`) один
прогон с `MRPROPER_SANITIZE=address` навсегда оставлял каноническое дерево
`build\Debug` под санитайзером — в кэше лежал бы `address`, ключ `set(CACHE)`
без `FORCE` его бы не перебил, а следующий `tools\build.bat Debug` уже без
переменной окружения собирал бы с санитайзерами и никто бы об этом не узнал.
Сейчас переменная окружения перекрывает кэш обычной переменной, а кэш хранит
только то, что человек задал явно. Проверено: после прогона с санитайзером в
`build/Debug/CMakeCache.txt` остаётся `MRPROPER_SANITIZE:STRING=`.

Нормализация значения (регистр, пробелы, `+` вместо `,`) сделана **точечными
строковыми заменами, а не регулярным выражением** `[ \t\r\n]+`. На CMake 3.20
класс символов с `\r\n` внутри компилируется, но не совпадает ни с чем, и
хвостовой пробел от `set X=address && …` доходил до проверки как `address ` —
то есть путь через переменную окружения, ради которого он и написан, падал с
`FATAL_ERROR`. Молча не сработавшая нормализация хуже явной ошибки, поэтому
регулярные выражения в нормализации значений не используются.

Недопустимое значение останавливает конфигурацию, а не молча отключает
проверки.

### Зависимости: vcpkg или FetchContent

SPEC §8 Этап 0 требует «vcpkg или FetchContent для зависимостей». Механизм
лежит в `cmake/MrProperDependencies.cmake` и по умолчанию ничего не делает —
до сих пор у проекта нет внешних зависимостей (тест-харнес свой,
`tests/harness.hpp`; Catch2 сознательно не подключён, ADR-001).

```cmake
# 1) пакет из системы / vcpkg
mrproper_find_dependency(fmt [VERSION 11])

# 2) конкретный источник с закреплённым тегом
mrproper_fetch_dependency(mrproper_thing
    GIT_REPOSITORY https://github.com/…/thing.git
    GIT_TAG            v1.2.3          # без тега конфигурация невоспроизводима
    FIND_PACKAGE_ARGS  NAMES thing     # если пакет уже есть — не качать второй
    EXCLUDE_FROM_ALL)
```

Правила, которые стоит знать до того, как объявлять зависимость:

* **Сеть запрещена по умолчанию** (`MRPROPER_ALLOW_NETWORK=OFF`). Каноническая
  сборка `tools\build.bat` не должна зависеть от сети; включение скачивания —
  явный `-DMRPROPER_ALLOW_NETWORK=ON`. Попытка объявить зависимость без него
  падает с объяснением, а не собирает проект без неё.
* `GIT_TAG` обязателен: ветка `master` сделала бы сборку невоспроизводимой.
* Список зависимостей не хранится в модуле — он объявляется в `CMakeLists.txt`
  того слоя, которому зависимость нужна. У `src/core` их не бывает вовсе.
* Статус vcpkg печатается при каждой конфигурации (`vcpkg не используется`
  либо `подключён, триплет «x64-windows»»), поэтому непонятно, почему пакет не
  нашёлся, обычно видно сразу.

## Формат и статический анализ

`cmake/MrProperStyle.cmake` объявляет три цели, каждая с `EXCLUDE_FROM_ALL` —
то есть обычная сборка, а значит и `tools\build.bat`, их никогда не запускает:

| Цель | Что делает |
|---|---|
| `mrproper-format` | `clang-format -i` по `src/**` и `tests/**` |
| `mrproper-format-check` | проверка без правки файлов; ненулевой код возврата = «формат нарушен» |
| `mrproper-tidy` | `clang-tidy -p <каталог с compile_commands.json>` по тем же файлам |

```bat
cmake --build --preset format        :: применить формат
cmake --build --preset format-check  :: ворота формата для CI
cmake --build --preset tidy          :: clang-tidy
```

Цель без инструмента падает с объяснением, а не молча ничего не делает: ворота,
которые не могут провалиться, ничем не отличаются от отсутствующих. Требуется
`clang-format ≥ 10` (ради `--dry-run --Werror`; на более старом проверка идёт
через `cmake/MrProperFormatCheck.cmake` и `--output-replacements-xml`).

Правила анализа задаёт `.clang-tidy`, формат — `.clang-format` (задача 03).
Этот модуль их не читает и не имеет права их менять: он только вызывает
инструменты.

`mrproper-tidy` требует `compile_commands.json`, которого генератор Visual
Studio на CMake 3.20 не пишет (см. ниже). Каталог берётся из
`MRPROPER_COMPILE_COMMANDS` либо ищется в `build/*/`; CI (шаг `clang-tidy`)
собирает базу отдельно в `build/tidy` генератором Ninja.

## Пресеты

`CMakePresets.json` написан для CMake 3.20 (единственная версия, которая
есть в Visual Studio 2019 Build Tools на этой машине), поэтому использует
схему версии 2: без `installDir`, `workflowPresets` и `outputErrors`.

| Пресет | Каталог | Назначение |
|---|---|---|
| `dev` | `build/Debug` | пресет, которым SPEC §8 Этап 0 проверяется: `cmake --build --preset dev && ctest --preset dev` |
| `debug` / `release` | `build/Debug`, `build/Release` | то же дерево, что у `tools\build.bat` |
| `vs2022-dev`, `vs2022-asan-debug` | `build/vs2022-*` | целевой тулчейн из спеки (MSVC v143) и Debug+ASan на нём |
| `asan-debug` | `build/asan-debug` | Debug + AddressSanitizer, отдельное дерево, чтобы ASan не закрепился в кэше обычной сборки |
| `ninja-debug`, `ninja-release` | `build/ninja-*` | генератор Ninja: быстрая сборка и `compile_commands.json` для clang-tidy |
| `wsl-debug` / `wsl-release` | `build/wsl-*` | ядро на хосте с GCC/Clang, `address,undefined` |
| `format`, `format-check`, `tidy` | `build/Debug` | цели формата и анализа |

```bat
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Пресеты `ninja-*`, `asan-debug` и цели формата — вспомогательные: канонической
сборкой проекта остаётся `tools\build.bat`. Им нужно окружение MSVC
(`vcvars64.bat`, то есть Developer Command Prompt), тогда как `tools\build.bat`
находит тулчейн сам.

**Ограничение схемы версии 2.** Файл пресетов привязан к CMake 3.20, который
понимает только схему версии 2. Современные CMake (4.x) схему версии 2 не
принимают: там нужен актуальный номер версии (6 и выше). Один файл на оба
тулчейна не рассчитан, поэтому на машине с новым CMake пресеты придётся
поднять отдельной правкой — с точным номером версии на этой машине проверить
нечем, CMake 4.x здесь не установлен. Это осознанный размен: сейчас пресеты
работают там, где идёт каноническая сборка.

## Про `compile_commands.json`

`CMAKE_EXPORT_COMPILE_COMMANDS` включён, но CMake 3.20 реализует его только для
генераторов Makefiles и Ninja, а на генераторе Visual Studio **молча**
игнорирует. Поэтому:

* `WARNING` печатается только если файл запросили явно
  (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` или пресетом) — тогда это забота
  человека, и молчание было бы ошибкой;
* при собственном значении по умолчанию печатается строка в отчёте о
  конфигурации (`compile_commands.json: генератор … не пишет … путь — пресет
  ninja-debug`) без `WARNING`: это известное свойство генератора, а не ошибка,
  и предупреждение в каждом логе сборки только учит игнорировать
  предупреждения.

Рабочий путь для clang-tidy и IDE — пресеты `ninja-*`: `ninja.exe` входит в
поставку Visual Studio, `build/ninja-debug/compile_commands.json` создаётся.

## Что проверено на этой машине, а что нет

Проверено через `tools\build.bat` и `tools\test.bat` (обе конфигурации,
27.09.2026, MSVC 19.29.30159.0, CMake 3.20.21032501-MSVC_2):

* Debug и Release собираются с нуля, тесты проходят — 32 проверки, 0 провалов;
* конфигурация печатает отчёт (проект, генератор, компилятор, C++, тесты,
  предупреждения, санитайзеры, `compile_commands.json`, install, зависимости);
* пресет `dev` и пресеты `debug`/`release` указывают на те же каталоги, что и
  скрипты, — чередование не создаёт двух деревьев;
* `add_test(core_unit)` попал в `build/Debug/tests/unit/CTestTestfile.cmake`,
  то есть `ctest` (и CI) увидит тесты, а не только `tools\test.bat`;
* `cmake_install.cmake` содержит 8 правил установки: `lib/` (статическое ядро),
  `include/mrproper/` (заголовки `*.hpp/*.h`), `share/MrProper/rules/`,
  `share/MrProper/` (`LICENSE`, `README.md`) и префикс
  `C:/Users/Daniil/AppData/Local/Programs/MrProper` (per-user, права
  администратора не нужны — SPEC §4 FR-9);
* `CPackConfig.cmake` содержит `set(CPACK_GENERATOR "NSIS;ZIP")` и
  `CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL`;
* санитайзеры включаются через переменную окружения, `/fsanitize=address`
  доходит до `ClCompile` всех четырёх конфигураций, проект собирается целиком;
* значение `MRPROPER_SANITIZE` из переменной окружения **не остаётся в кэше**:
  после прогона с санитайзером в `CMakeCache.txt` пусто, обычный
  `tools\build.bat Debug` собирается без них;
* цели `mrproper-format`, `mrproper-format-check`, `mrproper-tidy` созданы
  (три отдельных `.vcxproj`) и **не входят в `ALL_BUILD`** — обычная сборка их
  не запускает.

Найдено и исправлено в ходе этой задачи (каждый пункт воспроизведён на машине):

1. **Нормализация `MRPROPER_SANITIZE` не работала.** Регулярное выражение
   `[ \t\r\n]+` на CMake 3.20 компилируется, но не совпадает ни с одним
   символом, поэтому `set MRPROPER_SANITIZE=address && tools\build.bat Debug`
   падал с `FATAL_ERROR: MRPROPER_SANITIZE="address " недопустимо`. То есть весь
   путь через переменную окружения — единственный способ включить санитайзеры
   через `build.bat` — не работал. Заменено на `string(STRIP)` плюс точечные
   `string(REPLACE)`.
2. **Одно значение из окружения «залипало» в кэше** и навсегда оставляло
   канонический `build\Debug` под санитайзером. Описано выше.
3. **`/fsanitize=address` передавался линковщику.** `link.exe` 19.29 его не
   знает и печатал `LNK4044: нераспознанный параметр "/fsanitize=address";
   игнорируется` на каждый линкующий проект. Флаг — свойство компилятора;
   в линковку он не передаётся, рантайм подтягивает линкер сам.
4. **Отключение инкрементальной компоновки для ASan не доходило до проекта.**
   Ни `set_property(DIRECTORY PROPERTY LINK_INCREMENTAL 0)`, ни
   `/INCREMENTAL:NO` в `add_link_options` не попадают в `.vcxproj` генератора
   Visual Studio 16 2019 на CMake 3.20: в `Link/AdditionalOptions` флага нет, а
   `<LinkIncremental>` приходит пустым элементом из шаблона. Свойство
   ставится на цели напрямую через `BUILDSYSTEM_TARGETS` — генератор записывает
   его как есть. Что из этого сработает на VS 2022, здесь не проверяется.

Известное ограничение тулчейна, а не конфигурации:

* **Прогон под ASan на MSVC 19.29 (VS 2019 16.11) невозможен.** Сборка проходит
  целиком, но процесс не стартует: код возврата 66
  (`STATUS_DLL_INIT_FAILED`) и пустой вывод, даже если положить
  `clang_rt.asan*.dll` рядом с exe. На configure выдаётся предупреждение с
  этим текстом, чтобы пустой вывод не выглядел как «тесты не нашлись».
  Надёжный путь — пресеты `wsl-*` (GCC/Clang) или Visual Studio 2022 17.x,
  как в CI. Проверено: `tools\test.bat Debug` над ASan-сборкой.

Не проверено (и почему):

* **Пресеты.** Ни `build.bat`, ни `test.bat` их не вызывают, а правило
  проверки запрещает звать `cmake` напрямую. Файл проверен только как JSON
  (14 configure, 13 build, 6 test пресетов) и сверён с локальной документацией
  CMake 3.20 (`Help/manual/cmake-presets.7.rst`): убраны `outputErrors` и
  `installDir`, у тестовых пресетов есть `configuration`. Сам
  `cmake --preset dev` не запускался — пункт проверки SPEC §8 Этап 0
  («`cmake --build --preset dev && ctest --preset dev` — зелёные») остаётся
  невыполненным по причине правила, а не по существу.
* **Цели формата и анализа.** `clang-format` и `clang-tidy` на этой машине не
  установлены (`MRPROPER_CLANG_FORMAT-NOTFOUND`, `MRPROPER_CLANG_TIDY-NOTFOUND`),
  а запуск `cmake --build --target …` запрещён правилом проверки. Проверено
  только, что цели созданы, не входят в `ALL_BUILD` и корректно сообщают об
  отсутствии инструмента.
* **`cmake --install` и `cpack`.** Ни `build.bat`, ни `test.bat` их не зовут.
  Правила установки генерируются корректно (проверено по `cmake_install.cmake`
  и `CPackConfig.cmake`), сам установщик не собирался.
* **Санитайзеры на GCC/Clang** — пресеты `wsl-*` требуют `cmake` на хосте, в
  WSL его нет.

## Известные особенности MSVC, уже учтённые

* `CMAKE_BUILD_TYPE` в генераторе Visual Studio не влияет ни на что: конфигурацию
  выбирает `--config`. `tools\build.bat` передаёт `-DCMAKE_BUILD_TYPE`, проект
  читает и показывает это значение в отчёте, иначе CMake ругается на
  «неиспользованный» параметр командной строки.
* C5072 («встраивание отключено из-за ASan») при `/WX` становится ошибкой
  сборки, поэтому при санитайзерах диагностика гасится через `/wd5072`.
* Имя цели `mrproper_unit_tests` и её путь `build\<Config>\mrproper_unit_tests.exe`
  закреплены за `tools\test.bat` — переименовывать нельзя.
* Запуск `cmd.exe` из WSL на этой машине периодически падает с
  `UtilAcceptVsock: accept4 failed 110`. Это не сборка: лога не появляется, и
  команда повторяется. Проверка, которая «упала» на первой попытке, обычно
  проходит со второй-третьей.
