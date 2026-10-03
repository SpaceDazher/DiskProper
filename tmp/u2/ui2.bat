@echo off
cd /d D:\Project\MrProper
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-smoke.ps1 %* > "%UILOG%" 2>&1
exit /b %errorlevel%
