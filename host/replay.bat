@echo off
setlocal EnableExtensions
set "HOST=%~dp0"
set "PYTHONPATH=%HOST%;%PYTHONPATH%"
if "%~1"=="" goto usage
if "%~1"=="-h" goto usage
if "%~1"=="--help" goto usage
echo [replay] %~1

where py >nul 2>&1 && (
  py -3 -m rdbg replay %*
  exit /b %ERRORLEVEL%
)
where python >nul 2>&1 && (
  python -m rdbg replay %*
  exit /b %ERRORLEVEL%
)
where python3 >nul 2>&1 && (
  python3 -m rdbg replay %*
  exit /b %ERRORLEVEL%
)
echo [replay] Python 3 not found. Install Python and tick "Add python.exe to PATH".
exit /b 1
:usage
echo Usage: %~nx0 ^<file.rlog^> [--port P] [--host IP] [--max-mb N] [--no-browser]
exit /b 1
