@echo off
setlocal EnableExtensions
set "HOST=%~dp0"
set "PYTHONPATH=%HOST%;%PYTHONPATH%"
if "%HTTP_PORT%"=="" set "HTTP_PORT=8080"
if "%CTRL_PORT%"=="" set "CTRL_PORT=15000"
echo [watch] http://localhost:%HTTP_PORT%  control:%CTRL_PORT%

where py >nul 2>&1 && (
  py -3 -m rdbg watch --port %HTTP_PORT% --control-port %CTRL_PORT% %*
  exit /b %ERRORLEVEL%
)
where python >nul 2>&1 && (
  python -m rdbg watch --port %HTTP_PORT% --control-port %CTRL_PORT% %*
  exit /b %ERRORLEVEL%
)
where python3 >nul 2>&1 && (
  python3 -m rdbg watch --port %HTTP_PORT% --control-port %CTRL_PORT% %*
  exit /b %ERRORLEVEL%
)
echo [watch] Python 3 not found. Install Python and tick "Add python.exe to PATH".
exit /b 1
