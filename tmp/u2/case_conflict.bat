@echo off
cd /d D:\Project\MrProper
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-smoke.ps1 -Page disks -ExpectPage settings > D:\Project\MrProper\tmp\u2\conflict.log 2>&1
exit /b %errorlevel%
