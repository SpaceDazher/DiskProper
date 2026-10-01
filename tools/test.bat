@echo off
REM MrProper: запуск тестов.
REM   tools\test.bat <Debug|Release> [слот] [suite] [prefix]
REM     слот   — каталог сборки, тот же, что и в build.bat
REM     suite  — all (по умолчанию) | unit | integration | e2e
REM     prefix — фильтр проверок по префиксу имени через MRPROPER_TEST_FILTER
REM            (например walk_ или vfsEdge_): доказать свою часть одной командой,
REM            пока соседняя задача чинит свой слой.
REM
REM Гоняются ОБА набора: юнит-тесты ядра и интеграционные тесты Win32-слоя.
REM Раньше запускался только юит, из-за чего 12 интеграционных провалов были
REM не видны ни человеку, ни CI, а база «305/0» читалась как полная.
REM
REM e2e (сценарии на Pester) идёт отдельным набором и по умолчанию НЕ входит
REM в all: сценарии идут минутами, требуют Pester 4.x и по замыслу не могут
REM трогать реальные данные (ключ -Destructive не передаётся никогда).
REM Для них нужен путь к модулю Pester 4.x:
REM   set MRPROPER_PESTER_MODULE=D:\Tools\Pester\4.10.1\Pester.psd1
REM   tools\test.bat Debug main e2e
setlocal EnableExtensions
cd /d "%~dp0.."

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"
set "SLOT=%~2"
if "%SLOT%"=="" set "SLOT=main"
set "SUITE=%~3"
if "%SUITE%"=="" set "SUITE=all"
set "PREFIX=%~4"
if not "%PREFIX%"=="" set "MRPROPER_TEST_FILTER=%PREFIX%"

REM --- e2e: отдельный путь, свой тулчейн (Pester) и своя длительность ---------
if /i "%SUITE%"=="e2e" goto :run_e2e_suite

set "UNIT=build\%SLOT%\%CFG%\%CFG%\mrproper_unit_tests.exe"
if not exist "%UNIT%" set "UNIT=build\%SLOT%\%CFG%\mrproper_unit_tests.exe"
set "INTEG=build\%SLOT%\%CFG%\%CFG%\mrproper_integration_tests.exe"
if not exist "%INTEG%" set "INTEG=build\%SLOT%\%CFG%\mrproper_integration_tests.exe"

if not exist "%UNIT%" (
  echo [test] не найден %UNIT% — сначала tools\build.bat %CFG% %SLOT%
  exit /b 2
)

if /i "%SUITE%"=="integration" goto :integration
"%UNIT%"
set "RC=%errorlevel%"
if /i "%SUITE%"=="unit" exit /b %RC%

:integration
if not exist "%INTEG%" (
  echo [test] KRITICHNO: integration tests are not built: %INTEG%
  echo [test] Gates are incomplete: returning 3, not the unit suite code.
  echo [test] Otherwise one suite out of two looks green - defect D-58.
  exit /b 3
)

echo.
"%INTEG%"
if errorlevel 1 set "RC=1"
exit /b %RC%

:run_e2e_suite
if not defined MRPROPER_PESTER_MODULE (
  echo [test] e2e trebuet Pester 4.x. Zadayte put k modulyu, naprimer:
  echo        set MRPROPER_PESTER_MODULE=D:\Tools\Pester\4.10.1\Pester.psd1
  echo        tools\test.bat %CFG% %SLOT% e2e
  echo        Pester 3.4 iz korobki i Pester 5 ne godnyatsya: scenarii napisany
  echo        na formatah 4.x, sm. tests\CMakeLists.txt.
  exit /b 2
)
if not exist "%MRPROPER_PESTER_MODULE%" (
  echo [test] MRPROPER_PESTER_MODULE указывает на несуществующий файл: %MRPROPER_PESTER_MODULE%
  exit /b 2
)

REM --- поиск vcvars64.bat: сперва D:\BuildTools, затем vswhere, затем известные
REM --- пути (vswhere отдаёт пустоту при повреждённом каталоге экземпляров,
REM --- а компилятор жив).
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VARS="
if exist "D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VARS=D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VARS if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
      set "VARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
      goto :gotvars
    )
  )
)
if not defined VARS for %%P in (
  "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  "C:\Program Files\Microsoft Visual Studio\2022\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
) do (
  if not defined VARS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VARS=%%~P\VC\Auxiliary\Build\vcvars64.bat"
)
:gotvars
    )
  )
)
:gotvars
if not defined VARS (
  echo [test] КРИТИЧНО: не найден vcvars64.bat — C++ тулчейн не установлен.
  exit /b 9009
)
call "%VARS%" >nul

echo [test] e2e: Pester 4.x, klyuch -Destructive ne peredaetsya - realnye dannye ne trogaem
if defined MRPROPER_E2E_FILTER (
  ctest --test-dir "build\%SLOT%" -C %CFG% -L e2e -R "%MRPROPER_E2E_FILTER%" --output-on-failure
) else (
  ctest --test-dir "build\%SLOT%" -C %CFG% -L e2e --output-on-failure
)
exit /b %errorlevel%
