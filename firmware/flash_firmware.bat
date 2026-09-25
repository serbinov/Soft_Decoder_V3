@echo off
setlocal
cd /d "%~dp0"

set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if not exist "%PIO%" (
    echo [ERROR] PlatformIO not found: %PIO%
    pause
    exit /b 1
)

rem Usage: flash_firmware.bat [COMx] [erase]
rem   erase  = full chip erase first (wipes NVS: settings, CVs, sound labels,
rem            Wi-Fi). Use it once when migrating an old partition layout.
rem   no arg = keep NVS/settings; just write bootloader + partitions + app.
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

echo ================================================
echo  Flash ADDITIPUS AURA-X decoder via %PORT%
if "%DO_ERASE%"=="1" (
    echo  MODE: full chip erase, NVS/settings WILL be cleared
) else (
    echo  MODE: keep settings, NVS/settings preserved
)
echo  (bootloader + partition table + ota_data + app)
echo ================================================

if "%DO_ERASE%"=="1" goto :do_erase
goto :flash

:do_erase
echo [1/2] Erasing flash (NVS/settings will be cleared)...
"%PIO%" run -e esp32-s3-devkitc-1 -t erase --upload-port %PORT%
if errorlevel 1 (
    echo.
    echo [FAIL] Erase failed. Check the COM port and board connection.
    pause
    exit /b 1
)
goto :flash

:flash
echo Writing firmware...
"%PIO%" run -e esp32-s3-devkitc-1 -t upload --upload-port %PORT%

if errorlevel 1 (
    echo.
    echo [FAIL] Flash failed. Check the COM port and board connection.
) else (
    echo.
    echo [OK] Firmware flashed to %PORT%.
)
pause
endlocal
