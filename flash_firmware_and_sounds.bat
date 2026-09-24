@echo off
setlocal
cd /d "%~dp0"

set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if not exist "%PIO%" (
    echo [ERROR] PlatformIO not found: %PIO%
    pause
    exit /b 1
)

rem Usage: flash_firmware_and_sounds.bat [COMx] [erase]
rem   erase  = full internal-chip erase first (wipes NVS: settings, CVs,
rem            sound labels, Wi-Fi). Use once when migrating an old layout.
rem   no arg = keep internal NVS/settings.
rem Sounds on the external W25Q128 are always (re)written by this script.
set "PORT="
set "DO_ERASE=0"
for %%a in (%*) do (
    if /I "%%a"=="erase" (set "DO_ERASE=1") else (set "PORT=%%a")
)
if not "%PORT%"=="" goto :port_ok

rem Auto-detect the Espressif USB-Serial-JTAG port (VID_303A/PID_1001).
for /f "usebackq delims=" %%i in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like 'USB\VID_303A*PID_1001*' -and $_.Name -match '\((COM\d+)\)' -and [System.IO.Ports.SerialPort]::GetPortNames() -contains $Matches[1] } | ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } }"`) do set "PORT=%%i"
if not "%PORT%"=="" (
    echo [INFO] Detected ESP32-S3 on %PORT%
    goto :port_ok
)

set "PORT=COM3"
:port_ok

echo ================================================================
echo  Flash firmware + write sounds to the external flash (%PORT%)
if "%DO_ERASE%"=="1" (
    echo  MODE: internal full erase ^(NVS/settings cleared^)
) else (
    echo  MODE: keep internal settings ^(NVS preserved^)
)
echo  External flash ^(W25Q128/sounds^) is always re-written.
echo ================================================================

if "%DO_ERASE%"=="1" goto :do_erase
goto :flash

:do_erase
echo [1/3] Erasing internal flash (NVS/settings will be cleared)...
"%PIO%" run -e esp32-s3-devkitc-1 -t erase --upload-port %PORT%
if errorlevel 1 (
    echo.
    echo [FAIL] Erase failed.
    pause
    exit /b 1
)
goto :flash

:flash
echo [2/3] Writing firmware...
"%PIO%" run -e esp32-s3-devkitc-1 -t upload --upload-port %PORT%
if errorlevel 1 (
    echo.
    echo [FAIL] Firmware flash failed.
    pause
    exit /b 1
)

echo.
echo [3/3] Uploading sounds (keep the board connected)...
powershell -ExecutionPolicy Bypass -File "%~dp0provision_sounds.ps1" -Port %PORT%
if errorlevel 1 (
    echo.
    echo [FAIL] Sound upload failed.
) else (
    echo.
    echo [OK] Sounds uploaded. The decoder restarts with the sounds ready.
)
pause
endlocal
