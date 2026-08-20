@echo off
setlocal EnableExtensions
set "HOST=%~dp0"
set "PYTHONPATH=%HOST%;%PYTHONPATH%"
where py >nul 2>&1 && (
  py -3 "%HOST%watch.py" %*
  exit /b %ERRORLEVEL%
)
where python >nul 2>&1 && (
  python "%HOST%watch.py" %*
  exit /b %ERRORLEVEL%
)
where python3 >nul 2>&1 && (
  python3 "%HOST%watch.py" %*
  exit /b %ERRORLEVEL%
)
echo [watch] Python 3 not found. Install Python and enable "Add python.exe to PATH".
exit /b 1
