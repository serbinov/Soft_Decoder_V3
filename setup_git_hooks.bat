@echo off
REM Enable the versioned git hooks (.githooks). Run once after cloning.
cd /d "%~dp0"
git config core.hooksPath .githooks
if errorlevel 1 (
    echo [FAIL] could not set core.hooksPath
    pause
    exit /b 1
)
echo [OK] git hooks enabled: core.hooksPath = .githooks
echo      every commit now records a line in CHANGELOG.md automatically.
pause
