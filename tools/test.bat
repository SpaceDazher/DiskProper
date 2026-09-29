@echo off
REM MrProper: запуск тестов.
REM   tools\test.bat <Debug|Release> [слот] [suite] [prefix]
REM     слот   — каталог сборки, тот же, что и в build.bat
REM     suite  — all (по умолчанию) | unit | integration
REM     prefix — фильтр проверок по префиксу имени через MRPROPER_TEST_FILTER
REM            (например walk_ или vfsEdge_): доказать свою часть одной командой,
REM            пока соседняя задача чинит свой слой.
REM Гоняются ОБА набора: юнит-тесты ядра и интеграционные тесты Win32-слоя.
REM Раньше запускался только юит, из-за чего 12 интеграционных провалов были
REM не видны ни человеку, ни CI, а база «305/0» читалась как полная.
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
  echo [test] интеграционные тесты не собраны: %INTEG% пропущен
  exit /b %RC%
)

echo.
"%INTEG%"
if errorlevel 1 set "RC=1"
exit /b %RC%
