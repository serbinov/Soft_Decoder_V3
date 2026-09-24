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
set "BUILD=.pio\build\esp32-s3-devkitc-1"
set "DST=..\release\flash_download_tool"

echo ================================================
echo  Build Flash Download Tool files (v%VERSION%)
echo ================================================
"%PIO%" run -e esp32-s3-devkitc-1
if errorlevel 1 (
    echo.
    echo [FAIL] Build failed.
    pause
    exit /b 1
)

if not exist "%DST%" mkdir "%DST%"

copy /Y "%BUILD%\bootloader.bin"       "%DST%\bootloader.bin"       >nul
copy /Y "%BUILD%\partitions.bin"       "%DST%\partitions.bin"       >nul
copy /Y "%BUILD%\ota_data_initial.bin" "%DST%\ota_data_initial.bin" >nul
copy /Y "%BUILD%\firmware.bin"         "%DST%\firmware.bin"         >nul

if errorlevel 1 (
    echo [FAIL] Copy failed.
    pause
    exit /b 1
)

rem Regenerate README.txt from the current partition table (partitions.csv) so the
rem documented offsets/sizes never go stale after a partition-table change.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gen_flash_readme.ps1" -Out "%DST%\README.txt"

echo.
echo [OK] Files written to %DST%:
echo   bootloader.bin        @ 0x00000
echo   partitions.bin        @ 0x08000
echo   ota_data_initial.bin  @ 0x10000
echo   firmware.bin          @ ota_0 offset (see README.txt)
echo.
echo Offsets and the current partition table: %DST%\README.txt
pause
endlocal
