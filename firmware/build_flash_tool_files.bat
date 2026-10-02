@echo off
setlocal
cd /d "%~dp0"

set /p VERSION=<version.txt
set "BUILD=build"
set "DST=..\release\flash_download_tool"

echo ================================================
echo  Build Flash Download Tool files (v%VERSION%)
echo ================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf_build.ps1" -Release
if errorlevel 1 (
    echo.
    echo [FAIL] Build failed.
    pause
    exit /b 1
)

rem Publication, hashes and README are validated by idf_build.ps1 -Release.

echo.
echo [OK] Files written to %DST%:
echo   bootloader.bin        @ 0x00000
echo   partitions.bin        @ 0x08000
echo   ota_data_initial.bin  @ otadata offset (see README.txt)
echo   firmware.bin          @ ota_0 offset (see README.txt)
echo.
echo Offsets and the current partition table: %DST%\README.txt
pause
endlocal
