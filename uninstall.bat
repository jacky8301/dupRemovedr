@echo off
setlocal

set "REG_KEY=HKEY_CLASSES_ROOT\Directory\shell\FindDupFiles"

echo === Duplicate File Cleaner - Uninstall ===
echo.

reg delete "%REG_KEY%" /f >nul 2>&1
if errorlevel 1 (
    echo [INFO] Already removed or not installed.
) else (
    echo [OK] Context menu removed.
)

echo.
pause
