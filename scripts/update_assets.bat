@echo off
setlocal enabledelayedexpansion
:: update_assets.bat
:: Пересчитывает sha256 для finalapp\assets\*.png и finalapp\hero_hashes.dat,
:: записывает их в manifest.json (url = raw.githubusercontent.com/main, без
:: GitHub Releases и версий) и коммитит. Запускать после добавления/переименования/
:: удаления файлов героев в assets/ или после regen hero_hashes.dat.
:: Run from repo root (dota_drafter\)

screencapture\build\build_hero_db.exe --dir C:\Users\ANDREY\Desktop\heropngs --out screencapture\hero_hashes.dat
if %ERRORLEVEL% neq 0 ( echo [FAIL] build_hero_db.exe failed & exit /b 1 )

copy /Y screencapture\hero_hashes.dat finalapp\hero_hashes.dat >nul
if %ERRORLEVEL% neq 0 ( echo [FAIL] copy hero_hashes.dat to finalapp failed & exit /b 1 )

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "scripts\update_assets.ps1"
if %ERRORLEVEL% neq 0 ( echo [FAIL] update_assets.ps1 failed & exit /b 1 )

git add finalapp\assets finalapp\hero_hashes.dat manifest.json
git commit -m "Update hero assets/hashes"
git push

echo [DONE] Assets/hashes manifest updated
