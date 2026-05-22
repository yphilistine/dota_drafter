@echo off
setlocal

:: ─────────────────────────────────────────────────────────────────────────────
:: build_unified.bat
::
:: Собирает dota_assistant.exe — единый пайплайн:
::   [1] DataFetcher  (OpenDota + STRATZ + PostgreSQL)
::   [2] GSI-сервер   (phase tracking)
::   [3] Portrait     (захват портретов → livepicks, без PNG)
::   [4] Picker       (CatBoost рекомендации)
::
:: Зависимости через vcpkg:
::   libcurl sqlite3 libpq openssl lz4 zlib nlohmann-json
::   catboost (отдельно в C:\catboost)
::
:: Запуск:
::   build\dota_assistant.exe <account_id>
::   build\dota_assistant.exe <account_id> <steam_key> <stratz_token>
:: ─────────────────────────────────────────────────────────────────────────────

set "OUT_DIR=build"
set "TARGET=dota_assistant"
set "VCPKG=C:\vcpkg\installed\x64-windows-static"
set "CATBOOST=C:\catboost"

:: ── Найти vcvarsall.bat ───────────────────────────────────────────────────────
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (
        `"%VSWHERE%" -latest -products * -requires Microsoft.VisualCPP.Tools.HostX86.TargetX64 -property installationPath`
    ) do (
        if exist "%%I\VC\Auxiliary\Build\vcvarsall.bat" (
            set "VCVARS=%%I\VC\Auxiliary\Build\vcvarsall.bat"
            goto :found_vcvars
        )
    )
)
for %%V in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do ( if exist "%%~V" ( set "VCVARS=%%~V" & goto :found_vcvars ) )
echo [ERROR] vcvarsall.bat не найден.
exit /b 1

:found_vcvars
echo [INFO] MSVC: %VCVARS%
call "%VCVARS%" x64 >nul 2>&1
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

:: ─────────────────────────────────────────────────────────────────────────────
:: Компиляция
::
:: Исходники:
::   main_unified.cpp        — оркестратор (NEW)
::   shared_types.h          — общие типы (NEW, header-only)
::   portrait_runner.cpp     — захват портретов (NEW)
::   common.cpp              — логирование, HTTP
::   playerdatafetcher.cpp   — OpenDota / STRATZ / SQLite
::   clouddatafetcher.cpp    — PostgreSQL → SQLite
::   datafetcher.cpp         — ИЗМЕНЁН: main → runDataFetcher
::   livestatsfetcher.cpp    — ИЗМЕНЁН: main → runGsiServer
::   dota_picker.cpp         — ИЗМЕНЁН: main → runPicker
::   dota2_capture.cpp       — захват окна (без изменений)
:: ─────────────────────────────────────────────────────────────────────────────

cl.exe ^
    /nologo ^
    /std:c++17 ^
    /EHsc ^
    /MT ^
    /O2 ^
    /W3 ^
    /DWIN32 /D_WINDOWS /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DCURL_STATICLIB ^
    /Fe:"%OUT_DIR%\%TARGET%.exe" ^
    /Fo:"%OUT_DIR%\\" ^
    /I"%VCPKG%\include" ^
    /I"%VCPKG%\include\postgresql" ^
    /I"%CATBOOST%" ^
    main_unified.cpp ^
    portrait_runner.cpp ^
    common.cpp ^
    playerdatafetcher.cpp ^
    clouddatafetcher.cpp ^
    datafetcher.cpp ^
    livestatsfetcher.cpp ^
    dota_picker.cpp ^
    dota2_capture.cpp ^
    /link ^
    /LIBPATH:"%VCPKG%\lib" ^
    /LIBPATH:"%CATBOOST%" ^
    libcurl.lib ^
    sqlite3.lib ^
    libpq.lib libpgcommon.lib libpgport.lib ^
    libssl.lib libcrypto.lib ^
    catboostmodel.lib ^
    lz4.lib zlib.lib ^
    gdiplus.lib ^
    Ws2_32.lib Crypt32.lib Wldap32.lib Normaliz.lib ^
    advapi32.lib bcrypt.lib user32.lib gdi32.lib ^
    shell32.lib Iphlpapi.lib Secur32.lib ^
    /SUBSYSTEM:CONSOLE ^
    /MACHINE:X64

if %ERRORLEVEL% neq 0 (
    echo(
    echo [FAIL] Сборка не удалась.
    exit /b 1
)

if exist "%CATBOOST%\catboostmodel.dll" copy /Y "%CATBOOST%\catboostmodel.dll" "%OUT_DIR%\" >nul
if exist "%CATBOOST%\catboostmodel-windows-x86_64-1.2.10.dll" copy /Y "%CATBOOST%\catboostmodel-windows-x86_64-1.2.10.dll" "%OUT_DIR%\" >nul

echo(
echo [OK] %OUT_DIR%\%TARGET%.exe
echo(
echo Usage:
echo   %OUT_DIR%\%TARGET%.exe ^<account_id^>
echo   %OUT_DIR%\%TARGET%.exe ^<account_id^> ^<steam_key^> ^<stratz_token^>
echo(
echo Phases:
echo   1) DataFetcher - download player data
echo   2) GSI server - monitor game phase on :3000
echo   3) DRAFT - simultaneously:
echo        Portrait - capture portraits to livepicks
echo        Picker   - CatBoost recommendations

endlocal
