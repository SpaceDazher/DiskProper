@echo off
cd /d D:\Project\MrProper
call tools\build.bat Debug u2 > D:\Project\MrProper\tmp\u2\build3.log 2>&1
echo BUILD_EXIT=%errorlevel% >> D:\Project\MrProper\tmp\u2\build3.log
