@echo off
REM MrProper: zapusk e2e-scenariev na Pester (SPEC 11 p.3, 12.3).
REM   tools\run-e2e.bat [Debug|Release] [slot] [filtr-ctest]
REM Tretiy argument - filtr imen testov CTest, chtoby gonyat odin scenarii:
REM   tools\run-e2e.bat Debug main e2e_UndoRestore
REM Scenarii napisany na Pester 4.x: vstroennyy v Windows PowerShell 5.1 Pester
REM 3.4.0 i Pester 5 ne podhodyat, sm. tests\CMakeLists.txt. Put k Pester 4.x:
REM   set MRPROPER_PESTER_MODULE=D:\Tools\Pester\4.10.1\Pester.psd1
REM Klyuch -Destructive ne peredaetsya NIKOGDA: na rabochei mashine eto realnyy
REM kesh polzovatelya, a ne testovye dannye.
setlocal EnableExtensions
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"
set "SLOT=%~2"
if "%SLOT%"=="" set "SLOT=main"
set "FILTER=%~3"
if defined FILTER set "MRPROPER_E2E_FILTER=%FILTER%"
if not defined MRPROPER_PESTER_MODULE set "MRPROPER_PESTER_MODULE=D:\Tools\Pester\4.10.1\Pester.psd1"
call "%~dp0test.bat" %CFG% %SLOT% e2e
exit /b %errorlevel%
