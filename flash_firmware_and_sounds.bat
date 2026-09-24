@echo off
setlocal
cd /d "%~dp0"

set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if not exist "%PIO%" (
    echo [ERROR] PlatformIO not found: %PIO%
    pause
    exit /b 1
)

set "PORT=%~1"
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
echo  WARNING: the external flash will be erased and re-written.
echo ================================================================

"%PIO%" run -e esp32-s3-devkitc-1 -t upload --upload-port %PORT%
if errorlevel 1 (
    echo.
    echo [FAIL] Firmware flash failed.
    pause
    exit /b 1
)

echo.
echo [OK] Firmware flashed. Uploading sounds (keep the board connected)...
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
