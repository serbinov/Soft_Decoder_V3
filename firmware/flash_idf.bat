@echo off
rem Native ESP-IDF build + flash (no PlatformIO). Extra args are forwarded, e.g.:
rem   flash_idf.bat -Port COM7 -Monitor
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf_build.ps1" -Flash %*
exit /b %errorlevel%
