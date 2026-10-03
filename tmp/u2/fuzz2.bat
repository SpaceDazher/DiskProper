@echo off
setlocal enabledelayedexpansion
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
set "MRPROPER_TEST_FILTER=vfsEdge_longPath_tree_beyond_max_path_is_visible"
set /a bad=0
set /a skip=0
for /L %%i in (1,1,%2) do (
  "%~1" > "D:\Temp\u2tmp\fz.log" 2>&1
  if errorlevel 1 (
    set /a bad+=1
    copy /y "D:\Temp\u2tmp\fz.log" "D:\Temp\u2tmp\fz_bad_%%i.log" >nul
  )
  findstr /C:"[skip]" "D:\Temp\u2tmp\fz.log" >nul && set /a skip+=1
)
echo [u2] BIN=%~1 runs=%2 RED=%bad% SKIP=%skip%
