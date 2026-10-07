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
if not exist "build\tests" mkdir build\tests

set CFLAGS=/c /O2 /EHsc /utf-8 /MD /std:c++17 /DUNICODE /D_UNICODE

echo [1/6] Compiling md5.cpp...
cl %CFLAGS% src\md5.cpp /Fo:build\md5.obj >nul 2>&1
if errorlevel 1 (
    echo ERROR: md5.cpp failed
    cl %CFLAGS% src\md5.cpp /Fo:build\md5.obj
    pause & exit /b 1
)

echo [2/6] Compiling scanner.cpp...
cl %CFLAGS% src\scanner.cpp /Fo:build\scanner.obj >nul 2>&1
if errorlevel 1 (
    echo ERROR: scanner.cpp failed
    cl %CFLAGS% src\scanner.cpp /Fo:build\scanner.obj
    pause & exit /b 1
)

echo [3/6] Compiling image_preview.cpp...
cl %CFLAGS% src\image_preview.cpp /Fo:build\image_preview.obj
if errorlevel 1 (
    echo ERROR: image_preview.cpp failed
    pause & exit /b 1
)

echo [4/6] Compiling resource.rc...
rc /i src /fo build\resource.res src\resource.rc >nul 2>&1
if errorlevel 1 (
    echo ERROR: resource.rc failed
    rc /i src /fo build\resource.res src\resource.rc
    pause & exit /b 1
)

echo [5/6] Compiling main.cpp...
cl %CFLAGS% src\main.cpp /Fo:build\main.obj >nul 2>&1
if errorlevel 1 (
    echo ERROR: main.cpp failed
    cl %CFLAGS% src\main.cpp /Fo:build\main.obj
    pause & exit /b 1
)

echo [6/6] Linking dupRemover.exe...
cl build\main.obj build\md5.obj build\scanner.obj build\image_preview.obj build\resource.res ^
    /Fe:bin\dupRemover.exe /link /SUBSYSTEM:WINDOWS ^
    advapi32.lib comctl32.lib shell32.lib ole32.lib user32.lib gdi32.lib uxtheme.lib windowscodecs.lib >nul 2>&1
if errorlevel 1 (
    echo ERROR: Link failed
    cl build\main.obj build\md5.obj build\scanner.obj build\image_preview.obj build\resource.res /Fe:bin\dupRemover.exe /link /SUBSYSTEM:WINDOWS advapi32.lib comctl32.lib shell32.lib ole32.lib user32.lib gdi32.lib uxtheme.lib windowscodecs.lib
    pause & exit /b 1
)

echo.
echo [OK] Build complete: bin\dupRemover.exe
echo Use the Register button in the app to add the Explorer context menu.

echo.
echo === Building console tests ===
if not exist "test" mkdir test 2>nul

cl %CFLAGS% test\test_md5.cpp /Fo:build\tests\test_md5.obj >nul 2>&1
if errorlevel 1 (
    echo WARNING: test_md5 build failed (non-critical)
) else (
    cl build\tests\test_md5.obj build\md5.obj build\scanner.obj ^
        /Fe:test\test_md5.exe /link advapi32.lib shell32.lib >nul 2>&1
)
if errorlevel 1 (
    echo WARNING: test_md5 build failed (non-critical)
) else (
    echo [OK] test\test_md5.exe
)

cl %CFLAGS% test\test_vectors.cpp /Fo:build\tests\test_vectors.obj >nul 2>&1
if errorlevel 1 (
    echo WARNING: test_vectors build failed (non-critical)
) else (
    cl build\tests\test_vectors.obj build\md5.obj ^
        /Fe:test\test_vectors.exe >nul 2>&1
)
if errorlevel 1 (
    echo WARNING: test_vectors build failed (non-critical)
) else (
    echo [OK] test\test_vectors.exe
)

cl %CFLAGS% test\test_scanner.cpp /Fo:build\tests\test_scanner.obj >nul 2>&1
if errorlevel 1 (
    echo WARNING: test_scanner build failed (non-critical)
) else (
    cl build\tests\test_scanner.obj build\md5.obj build\scanner.obj ^
        /Fe:test\test_scanner.exe /link advapi32.lib shell32.lib >nul 2>&1
)
if errorlevel 1 (
    echo WARNING: test_scanner build failed (non-critical)
) else (
    echo [OK] test\test_scanner.exe
)

echo.
pause
