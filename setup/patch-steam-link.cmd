@echo off
setlocal
rem Works at archive root or in the source setup directory.
set "engine=%~dp0steam_link_setup.py"
if exist "%~dp0setup\steam_link_setup.py" set "engine=%~dp0setup\steam_link_setup.py"
set "bundled=%~dp0runtime\python\python.exe"
if exist "%~dp0setup\runtime\python\python.exe" set "bundled=%~dp0setup\runtime\python\python.exe"
rem Release archives include Python and a private SSH client. The engine prefers
rem Windows OpenSSH and uses the included client without changing OS features.
if exist "%bundled%" (
  "%bundled%" -B "%engine%" %*
  goto done
)
py -3 -c "import sys;sys.exit(not sys.version_info.__ge__((3,9)))" >nul 2>nul
if not errorlevel 1 (
  py -3 -B "%engine%" %*
  goto done
)
python3 -c "import sys;sys.exit(not sys.version_info.__ge__((3,9)))" >nul 2>nul
if not errorlevel 1 (
  python3 -B "%engine%" %*
  goto done
)
python -c "import sys;sys.exit(not sys.version_info.__ge__((3,9)))" >nul 2>nul
if not errorlevel 1 (
  python -B "%engine%" %*
  goto done
)
echo Python 3.9 or newer was not found. Install Python from python.org, including its launcher, then run this file again.
echo Setup did not download or install anything.
if "%~1"=="" pause
exit /b 2
:done
set "result=%errorlevel%"
if "%~1"=="" pause
exit /b %result%
