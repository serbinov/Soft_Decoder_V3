@echo off
setlocal
cd /d "%~dp0"

set /p VERSION=<version.txt
set "BUILD=build"
set "DST=..\release\flash_download_tool"

echo ================================================
echo  Build Flash Download Tool files (v%VERSION%)
echo ================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf_build.ps1"
if errorlevel 1 (
    echo.
    echo [FAIL] Build failed.
    pause
    exit /b 1
)

if not exist "%DST%" mkdir "%DST%"

copy /Y "%BUILD%\bootloader\bootloader.bin"             "%DST%\bootloader.bin"       >nul
copy /Y "%BUILD%\partition_table\partition-table.bin"   "%DST%\partitions.bin"       >nul
copy /Y "%BUILD%\ota_data_initial.bin"                  "%DST%\ota_data_initial.bin" >nul
copy /Y "%BUILD%\soft_decoder_v3.bin"                   "%DST%\firmware.bin"         >nul

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
echo   ota_data_initial.bin  @ otadata offset (see README.txt)
echo   firmware.bin          @ ota_0 offset (see README.txt)
echo.
echo Offsets and the current partition table: %DST%\README.txt
pause
endlocal
