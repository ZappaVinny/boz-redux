@echo off
setlocal
cd /d "%~dp0"
if not exist com.activision.boz.apk (
  echo Put your com.activision.boz.apk (version 1.0.11^) in this folder, then run setup.bat again.
  pause
  exit /b 1
)
codboz_apk_extract.exe extract com.activision.boz.apk assets
if errorlevel 1 (
  echo APK extraction failed.
  pause
  exit /b 1
)
for %%f in (blackops_etc.dz blackops_gles1.dz) do (
  if exist "%%f" (
    move /y "%%f" assets\ >nul
  ) else if not exist "assets\%%f" (
    echo Downloading %%f from Activision's CDN...
    curl -fL -o "assets\%%f" "http://cdn-boz-android.callofduty.com/PROD/CODBOZ/1_0_9/%%f"
    if errorlevel 1 (
      echo Download of %%f failed. Copy it into the assets folder manually.
      pause
      exit /b 1
    )
  )
)
echo.
echo Game data is ready. Start the game with run.bat.
pause
