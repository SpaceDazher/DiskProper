; ===========================================================================
;  MrProper — установщик для Windows 10/11 x64. Формат: Inno Setup 6.
; ===========================================================================
;  Владелец файла: J2. Спека: docs/SPEC.md §12.10 (пробел «нет установщика»),
;  §8 Этап 5, §5, §4 FR-7/FR-8, §9.1 ADR-009.
;
;  ПОЧЕМУ INNO SETUP, А НЕ WIX
;  -------------------------
;  Формат в репозитории один: .iss. WiX (.wxs) сознательно не добавляется:
;    1) WiX — это NuGet-пакет (wix.exe) плюс .NET-зависимость и подпись
;       (wix build -bindpath ... требует установленного пакета), то есть
;       требование «не тащить nuget» из задачи J2 выполняется только Inno;
;    2) WiX Toolset 3.x/4.x требует SDK/msbuild, которого в образе Visual
;       Studio 2019 BuildTools на этой машине нет (проверено: каталогов
;       «WiX Toolset*» в Program Files нет, candle.exe/light.exe в PATH нет);
;    3) ISCC.exe — один самодостаточный файл, ставится без администратора
;       (/CURRENTUSER) и без nuget, компилирует .iss -> .exe одной командой;
;    4) .iss читается как текст и проверяется без установки чего бы то ни
;       было: список файлов виден глазами, ниже §«Состав» он совпадает с
;       фактическим содержимым build\.
;
;  НЕПОДПИСАННЫЙ УСТАНОВЩИК (ADR-009)
;  ----------------------------------
;  В [Setup] НЕТ строки SignTool: сертификата подписи нет, утилита личная
;  (SPEC §9.1 ADR-009, §10). Поэтому:
;    * первый экран установки (InitializeSetup ниже) прямо говорит, что
;      SmartScreen покажет «неизвестный издатель», и что это ожидаемо;
;    * проверять сборку следует по SHA-256, а не по подписи.
;  Если сертификат появится, добавляется ровно одна строка
;      SignTool=signtool /a $f $q
;  и удаляется блок InitializeSetup — остальное не меняется.
;
;  СОСТАВ (сверен с фактическим содержимым build\ на 2026-09-30)
;  -----------------------------------------------------------
;    bin\mrproper.exe            программа (GUI)  <- build\<cfg>\src\ui\<cfg>\
;    bin\mrproper-cli.exe        CLI             <- build\<cfg>\mrproper_cli.exe
;    share\MrProper\rules\*.json набор правил + manifest.json  (25 файлов)
;    share\MrProper\rules\rules.sig подпись набора (ADR-008), когда есть
;    share\MrProper\README.md    документация
;    share\MrProper\LICENSE      документация (MIT)
;    share\MrProper\docs\rules-authoring.md  документация
;
;  КУДА СТАВИТЬ И КУДА ПИШЕТ ПРОГРАММА
;  ------------------------------------
;  Файлы программы: {autopf}\MrProper = %ProgramFiles%\MrProper
;  Данные программы (их установщик НЕ трогает, см. [UninstallDelete]):
;    %ProgramData%\MrProper\Trash        корзина   (SPEC §153, src/core/trash.hpp:147)
;    %LOCALAPPDATA%\MrProper\reports     отчёты    (SPEC §162, src/ui/view_report.cpp:1470)
;    %LOCALAPPDATA%\MrProper\rules       состояние правил (src/platform/rulesync_client.cpp:167)
;  ВНИМАНИЕ: в постановке задачи J2 корзина названа как
;  «%LOCALAPPDATA%\MrProper\Trash». Это неверно: по действующему контракту
;  (SPEC §153 и src/core/trash.hpp) корзина общая и лежит в ProgramData,
;  иначе файл, удалённый администратором, нельзя было бы восстановить
;  обычным запуском. Ниже в диалогах написан реальный путь.
;
;  ФОРМАТ И ВЕРСИЯ: Inno Setup 6.3 или новее (6.3 — первая, где есть
;  skipifsourcedoesntexist и x64compatible; см. [Setup] и packaging/README.md §6).
;
;  СБОРКА
;  ------
;  Из CMake: tools\build.bat Release <слот>, затем
;      cmake --build build\<слот> --config Release --target mrproper-installer
;  Вручную:  ISCC.exe packaging\mrproper.iss   (подстановки по умолчанию ниже)
;  Обе команды дают один и тот же состав: файлы берутся из build\main\Release
;  и из rules\, версия — из project(MrProper VERSION ...).
; ===========================================================================

; --- Подстановки ------------------------------------------------------------
; Значения по умолчанию делают .iss самодостаточным (ISCC без аргументов
; собирает установщик из build\main), а CMake перекрывает их ключами /D.
; Обёртка #ifndef обязательна: /D эмулирует «#define public», то есть
; повторный #define безусловно упал бы с «redefined».
#ifndef MrVersion
  #define MrVersion "0.1.0"
#endif
#ifndef MrVersionFull
  #define MrVersionFull "0.1.0.0"
#endif
#ifndef MrAppName
  #define MrAppName "MrProper"
#endif
#ifndef MrSourceRoot
  #define MrSourceRoot ".."
#endif
#ifndef MrBuildRoot
  #define MrBuildRoot "..\build\main"
#endif
#ifndef MrBuildConfig
  #define MrBuildConfig "Release"
#endif
#ifndef MrOutputDir
  #define MrOutputDir "..\build\dist"
#endif

; Производные пути НЕ вынесены в отдельные #define: ISPP подставляет тело
; define один раз и рекурсию не делает, поэтому «#define MrGuiExe
; "{#MrBuildRoot}\..."» дало бы в [Files] путь с литеральными скобками.
; Полные пути написаны прямо в [Files] — мест, где они нужны, всего три.
; Слеши обратные: [Files]/[Run] отдают их Inno Setup, а он ждёт Windows-путь.

[Setup]
; AppId — постоянный идентификатор: по нему «Программы и компоненты» ищет
; запись, поэтому менять его нельзя, иначе после обновления в списке будет
; две строки, а удаление по старой перестанет работать.
AppId={{8F3C1A62-5D47-4B90-9C21-0E6A4B7D3F58}
AppName={#MrAppName}
AppVersion={#MrVersion}
AppVerName={#MrAppName} {#MrVersion}
AppPublisher=MrProper
AppPublisherURL=https://github.com/SpaceDazher/MrProper
AppSupportURL=https://github.com/SpaceDazher/MrProper/issues
AppUpdatesURL=https://github.com/SpaceDazher/MrProper/releases

; Требование J2 №4: файлы программы — в %ProgramFiles%. {autopf} раскрывается
; в %ProgramFiles% при PrivilegesRequired=admin (режим по умолчанию) и в
; %LOCALAPPDATA%\Programs при /CURRENTUSER. Второй режим оставлен сознательно:
; SPEC §4 FR-9 и §10 требуют, чтобы утилитой можно было пользоваться без
; прав администратора.
DefaultDirName={autopf}\{#MrAppName}
DefaultGroupName={#MrAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline

; Пункт в «Программах и компонентах»: его создаёт сам Inno Setup по AppId,
; отдельная строка не нужна. Имя и иконку задаём явно, чтобы в списке было
; видно версию, а не «Uninstall».
UninstallDisplayName={#MrAppName} {#MrVersion}
UninstallDisplayIcon={app}\bin\mrproper.exe
VersionInfoVersion={#MrVersionFull}
SetupLogging=yes

OutputDir={#MrOutputDir}
OutputBaseFilename={#MrAppName}-{#MrVersion}-win64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; Страница выбора языка убрана намеренно: она встаёт ПЕРЕД окном
; InitializeSetup с предупреждением SmartScreen (ADR-009), а требование
; J2 №2 — чтобы это предупреждение было первым экраном установки.
; Проверено на локали ru-RU: без этой строки Inno показывал
; TSelectLanguageForm «Выберите язык установки» раньше окна. Язык при этом
; определяется по системе (см. [Languages]): ru-RU → русский,
; en-US → английский, иначе первая строка раздела.
ShowLanguageDialog=no

; x64compatible, а не x64: второе объявлено устаревшим начиная с Inno Setup
; 6.3 и печатает предупреждение при каждой сборке. Нижняя граница версии
; компилятора — 6.3: там же появился флаг skipifsourcedoesntexist, которым
; пропускается отсутствующий rules.sig.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
MinVersion=10.0

; SignTool здесь отсутствует намеренно — см. шапку файла (ADR-009).

[Languages]
; Порядок строк = порядок выбора при неоднозначном совпадении, русский
; первым: продукт русскоязычный (SPEC, README, сообщения программы).
;
; ShowLanguageDialog=no убирает окно выбора языка. Причина не в
; удобстве, а в требовании J2 №2: окно InitializeSetup с
; предупреждением SmartScreen должно быть ПЕРВЫМ экраном установки, а
; окно выбора языка по умолчанию показывается ВСЕГДА (ShowLanguageDialog
; по умолчанию yes) и встаёт перед этим окном.
; Проверено на этой машине (локаль ru-RU): без этой строки Inno
; показывал TSelectLanguageForm «Выберите язык установки» раньше окна с
; предупреждением.
;
; Язык при этом определяется по системе: ru-RU -> русский, en-US ->
; английский; на любой другой локали берётся первая строка (русский).
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Icons]
; Ярлык в меню «Пуск» (требование J2 №3). Рабочий каталог — корень
; установки: программа читает набор правил относительно себя.
Name: "{group}\{#MrAppName}"; Filename: "{app}\bin\mrproper.exe"; \
  WorkingDir: "{app}"; IconFilename: "{app}\bin\mrproper.exe"; \
  Comment: "{cm:LaunchProgram,{#MrAppName}}"

[Files]
; --- Программа -------------------------------------------------------------
; GUI-цель называется mrproper (src/ui/CMakeLists.txt) и кладёт файл в
; подкаталог src\ui относительно корня сборки; CLI-цель mrproper_cli
; переопределяет RUNTIME_OUTPUT_DIRECTORY на корень (build\<cfg>\).
; Сборка называет файл mrproper_cli.exe, установка — mrproper-cli.exe:
; так он называется в SPEC §6.2 и в docs/SPEC.md:234. Переименование при
; установке безопасно: ни один исполняемый файл не ищет соседа по имени.
; ignoreversion обязателен — иначе переустановка той же версии спрашивает
; «перезаписать?», а разницы в файле может и не быть.
Source: "{#MrBuildRoot}\src\ui\{#MrBuildConfig}\mrproper.exe"; \
  DestDir: "{app}\bin"; \
  DestName: "mrproper.exe"; Flags: ignoreversion
Source: "{#MrBuildRoot}\{#MrBuildConfig}\mrproper_cli.exe"; \
  DestDir: "{app}\bin"; \
  DestName: "mrproper-cli.exe"; Flags: ignoreversion

; --- Набор правил (ADR-008) ------------------------------------------------
; Каталог целиком, а не список: агенты добавляют правила по одному файлу, и
; жёсткий перечень означал бы, что новое правило молча не попадёт в
; установку. Исключение — rules\*.key, *.pem и rules\private-*: приватная
; часть ключа подписи в репозитории не лежит (tools/rule_keys.md §1) и в
; установку попасть не должна ни при каких условиях.
Source: "{#MrSourceRoot}\rules\*.json"; DestDir: "{app}\share\MrProper\rules"; \
  Flags: ignoreversion
; Подпись набора: rules.sig лежит рядом с манифестом (ADR-008,
; src/platform/rulesync_client.hpp:105). На 2026-09-30 файла в репозитории
; нет — флаг skipifsourcedoesntexist (Inno Setup 6.1+) пропускает строку
; без ошибки; когда подпись появится, она попадёт в установку сама. Check()
; здесь НЕ помогает: ISCC проверяет существование Source на этапе
; предобработки, до запуска Check, и падает с PreprocessingError.
Source: "{#MrSourceRoot}\rules\rules.sig"; DestDir: "{app}\share\MrProper\rules"; \
  Flags: ignoreversion skipifsourcedoesntexist

; --- Документация ----------------------------------------------------------
Source: "{#MrSourceRoot}\README.md"; DestDir: "{app}\share\MrProper"; \
  Flags: ignoreversion
Source: "{#MrSourceRoot}\LICENSE"; DestDir: "{app}\share\MrProper"; \
  Flags: ignoreversion
Source: "{#MrSourceRoot}\docs\rules-authoring.md"; \
  DestDir: "{app}\share\MrProper\docs"; Flags: ignoreversion

[Run]
; postinstall — предложение, а не автозапуск: утилита чистит диск, и молча
; стартовать после установки не должен никто (SPEC §12).
Filename: "{app}\bin\mrproper.exe"; \
  Description: "{cm:LaunchProgram,{#MrAppName}}"; \
  Flags: nowait postinstall skipifsilent

; --- Удаление --------------------------------------------------------------
; Раздел [UninstallDelete] НАМЕРЕННО ПУСТ (требование J2 №5).
; Inno Setup удаляет ровно те файлы, которые сам установил, и ничего
; больше; корзина, отчёты и состояние правил лежат вне {app}, поэтому
; команда удаления до них не доходит даже теоретически. Любая будущая
; непустая строка здесь удалит пользовательские данные — это запрещено.
[UninstallDelete]

[Code]
{ ---------------------------------------------------------------------------
  Блоки кода нужны ради двух вещей, которых не умеет сам [Setup]:

    InitializeSetup      — первый экран установки: предупреждение SmartScreen
                           про «неизвестного издателя» (ADR-009, J2 №2).
    InitializeUninstall  — диалог удаления: корзина и отчёты ОСТАЮТСЯ
                           (J2 №5). Плюс CurUninstallStepChanged печатает,
                           где именно пользователь их найдёт.

  Два правила, без которых [Code] валит установку:
    * в InitializeSetup константа app ещё не инициализирована (пользователь
      не выбрал каталог), поэтому в этом диалоге каталог не упоминается: он и
      так стоит на следующей странице мастера. ExpandConstant на app здесь
      даёт фатальную ошибку — проверено;
    * оба вопроса задаются SuppressibleMsgBox, а не MsgBox: обычный MsgBox
      из [Code] ключом /SUPPRESSMSGBOXES НЕ подавляется (проверено: установка
      с /VERYSILENT висела на этом окне, код возврата не наступал).
      SuppressibleMsgBox в тихом режиме возвращает кнопку по умолчанию, а
      в обычном показывает окно, поэтому установка вручную и установка из
      CI работают обе;
    * ответ сравнивается с IDNO, а не с IDYES: подавленный диалог в тихом
      режиме возвращает значение по умолчанию, и «= IDYES» отменяло бы
      установку в пакетной сборке.
  --------------------------------------------------------------------------- }

function InitializeSetup(): Boolean;
var
  Notice: String;
begin
  Notice :=
    'MrProper не подписан цифровой подписью (ADR-009).' + #13#10 + #13#10 +
    'Windows SmartScreen покажет предупреждение и назовёт издателя' + #13#10 +
    '«Неизвестный издатель» (Unknown publisher). Это ожидаемо: сертификата' + #13#10 +
    'подписи нет, утилита личная, и повреждённой сборку это не делает.' + #13#10 + #13#10 +
    'Что нажать:' + #13#10 +
    '  1) на запрос UAC — «Да»;' + #13#10 +
    '  2) на экране SmartScreen — «Подробнее», затем «Выполнить в любом случае».' + #13#10 + #13#10 +
    'Продолжить установку?';
  Result := SuppressibleMsgBox(Notice, mbConfirmation, MB_YESNO, MB_DEFBUTTON1) <> IDNO;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Done: String;
begin
  if CurStep <> ssPostInstall then
    exit;
  Done :=
    'MrProper {#MrVersion} установлен.' + #13#10 + #13#10 +
    'Файлы программы:  ' + ExpandConstant('{app}') + #13#10 +
    'Правила:          ' + ExpandConstant('{app}\share\MrProper\rules') + #13#10 +
    'Ярлык:            меню «Пуск» → MrProper' + #13#10 +
    'Удаление:         «Программы и компоненты» → MrProper → Удалить' + #13#10 + #13#10 +
    'Удаление НЕ трогает ваши данные:' + #13#10 +
    '  %ProgramData%\MrProper\Trash      — корзина (можно восстановить)' + #13#10 +
    '  %LOCALAPPDATA%\MrProper\reports   — отчёты' + #13#10 + #13#10 +
    'Программа запускается с правами текущего пользователя (asInvoker):' + #13#10 +
    'запрос UAC будет только на операции, которым права нужны.';
  SuppressibleMsgBox(Done, mbInformation, MB_OK, 0);
end;

function InitializeUninstall(): Boolean;
var
  Notice: String;
begin
  Notice :=
    'Будут удалены только файлы программы из ' + ExpandConstant('{app}') + '.' + #13#10 + #13#10 +
    'ВАШИ ДАННЫЕ ОСТАЮТСЯ:' + #13#10 +
    '  %ProgramData%\MrProper\Trash    — корзина и её транзакции' + #13#10 +
    '  %LOCALAPPDATA%\MrProper\reports — отчёты очистки' + #13#10 +
    '  %LOCALAPPDATA%\MrProper\rules   — состояние набора правил' + #13#10 + #13#10 +
    'Удалить эти каталоги вручную можно будет только после удаления' + #13#10 +
    'программы. Продолжить удаление?';
  Result := SuppressibleMsgBox(Notice, mbConfirmation, MB_YESNO, MB_DEFBUTTON1) <> IDNO;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  SuppressibleMsgBox(
    'MrProper удалён. Данные не тронуты, они остались здесь:' + #13#10 + #13#10 +
    '  %ProgramData%\MrProper\Trash    — корзина' + #13#10 +
    '  %LOCALAPPDATA%\MrProper\reports — отчёты' + #13#10 + #13#10 +
    'Удалить их вручную: проводник → вставить путь → удалить.',
    mbInformation, MB_OK, 0);
end;
