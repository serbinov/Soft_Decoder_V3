@echo off
rem Starts the local control panel (built-in PowerShell, no install needed) and
rem opens Chrome/Edge. The actual flashing is done by the project scripts.
rem The server stops by itself when the web page is closed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_flasher.ps1" %*
if errorlevel 1 pause
