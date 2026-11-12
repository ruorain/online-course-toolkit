@echo off
rem ============================================================
rem  TopVideoMonitor build script (MinGW-w64 g++ / C++17)
rem  Output: TopVideoMonitor.exe (single file) + probe.exe (self-check CLI)
rem ============================================================
setlocal
cd /d "%~dp0"

set CXX=g++
where g++ >nul 2>nul
if not errorlevel 1 goto havecxx
if not exist "C:\msys64\mingw64\bin\g++.exe" goto nocxx
set CXX=C:\msys64\mingw64\bin\g++.exe
:havecxx

set RC=windres
where windres >nul 2>nul
if not errorlevel 1 goto haverc
if exist "C:\msys64\mingw64\bin\windres.exe" set RC=C:\msys64\mingw64\bin\windres.exe
:haverc

if not exist build mkdir build

echo [1/3] compiling resources ...
"%RC%" src\app.rc -O coff -o build\app_res.o
if errorlevel 1 goto err

echo [2/3] compiling TopVideoMonitor.exe ...
%CXX% -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -finput-charset=UTF-8 -mwindows ^
    -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type ^
    src\main.cpp src\json.cpp src\ws.cpp src\cdp.cpp src\util.cpp src\config.cpp ^
    src\monitor.cpp src\sound.cpp src\ui.cpp src\settings.cpp ^
    build\app_res.o -o TopVideoMonitor.exe ^
    -lws2_32 -lole32 -luuid -lcomctl32 -lgdi32 -luser32 -lshell32 -static -s
if errorlevel 1 goto err

echo [3/3] compiling probe.exe ...
%CXX% -std=c++17 -O2 -DUNICODE -D_UNICODE -finput-charset=UTF-8 ^
    tools\probe.cpp src\json.cpp src\ws.cpp src\cdp.cpp src\util.cpp ^
    -o probe.exe -lws2_32 -lole32 -luuid -static -s
if errorlevel 1 goto err

echo.
echo Build OK: TopVideoMonitor.exe  probe.exe
exit /b 0

:nocxx
echo [x] g++ not found. Install MinGW-w64 first.
exit /b 1

:err
echo.
echo [x] Build failed.
exit /b 1
