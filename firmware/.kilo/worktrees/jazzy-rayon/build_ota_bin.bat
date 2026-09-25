@echo off
setlocal
cd /d "%~dp0"

set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if not exist "%PIO%" (
    echo [ERROR] PlatformIO not found: %PIO%
    pause
    exit /b 1
)

set /p VERSION=<version.txt
set "SRC=.pio\build\esp32-s3-devkitc-1\firmware.bin"
set "DST=..\release\ADDITIPUS_AURA-X_v%VERSION%.bin"

echo ================================================
echo  Build OTA firmware v%VERSION%
echo ================================================
"%PIO%" run -e esp32-s3-devkitc-1
if errorlevel 1 (
    echo.
    echo [FAIL] Build failed.
    pause
    exit /b 1
)

if not exist "%SRC%" (
    echo [ERROR] firmware.bin not found: %SRC%
    pause
    exit /b 1
)

if not exist "..\release" mkdir "..\release"

copy /Y "%SRC%" "%DST%" >nul
if errorlevel 1 (
    echo [FAIL] Copy failed.
    pause
    exit /b 1
)

echo.
echo [OK] OTA bin created: %DST%
pause
endlocal
