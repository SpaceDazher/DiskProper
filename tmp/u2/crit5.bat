@echo off
cd /d D:\Project\MrProper
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
for %%i in (1 2 3 4 5) do (
  echo ===== RUN %%i =====
  call tools\test.bat Debug u2 > "D:\Project\MrProper\tmp\u2\crit_run%%i.log" 2>&1
  echo RUN%%i_EXIT=%errorlevel%
)
