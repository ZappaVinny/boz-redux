@echo off
setlocal
cd /d "%~dp0"
if not exist assets\boz.s3e.unpacked (
  echo Run setup.bat first.
  pause
  exit /b 1
)
if not exist saves mkdir saves
set "HOME=%~dp0saves"
echo Starting BOZ Redux. The log is written to boz-log.txt.
codboz_s3e_loader.exe --root . --run assets\boz.s3e.unpacked > boz-log.txt 2>&1
echo Game exited with code %errorlevel%.
pause
