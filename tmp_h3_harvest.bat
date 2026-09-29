@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "N=%1"
set "TAG=%2"
set "OUT=%TEMP%\h3-codes-%TAG%.txt"
if exist "%OUT%" del /q "%OUT%"
for /l %%i in (1,1,%N%) do (
  D:\Project\MrProper\build\a3\Debug\mrproper_cli.exe scan --json --rules %TEMP%\mrproper-h3-rules --quiet >nul 2>nul
  echo %%i=!errorlevel!>> "%OUT%"
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$c=Get-Content '%OUT%'; $codes=$c | ForEach-Object { ($_ -split '=')[1] }; $g=$codes | Group-Object | Sort-Object Count -Descending; $g | ForEach-Object { 'CODE ' + $_.Name + ' x' + $_.Count }; 'TOTAL ' + $codes.Count"
