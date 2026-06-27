@echo off
setlocal enabledelayedexpansion
:: pack_data.bat DATA_VERSION SCHEMA_VERSION
:: Example: pack_data.bat 2025.07.01 1
:: Run from repo root (dota_drafter\)

set "DVER=%~1"
set "SCHEMA=%~2"
if "%DVER%"=="" ( echo Usage: pack_data.bat DATA_VERSION SCHEMA_VERSION & exit /b 1 )
if "%SCHEMA%"=="" ( echo Usage: pack_data.bat DATA_VERSION SCHEMA_VERSION & exit /b 1 )

:: ── 1. Write meta ────────────────────────────────────────────────────────
echo [1/5] Writing meta table to _data.db...
sqlite3 finalapp\draft_helper_abstract_data.db "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
sqlite3 finalapp\draft_helper_abstract_data.db "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','%SCHEMA%');"
sqlite3 finalapp\draft_helper_abstract_data.db "INSERT OR REPLACE INTO meta(key,value) VALUES('data_version','%DVER%');"

:: ── 2. SHA-256 ───────────────────────────────────────────────────────────
echo [2/5] Computing SHA-256...
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile finalapp\draft_helper_abstract.cbm SHA256') do (
    set "SHA_CBM=%%H"
    goto :sha1
)
:sha1
set "SHA_CBM=%SHA_CBM: =%"
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile finalapp\draft_helper_abstract_data.db SHA256') do (
    set "SHA_DB=%%H"
    goto :sha2
)
:sha2
set "SHA_DB=%SHA_DB: =%"
echo CBM: %SHA_CBM%
echo DB:  %SHA_DB%

:: ── 3. GitHub release ────────────────────────────────────────────────────
echo [3/5] Creating GitHub release data-%DVER%...
gh release create "data-%DVER%" finalapp\draft_helper_abstract.cbm finalapp\draft_helper_abstract_data.db --title "Data %DVER%" --notes "Data version %DVER%, schema %SCHEMA%"

:: ── 4. Update manifest.json ──────────────────────────────────────────────
echo [4/5] Updating manifest.json...
set "BASE_URL=https://github.com/yphilistine/dota_drafter/releases/download/data-%DVER%"
set "PS1=%TEMP%\dd_update_data_manifest.ps1"
(
    echo $j = Get-Content 'manifest.json' -Raw
    echo $m = ConvertFrom-Json $j
    echo $m.data.version = '%DVER%'
    echo $m.data.schema = [int]%SCHEMA%
    echo $m.data.files.'draft_helper_abstract.cbm'.url = '%BASE_URL%/draft_helper_abstract.cbm'
    echo $m.data.files.'draft_helper_abstract.cbm'.sha256 = '%SHA_CBM%'
    echo $m.data.files.'draft_helper_abstract_data.db'.url = '%BASE_URL%/draft_helper_abstract_data.db'
    echo $m.data.files.'draft_helper_abstract_data.db'.sha256 = '%SHA_DB%'
    echo $out = ConvertTo-Json $m -Depth 10
    echo Set-Content 'manifest.json' $out -Encoding utf8
) > "%PS1%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
del "%PS1%" >nul 2>&1

:: ── 5. Commit ────────────────────────────────────────────────────────────
echo [5/5] Committing manifest.json...
git add manifest.json
git commit -m "Release data %DVER% (schema %SCHEMA%)"
git push

echo [DONE] Released data %DVER%
