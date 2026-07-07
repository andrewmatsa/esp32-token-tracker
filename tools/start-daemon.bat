@echo off
cd /d "%~dp0\.."
python tools\usage-daemon.py --push claude:0
pause
