@echo off
rem Build a standalone exe (no Python needed on the target machine).
rem Output: dist\TopVideoMonitor.exe
setlocal
cd /d "%~dp0"

set PY=py -3
where py >nul 2>nul
if errorlevel 1 set PY=python

%PY% -m pip install --quiet --disable-pip-version-check pyinstaller
if errorlevel 1 goto err

%PY% -m PyInstaller --noconfirm --clean --onefile --noconsole --name TopVideoMonitor ^
    --icon app.ico ^
    --add-data "????.html;." --add-data "????.html;." ^
    main.py
if errorlevel 1 goto err

echo.
echo Build OK: dist\TopVideoMonitor.exe
exit /b 0

:err
echo.
echo [x] Build failed.
exit /b 1
