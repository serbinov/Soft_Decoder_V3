@echo off
setlocal
cd /d "%~dp0"

echo ================================================
echo  Bump firmware version (+1 minor)
echo ================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\bump_version.ps1"
if errorlevel 1 (
    echo.
    echo [FAIL] Version bump failed.
    pause
    exit /b 1
)

echo.
echo Done. Next: run build_ota_bin.bat to produce the new .bin
pause
endlocal
