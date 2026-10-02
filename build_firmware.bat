@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto build
if /i "%~1"=="--no-pause" goto build
echo Usage: %~nx0 [--no-pause]
exit /b 2

:build
set "BUILD_RESULT=1"
if not exist "%~dp0firmware\idf_build.ps1" (
    echo [FAIL] Build wrapper not found: firmware\idf_build.ps1
    goto finish
)
rem Build only: no arguments enabling flash, erase or serial monitor.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0firmware\idf_build.ps1" -Release
set "BUILD_RESULT=%ERRORLEVEL%"
if not "%BUILD_RESULT%"=="0" (
    echo [FAIL] Firmware build failed. See the output above.
    goto finish
)
if not exist "%~dp0release\flash_download_tool\firmware.bin" (
    echo [FAIL] Build finished without the expected release firmware.
    set "BUILD_RESULT=1"
    goto finish
)
echo.
echo [OK] Firmware built, packaged and BIN integrity verified.
echo Release: "%~dp0release"
echo Flasher files: "%~dp0release\flash_download_tool"
echo Image identity and hashes: "%~dp0release\manifest.json"

:finish
if "%~1"=="" pause
exit /b %BUILD_RESULT%
