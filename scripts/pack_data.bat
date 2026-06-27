@echo off
setlocal enabledelayedexpansion
:: pack_data.bat DATA_VERSION SCHEMA_VERSION
:: Example: pack_data.bat 2025.07.01 1

set "DVER=%~1"
set "SCHEMA=%~2"
if "%DVER%"=="" ( echo Usage: pack_data.bat DATA_VERSION SCHEMA_VERSION & exit /b 1 )
if "%SCHEMA%"=="" ( echo Usage: pack_data.bat DATA_VERSION SCHEMA_VERSION & exit /b 1 )

echo [1/5] Writing meta table to _data.db...
sqlite3 finalapp\draft_helper_abstract_data.db "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
sqlite3 finalapp\draft_helper_abstract_data.db "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','%SCHEMA%');"
sqlite3 finalapp\draft_helper_abstract_data.db "INSERT OR REPLACE INTO meta(key,value) VALUES('data_version','%DVER%');"

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

echo [3/5] Creating GitHub release data-%DVER%...
gh release create "data-%DVER%" ^
    finalapp\draft_helper_abstract.cbm ^
    finalapp\draft_helper_abstract_data.db ^
    --title "Data %DVER%" --notes "Data version %DVER%, schema %SCHEMA%"

echo [4/5] Updating manifest.json...
set "BASE_URL=https://github.com/yphilistine/dota_drafter/releases/download/data-%DVER%"
powershell -Command "$m = Get-Content manifest.json -Raw | ConvertFrom-Json; $m.data.version = '%DVER%'; $m.data.schema = [int]%SCHEMA%; $m.data.files.'draft_helper_abstract.cbm'.url = '%BASE_URL%/draft_helper_abstract.cbm'; $m.data.files.'draft_helper_abstract.cbm'.sha256 = '%SHA_CBM%'; $m.data.files.'draft_helper_abstract_data.db'.url = '%BASE_URL%/draft_helper_abstract_data.db'; $m.data.files.'draft_helper_abstract_data.db'.sha256 = '%SHA_DB%'; $m | ConvertTo-Json -Depth 10 | Set-Content manifest.json -Encoding utf8"

echo [5/5] Committing manifest.json...
git add manifest.json
git commit -m "Release data %DVER% (schema %SCHEMA%)"
git push

echo [DONE] Released data %DVER%
