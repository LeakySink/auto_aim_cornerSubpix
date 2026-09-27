@echo off
REM Compatibility: open hub portal (Calibrate is a feature on the home page)
setlocal EnableExtensions
set "HOST=%~dp0"
set "PYTHONPATH=%HOST%;%PYTHONPATH%"
if "%HTTP_PORT%"=="" set "HTTP_PORT=8080"
echo [calibrate] redirecting to hub portal (use host\watch.bat or python -m rdbg serve, then Calibrate)

where py >nul 2>&1 && (
  py -3 -m rdbg calibrate --port %HTTP_PORT% %*
  exit /b %ERRORLEVEL%
)
where python >nul 2>&1 && (
  python -m rdbg calibrate --port %HTTP_PORT% %*
  exit /b %ERRORLEVEL%
)
where python3 >nul 2>&1 && (
  python3 -m rdbg calibrate --port %HTTP_PORT% %*
  exit /b %ERRORLEVEL%
)
echo [calibrate] Python 3 not found. Install Python and tick "Add python.exe to PATH".
exit /b 1
