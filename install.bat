@echo off
setlocal
cd /d "%~dp0"

set "EXE_PATH=%~dp0bin\dupRemover.exe"
set "REG_KEY=HKEY_CLASSES_ROOT\Directory\shell\FindDupFiles"

echo === Duplicate File Cleaner - Install Context Menu ===
echo.

if not exist "%EXE_PATH%" (
    echo [ERROR] dupRemover.exe not found at: %EXE_PATH%
    echo Please build first: cmake -B build ^&^& cmake --build build --config Release
    pause
    exit /b 1
)

echo Registering context menu...
reg add "%REG_KEY%" /ve /d "Find Duplicate Files" /f >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Registry write failed. Run as Administrator.
    pause
    exit /b 1
)
reg add "%REG_KEY%" /v "Icon" /t REG_SZ /d "%EXE_PATH%,0" /f >nul 2>&1
reg add "%REG_KEY%\command" /ve /d "\"%EXE_PATH%\" \"%%1\"" /f >nul 2>&1

echo.
echo [OK] Installed. Right-click any folder -^> "Find Duplicate Files"
echo.
pause
