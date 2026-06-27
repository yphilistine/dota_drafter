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

:: ── 1. Update version.h ─────────────────────────────────────────────────
echo [1/6] Updating version.h to %VER%...
set "VFILE=finalapp\version.h"
set "VTMP=%VFILE%.tmp"
> "%VTMP%" (
    for /f "usebackq delims=" %%L in ("%VFILE%") do (
        set "LINE=%%L"
        echo !LINE:kAppVersion      = "1.0.0"=kAppVersion      = "%VER%"!
    )
)
move /y "%VTMP%" "%VFILE%" >nul

:: ── 2. Build ─────────────────────────────────────────────────────────────
echo [2/6] Building app...
pushd finalapp
call build_unified.bat
if %ERRORLEVEL% neq 0 ( echo [FAIL] Build failed & popd & exit /b 1 )
popd

:: ── 3. Update .iss and build installer ───────────────────────────────────
echo [3/6] Building installer...
set "ISSFILE=installer\dota_draft_setup.iss"
set "ISSTMP=%ISSFILE%.tmp"
> "%ISSTMP%" (
    for /f "usebackq delims=" %%L in ("%ISSFILE%") do (
        set "LINE=%%L"
        set "CHECK=!LINE:~0,16!"
        if "!CHECK!"=="#define version " (
            echo #define version "%VER%"
        ) else (
            echo !LINE!
        )
    )
)
move /y "%ISSTMP%" "%ISSFILE%" >nul

"C:\Program Files\Inno Setup 7\ISCC.exe" "%ISSFILE%"
if %ERRORLEVEL% neq 0 (
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" "%ISSFILE%"
    if %ERRORLEVEL% neq 0 ( echo [FAIL] Installer build failed & exit /b 1 )
)

:: ── 4. SHA-256 ───────────────────────────────────────────────────────────
echo [4/6] Computing SHA-256...
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile installer\dota_drafter_setup.exe SHA256') do (
    set "SHA=%%H"
    goto :got_sha
)
:got_sha
set "SHA=%SHA: =%"
echo SHA-256: %SHA%

:: ── 5. GitHub release ────────────────────────────────────────────────────
echo [5/6] Creating GitHub release v%VER%...
gh release create "v%VER%" installer\dota_drafter_setup.exe --title "v%VER%" --notes "App version %VER%"

:: ── 6. Update manifest.json ──────────────────────────────────────────────
echo [6/6] Updating manifest.json...
set "URL=https://github.com/yphilistine/dota_drafter/releases/download/v%VER%/dota_drafter_setup.exe"

set "PSSCRIPT=%TEMP%\update_manifest.ps1"
> "%PSSCRIPT%" (
    echo $m = Get-Content 'manifest.json' -Raw ^| ConvertFrom-Json
    echo $m.app.version = '%VER%'
    echo $m.app.sha256 = '%SHA%'
    echo $m.app.url = '%URL%'
    echo $m ^| ConvertTo-Json -Depth 10 ^| Set-Content 'manifest.json' -Encoding utf8
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PSSCRIPT%"
del "%PSSCRIPT%" >nul 2>&1

git add manifest.json finalapp\version.h installer\dota_draft_setup.iss
git commit -m "Release app v%VER%"
git push

echo [DONE] Released app v%VER%
