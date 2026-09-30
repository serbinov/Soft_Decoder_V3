@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem Usage: flash_firmware.bat [COMx] [erase]
rem   erase  = full chip erase first (wipes NVS: settings, CVs, sound labels, Wi-Fi).
rem   no arg = keep NVS/settings; just write bootloader + partitions + app.
rem Builds with ESP-IDF 6.0 (idf.py) and flashes via idf_build.ps1.
set "ARGS="
for %%a in (%*) do (
    if /I "%%a"=="erase" (set "ARGS=!ARGS! -Erase") else (set "ARGS=!ARGS! -Port %%a")
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf_build.ps1" -Flash !ARGS!
pause
endlocal
