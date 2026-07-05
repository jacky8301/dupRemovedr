@echo off
cd /d "%~dp0"

call "D:\Software\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Cannot find VS 2022. Edit vcvars path in build.bat.
    pause
    exit /b 1
)

echo === Building Duplicate File Cleaner ===
echo.

if not exist "build" mkdir build
if not exist "bin" mkdir bin

echo [1/4] Compiling md5.cpp...
set CFLAGS=/c /O2 /EHsc /utf-8 /MD /std:c++17 /DUNICODE /D_UNICODE

echo [1/4] Compiling md5.cpp...
cl %CFLAGS% src\md5.cpp /Fo:build\md5.obj >nul 2>&1
if errorlevel 1 (
    echo ERROR: md5.cpp failed
    cl %CFLAGS% src\md5.cpp /Fo:build\md5.obj
    pause & exit /b 1
)

echo [2/4] Compiling scanner.cpp...
cl %CFLAGS% src\scanner.cpp /Fo:build\scanner.obj >nul 2>&1
if errorlevel 1 (
    echo ERROR: scanner.cpp failed
    cl %CFLAGS% src\scanner.cpp /Fo:build\scanner.obj
    pause & exit /b 1
)

echo [3/4] Compiling resource.rc...
rc /i src /fo build\resource.res src\resource.rc >nul 2>&1
if errorlevel 1 (
    echo ERROR: resource.rc failed
    rc /i src /fo build\resource.res src\resource.rc
    pause & exit /b 1
)

echo [4/4] Compiling main.cpp and linking...
cl /O2 /EHsc /utf-8 /MD /std:c++17 /DUNICODE /D_UNICODE ^
    src\main.cpp build\md5.obj build\scanner.obj build\resource.res ^
    /Fe:bin\dupRemover.exe ^
    /link advapi32.lib comctl32.lib shell32.lib ole32.lib user32.lib gdi32.lib >nul 2>&1
if errorlevel 1 (
    echo ERROR: Link failed
    cl /O2 /EHsc /utf-8 /MD /std:c++17 /DUNICODE /D_UNICODE src\main.cpp build\md5.obj build\scanner.obj build\resource.res /Fe:bin\dupRemover.exe /link advapi32.lib comctl32.lib shell32.lib ole32.lib user32.lib gdi32.lib
    pause & exit /b 1
)

echo.
echo [OK] Build complete: bin\dupRemover.exe
echo Run install.bat AS ADMIN to register the context menu.

echo.
echo === Building console tests ===
if not exist "test" mkdir test 2>nul

cl /O2 /EHsc /utf-8 /MD /std:c++17 /DUNICODE /D_UNICODE ^
    test\test_md5.cpp src\md5.cpp src\scanner.cpp ^
    /Fe:test\test_md5.exe ^
    /link advapi32.lib shell32.lib >nul 2>&1
if errorlevel 1 (
    echo WARNING: test_md5 build failed (non-critical)
) else (
    echo [OK] test\test_md5.exe
)

cl /O2 /EHsc /utf-8 /MD /std:c++17 /DUNICODE /D_UNICODE ^
    test\test_vectors.cpp src\md5.cpp ^
    /Fe:test\test_vectors.exe >nul 2>&1
if errorlevel 1 (
    echo WARNING: test_vectors build failed (non-critical)
) else (
    echo [OK] test\test_vectors.exe
)

echo.
pause
