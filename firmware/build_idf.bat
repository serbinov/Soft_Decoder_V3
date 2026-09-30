@echo off
rem Native ESP-IDF build (no PlatformIO). Extra args are forwarded, e.g.:
rem   build_idf.bat -Flash -Port COM7
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0idf_build.ps1" %*
exit /b %errorlevel%
