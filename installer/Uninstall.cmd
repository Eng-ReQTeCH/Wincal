@echo off
setlocal
set "ROOT=%LOCALAPPDATA%\Win11CalendarFix"
if "%~1"=="--cleanup" (
    start "" /wait "%ROOT%\Setup.exe" --uninstall --cleanup
) else (
    start "" /wait "%ROOT%\Setup.exe" --uninstall
)
if errorlevel 1 exit /b %errorlevel%
del /q "%ROOT%\Setup.exe"
if exist "%ROOT%\Setup.exe" exit /b 1
(goto) 2>nul & del /q "%ROOT%\Uninstall.cmd" & rd "%ROOT%" 2>nul & exit /b 0
