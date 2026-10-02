@echo off
setlocal
set "OUT=%~1"
set "MRPROPER_SCAN_PROFILE=%OUT%"
echo [%OUT%] start %TIME%
build\a4\Debug\mrproper_cli.exe scan --json --quiet --rules rules > "%OUT%.json" 2> "%OUT%.err"
echo [%OUT%] scan exit %ERRORLEVEL%
echo [%OUT%] done %TIME%
endlocal
