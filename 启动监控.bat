@echo off
rem TopVideoMonitor launcher - starts the always-on-top bar without a console window.
setlocal
cd /d "%~dp0"

where pyw.exe >nul 2>nul
if not errorlevel 1 (
    start "" pyw.exe -3 "%~dp0main.py"
    goto :eof
)

where pythonw.exe >nul 2>nul
if not errorlevel 1 (
    start "" pythonw.exe "%~dp0main.py"
    goto :eof
)

start "" python.exe "%~dp0main.py"
