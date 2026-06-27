@echo off
setlocal enabledelayedexpansion
:: release_app.bat VERSION
:: Example: release_app.bat 1.2.0

set "VER=%~1"
if "%VER%"=="" (
    echo Usage: release_app.bat MAJOR.MINOR.PATCH
    exit /b 1
)

for /f "tokens=1,2,3 delims=." %%a in ("%VER%") do (
    set "MAJOR=%%a"
    set "MINOR=%%b"
    set "PATCH=%%c"
)

echo [1/6] Updating version.h to %VER%...
powershell -Command "(Get-Content finalapp\version.h -Encoding utf8) -replace 'kAppVersion\s*=\s*\"[^\"]*\"', 'kAppVersion      = \"%VER%\"' | Set-Content finalapp\version.h -Encoding utf8"

echo [2/6] Building app...
pushd finalapp
call build_unified.bat
if %ERRORLEVEL% neq 0 ( echo [FAIL] Build failed & popd & exit /b 1 )
popd

echo [3/6] Building installer...
powershell -Command "(Get-Content installer\dota_draft_setup.iss -Encoding utf8) -replace '#define version \"[^\"]*\"', '#define version \"%VER%\"' | Set-Content installer\dota_draft_setup.iss -Encoding utf8"
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\dota_draft_setup.iss
if %ERRORLEVEL% neq 0 ( echo [FAIL] Installer build failed & exit /b 1 )

echo [4/6] Computing SHA-256...
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile installer\dota_drafter_setup.exe SHA256') do (
    set "SHA=%%H"
    goto :got_sha
)
:got_sha
set "SHA=%SHA: =%"
echo SHA-256: %SHA%

echo [5/6] Creating GitHub release v%VER%...
gh release create "v%VER%" installer\dota_drafter_setup.exe --title "v%VER%" --notes "App version %VER%"

echo [6/6] Updating manifest.json...
set "URL=https://github.com/yphilistine/dota_drafter/releases/download/v%VER%/dota_drafter_setup.exe"
powershell -Command "$m = Get-Content manifest.json -Raw | ConvertFrom-Json; $m.app.version = '%VER%'; $m.app.sha256 = '%SHA%'; $m.app.url = '%URL%'; $m | ConvertTo-Json -Depth 10 | Set-Content manifest.json -Encoding utf8"

git add manifest.json finalapp\version.h installer\dota_draft_setup.iss
git commit -m "Release app v%VER%"
git push

echo [DONE] Released app v%VER%
