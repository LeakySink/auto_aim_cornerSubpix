@echo off
setlocal EnableExtensions
set "HOST=%~dp0"
set "PYTHONPATH=%HOST%;%PYTHONPATH%"
if "%HTTP_PORT%"=="" set "HTTP_PORT=8080"
if "%DATA_PORT%"=="" set "DATA_PORT=15001"
if "%PEER_PORT%"=="" set "PEER_PORT=15100"
if "%DISCOVER_PORT%"=="" set "DISCOVER_PORT=15999"
echo [watch] http://localhost:%HTTP_PORT%  data:%DATA_PORT% peer:%PEER_PORT%

where py >nul 2>&1 && (
  py -3 -m rdbg watch --port %HTTP_PORT% --data-port %DATA_PORT% --peer-port %PEER_PORT% --discover-port %DISCOVER_PORT% %*
  exit /b %ERRORLEVEL%
)
where python >nul 2>&1 && (
  python -m rdbg watch --port %HTTP_PORT% --data-port %DATA_PORT% --peer-port %PEER_PORT% --discover-port %DISCOVER_PORT% %*
  exit /b %ERRORLEVEL%
)
where python3 >nul 2>&1 && (
  python3 -m rdbg watch --port %HTTP_PORT% --data-port %DATA_PORT% --peer-port %PEER_PORT% --discover-port %DISCOVER_PORT% %*
  exit /b %ERRORLEVEL%
)
echo [watch] Python 3 not found. Install Python and tick "Add python.exe to PATH".
exit /b 1
