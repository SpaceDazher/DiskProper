@echo off
set "TEMP=D:\Temp\u2tmp"
set "TMP=D:\Temp\u2tmp"
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Project\MrProper\tmp\u2_probe_alloc.ps1" -Runs 3 -Root "D:\Temp\u2alloc" -Handle
