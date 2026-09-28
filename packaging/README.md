# Упаковка: манифест приложения и ресурс версии

Каталог `packaging/` закрывает две вещи, которые Windows требует от любого
десктопного приложения: манифест запуска (права, совместимость, DPI, длинные
пути) и ресурс версии (свойства файла, «Программы и компоненты», диспетчер задач).

Где это зафиксировано в `docs/SPEC.md`:

* §5 «Безопасность» — «всегда UAC-манифест», «рантайм-повышения нет; повышение —
  один раз при старте»;
* §5 «Платформа» и «Совместимость» — Windows 10 22H2 / Windows 11 x64,
  `_WIN32_WINNT=0x0A00`, x86 не поддерживается;
* §5 «DPI» — Per-Monitor V2; §5 «Пути» — длинные пути, пробелы, не-ASCII,
  кириллица; §5 «Тема»/«Локализация» — системная тема, переводы в ресурсах;
* §8, Этап 5 — «Установщик (WiX или Inno Setup), манифест UAC (`asInvoker` +
  elevation helper)»;
* §10, риск «Установка требует прав, которые мы не хотим давать всему UI» —
  митигация `asInvoker` + helper только на операцию;
* §12, Definition of Done — «приложение запускается без повышения прав; повышение
  запрашивается только на операции»;
* §4 FR-8 — отчёт содержит версию приложения; §13.1 — имя продукта `MrProper`
  используется в строках версии; §3 — границы версий.

Замечание по терминологии задачи: ссылка «§4 FR-9» в текущей редакции SPEC.md
указывает на «Настройки и политики» — к манифесту это отношения не имеет.
Выше перечислены реальные требования, которые этот каталог закрывает.

| Файл | Что это | Кто компилирует |
| --- | --- | --- |
| `app.manifest` | Манифест приложения: `asInvoker`, Windows 10/11, PerMonitorV2, UTF-8, `longPathAware` | линкер MSVC, ресурс `RT_MANIFEST` (тип 24) |
| `version.rc` | `VS_VERSION_INFO`: версия, издатель, описание, оригинальное имя файла | `rc.exe` |
| `README.md` | Этот файл: как встроить, где правда о версии, что проверить | — |

Ни один из файлов не является исходником C++, поэтому в дерево сборки их
подключает владелец CMake-файлов (задачи 01/02), а не этот каталог.

## 1. Встраивание в CMake

Владелец `CMakeLists.txt` (задачи 01/02) добавляет в цель приложения ровно
четыре строки:

```cmake
target_sources(mrproper_app WIN32 PRIVATE packaging/version.rc)
target_link_options(mrproper_app PRIVATE
    /MANIFEST:EMBED
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/packaging/app.manifest")
```

Правила, которые ломают запуск, если их нарушить:

1. **Манифест должен быть ровно один.** `/MANIFEST:EMBED` с `/MANIFESTINPUT`
   встраивают `app.manifest`. Если параллельно добавить в `version.rc` строку
   `1 24 "app.manifest"`, в `.exe` окажется два `RT_MANIFEST`, и загрузчик
   откажется запускать файл. Второй способ НЕ применяется.
2. **`/MANIFEST:NO` ставить нельзя.** Он выключает встраивание манифеста
   линкером, и вместе с ним пропадает `asInvoker` (то есть UAC-поведение
   вернётся к умолчанию линкера), DPI и `longPathAware`.
3. **Путь к манифесту — абсолютный и в кавычках.** Иначе `D:\Project\...`
   без пробелов соберётся, а путь с пробелами — нет.
4. `version.rc` компилируется как ресурс, ему нужны Windows SDK и
   `code_page(65001)` (уже стоит в файле) — иначе русские строки испортятся.
5. **`_DEBUG` в ресурс приходит только из командной строки `rc.exe`.** Сам
   `rc.exe` этот символ не определяет: сборка без ключей даёт
   `dwFileFlags = 0x0`, с `/D_DEBUG` — `dwFileFlags = 0x1`. Обычные
   `target_compile_definitions` попадают в `CL`, а не в `RC`, поэтому если
   владелец CMake-файла хочет `VS_FF_DEBUG` в Debug-сборке, определение надо
   передать именно шагу компиляции ресурса (свойства `.rcproj` либо своя
   `add_custom_command` с `rc.exe`). Если не передать — файл остаётся валидным,
   просто без бита «отладочная сборка»; это не ошибка, а отсутствие отметки.

## 2. Права: почему `asInvoker`

`requestedExecutionLevel level="asInvoker"` означает: приложение стартует с
правами текущего пользователя и **не** просит повышения при запуске. Это
осознанный выбор, а не временная заглушка:

* утилита чистит то, что принадлежит пользователю (temp профиля, кэши браузеров,
  логи приложений) — права администратора для этого не нужны, а запрос UAC
  при каждом запуске учит пользователя нажимать «Да» на любые окна;
* приложение ничего не делает молча вслепую: удаление идёт по плану, который
  пользователь собрал и подтвердил, а категории с риском `Risky` в MVP только
  оцениваются — `winsxs.report` (очистка WinSxS — `StartComponentCleanup`,
  руками не трогать) и `installer.cache` (удаление ломает uninstall/repair);
  `Risky` по умолчанию скрыт (§4 FR-3, §4 FR-4, §12);
* `uiAccess="false"` — приложению не нужен ввод в защищённый рабочий стол.

Что делать, если elevated-режим понадобится позже: **отдельный** исполняемый
файл-хелпер, который запускается через `ShellExecuteEx` с глаголом `runas` и
только после явного подтверждения пользователя. Манифест основной программы
при этом не меняется, `asInvoker` остаётся: повышение прав — решение конкретной
операции, а не свойство всего приложения.

## 3. Версия: где правда

Правда — `project(MrProper VERSION ...)` в `CMakeLists.txt`. Оттуда версия
раздаётся в четыре места, и все четыре должны совпадать:

| Место | Что хранит | Сейчас |
| --- | --- | --- |
| `CMakeLists.txt` | `project(MrProper VERSION ...)` | `0.1.0` |
| `packaging/app.manifest` | `assemblyIdentity/@version` | `0.1.0.0` |
| `packaging/version.rc` | `FILEVERSION`, `PRODUCTVERSION` (числа) | `0.1.0.0` |
| `packaging/version.rc` | `FileVersion`, `ProductVersion` (строки) | `0.1.0.0` |
| `src/cli` (задача 61) | вывод `--version` | — |

Связать автоматически можно так: в `version.rc` все значения защищены
`#ifndef`, поэтому CMake переопределяет их через `/D`, а файл править не нужно:

```cmake
target_compile_definitions(mrproper_app PRIVATE
    MRPROPER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MRPROPER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MRPROPER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    MRPROPER_VERSION_BUILD=0
    MRPROPER_FILEVERSION_STRING="${PROJECT_VERSION}.0"
    MRPROPER_PRODUCTVERSION_STRING="${PROJECT_VERSION}.0")
```

Два ограничения, из-за которых строка версии задаётся целиком, а не склеивается
из чисел:

* у препроцессора `rc.exe` нельзя надёжно конкатенировать строки — попытка
  собрать `MAJOR "." MINOR` даёт мусор в свойствах файла;
* `assemblyIdentity/@version` в манифесте — это XML, препроцессор туда не
  попадёт, его нужно обновлять руками при bump-е.

Порядок bump-е: `CMakeLists.txt` → `app.manifest` → строковые версии в
`version.rc` → вывод `--version` в CLI. Числовые `FILEVERSION/PRODUCTVERSION`
подхватят версию сами, если CMake передаёт `/D`.

## 4. Что заменить перед релизом

В `version.rc` два поля — плейсхолдеры, и оставлять их такими в релизе нельзя:
свойства файла будут врать пользователю.

* `MRPROPER_COMPANY_NAME` — настоящее имя издателя;
* `MRPROPER_COPYRIGHT` — строка правообладателя с годом.

Русский и английский варианты строк лежат рядом (блоки `040904B0` и `04090409`),
поэтому интерфейс свойств будет русским, а в англоязычной Windows — английским.
Если продукт останется только русским, блок `04090409` и его `Translation`
удаляются.

## 5. Проверки

Главный критерий задачи — манифест валиден как XML. Проверяется из WSL
(репозиторий виден как `/mnt/d/Project/MrProper`):

```bash
cd /mnt/d/Project/MrProper
python3 -c "import xml.etree.ElementTree as ET; ET.parse('packaging/app.manifest'); print('OK')"
```

Полная проверка, включая значения (вывод `OK 0.1.0.0 asInvoker 1 4` означает:
версия из `assemblyIdentity`, `asInvoker`, ровно один `supportedOS`, четыре
элемента `windowsSettings`):

```bash
cd /mnt/d/Project/MrProper && python3 - <<'PY'
import xml.etree.ElementTree as ET
NS = {'asm.v1': 'urn:schemas-microsoft-com:asm.v1',
      'asm.v3': 'urn:schemas-microsoft-com:asm.v3',
      'compat': 'urn:schemas-microsoft-com:compatibility.v1'}
r = ET.parse('packaging/app.manifest').getroot()
ver = r.find('asm.v1:assemblyIdentity', NS).get('version')
lvl = r.find('.//asm.v3:requestedExecutionLevel', NS).get('level')
sos = len(r.findall('.//compat:supportedOS', NS))
win = len(list(r.find('asm.v3:application/asm.v3:windowsSettings', NS)))
print('OK', ver, lvl, sos, win)
PY
```

Манифест дополнительно проверяется штатным инструментом Windows SDK — он
одновременно проверяет и XML, и то, что сборка идентичности осмысленна:

```bat
REM из корня репозитория
"%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.19041.0\x64\mt.exe" ^
  -nologo -validate_manifest -manifest packaging\app.manifest -out:%TEMP%\mt-app.xml
REM ожидается «Parsing of manifest successful», код возврата 0
```

Ресурс версии собирается тем же `rc.exe`, что и проект (три варианта: обычный,
`/D_DEBUG`, переопределение версии через `/D`):

```bat
set "RC=%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.19041.0\x64\rc.exe"
set "INC=%ProgramFiles(x86)%\Windows Kits\10\Include\10.0.19041.0"
"%RC%" /nologo /fo "%TEMP%\mrp-rel.res" /i "%INC%\um" /i "%INC%\shared" packaging\version.rc
"%RC%" /nologo /D_DEBUG  /fo "%TEMP%\mrp-dbg.res" /i "%INC%\um" /i "%INC%\shared" packaging\version.rc
```

Числа версии в собранном ресурсе читаются из `VS_FIXEDFILEINFO` по сигнатуре
`0xFEEF04BD` (в `.res` это 8 байт подряд: `FILEVERSION` = два `WORD` из
`dwFileVersionMS/LS`, произведение вида `%d.%d.%d.%d`); строки лежат рядом в
UTF-16LE и читаются обычным поиском подстроки.

Когда появится собранный `.exe` (цели приложения в дереве сборки пока нет —
её создают задачи 01/02 и 66), манифест и версию в бинарнике проверяют так:

```bat
mt.exe -inputresource:MrProper.exe -out:%TEMP%\embedded.xml
dumpbin /headers MrProper.exe | findstr /i subsystem
```

Проект целиком собирается и тестируется только через обёртки:
`tools\build.bat <Debug|Release>` и `tools\test.bat <Debug|Release>`.

Что проверено на этой машине, а что нет (проверено в этом прогоне):

* `app.manifest` разбирается `xml.etree`, содержит `asInvoker` + `uiAccess=false`,
  ровно один `supportedOS` для Windows 10/11 и все четыре элемента
  `windowsSettings` — **да**;
* `mt.exe -validate_manifest` на `app.manifest` — **да**: «Parsing of manifest
  successful», код 0. Контрольный пустой файл при этом честно отвергается
  (`c1010070`, код 31), то есть инструмент рабочий, а не «сломавшийся на
  этой машине»;
* `version.rc` собирается `rc.exe` во всех трёх вариантах: обычный — `0.1.0.0`,
  `/D_DEBUG` — `0.1.0.0` и `dwFileFlags=0x1` (`VS_FF_DEBUG`), переопределение
  через `/D` — `1.2.3.4` и строки `1.2.3.4`; русские строки («очистка диска»)
  и английские целы — **да**;
* проект собирается через `tools\build.bat Debug` — **да** (цель приложения в
  дереве сборки ещё нет, собираются `mrproper_core` и `mrproper_unit_tests`);
* запуск собранного `.exe`, просмотр свойств в проводнике, встраивание
  манифеста линкером — **нет**: цели приложения пока не существует, а
  `CMakeLists.txt` (владелец — задачи 01/02) ещё не подключает
  `packaging/version.rc` и `/MANIFESTINPUT`.

Отдельно: проверяйте `version.rc` **и в Debug**. Ветка `#ifdef _DEBUG` в обычной
сборке не компилируется, и опечатка в её макросе (например, несуществующего
`VIF_DEBUG` вместо `VS_FF_DEBUG`) проявится только в Debug-конфигурации —
именно там, где собирается основная разработка.
