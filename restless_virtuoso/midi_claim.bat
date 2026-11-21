@echo off
cd /d "%~dp0"
where py >nul 2>nul && (set PY=py -3) || (set PY=python)
%PY% -u midi_claim.py
set CODE=%errorlevel%
echo.
echo exited with code %CODE%
pause
