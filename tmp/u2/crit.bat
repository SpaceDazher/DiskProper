@echo off
cd /d D:\Project\MrProper
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
echo === run %1 of %2, %3 ===
call tools\test.bat Debug u2
echo CRITERION_EXIT=%errorlevel%
