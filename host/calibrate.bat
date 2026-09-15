@echo off
REM Windows calibrate UI. Usage: host\calibrate.bat [--no-browser]
setlocal
set DIR=%~dp0
set PYTHONPATH=%DIR%;%PYTHONPATH%

if "%HTTP_PORT%"=="" set HTTP_PORT=8090
if "%DATA_PORT%"=="" set DATA_PORT=15001
if "%PEER_PORT%"=="" set PEER_PORT=15100
if "%DISCOVER_PORT%"=="" set DISCOVER_PORT=15999

where python >nul 2>nul
if errorlevel 1 (
  echo [calibrate] Python not found
  exit /b 1
)

echo [calibrate] http://localhost:%HTTP_PORT%  data:%DATA_PORT% peer:%PEER_PORT%
echo [calibrate] robot: .\build\calibrate configs\calibration.yaml
python -m rdbg calibrate --port %HTTP_PORT% --data-port %DATA_PORT% --peer-port %PEER_PORT% --discover-port %DISCOVER_PORT% %*
