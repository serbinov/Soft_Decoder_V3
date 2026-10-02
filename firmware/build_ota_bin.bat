@echo off
setlocal
cd /d "%~dp0"

set /p VERSION=<version.txt
set "SRC=build\soft_decoder_v3.bin"
set "DST=..\release\ADDITIPUS_AURA-X_v%VERSION%.bin"

echo ================================================
echo  Build OTA firmware v%VERSION%
echo ================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf_build.ps1" -Release
if errorlevel 1 (
    echo.
    echo [FAIL] Build failed.
    pause
    exit /b 1
)

if not exist "%SRC%" (
    echo [ERROR] firmware image not found: %SRC%
    pause
    exit /b 1
)

rem The versioned raw/OTA copies are published and checked by -Release.

echo.
echo [OK] OTA bin created: %DST%
pause
endlocal
