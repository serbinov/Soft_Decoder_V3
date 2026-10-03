@echo off
rem Launch the local AURA-X decoder web preview (real UI + mock firmware API).
setlocal
if "%PREVIEW_PORT%"=="" set PREVIEW_PORT=8080
start "AURA-X preview server" cmd /k python "%~dp0mock_server.py"
timeout /t 2 >nul
start "" http://127.0.0.1:%PREVIEW_PORT%/
endlocal
