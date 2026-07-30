@echo off
setlocal

:: =============================================================================
:: build.bat  -  Dota Draft Assistant (unified GUI)
::
:: Объединяет buildGUI.bat + build_unified.bat в один скрипт.
:: Собирает dota_assistant.exe: ImGui D3D11 GUI + все фазы (1-3) в одном exe.
::
:: Использование:
::   build.bat                - собрать
::   build.bat gui            - то же самое
::   build.bat unified        - то же самое (синоним)
::   build.bat debug          - с /Od /Zi без оптимизаций
:: =============================================================================

set "OUT_DIR=build"
set "TARGET=Dota_Drafter"
set "VCPKG=C:\vcpkg\installed\x64-windows-static"
set "CATBOOST=C:\catboost"

set "OPT_FLAGS=/O2"
if /i "%~1"=="debug" set "OPT_FLAGS=/Od /Zi"

:: -- Найти vcvarsall.bat -------------------------------------------------------
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

:: -- Windows SDK (d3d11.lib, dxgi.lib и т.д.) ---------------------------------
set "WINSDK_LIB="
if defined WindowsSdkDir (
    if defined WindowsSdkLibVersion (
        set "WINSDK_LIB=%WindowsSdkDir%Lib\%WindowsSdkLibVersion%um\x64"
    )
)
if not defined WINSDK_LIB (
    for /f "tokens=*" %%K in (
        'dir /b /ad /o-n "C:\Program Files (x86)\Windows Kits\10\Lib\" 2^>nul'
    ) do (
        if not defined WINSDK_LIB (
            set "WINSDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\%%K\um\x64"
        )
    )
)
if not defined WINSDK_LIB (
    echo [ERROR] Windows SDK не найден.
    exit /b 1
)
echo [INFO] WINSDK: %WINSDK_LIB%

:: -----------------------------------------------------------------------------
:: Компиляция
::
:: Исходники:
::   mainGUI.cpp             - GUI (ImGui/D3D11): WinMain, D3D11, WndProc, RenderFrame
::   app_state.cpp           - общее состояние (GameInfo/PortraitState/PickerState/PlayerState/AppNotice)
::   update_window.cpp       - нативное окно апдейтера (Win32, до D3D11/ImGui)
::   orchestrator.cpp        - фоновый оркестратор (потоки portrait/picker/GSI, livepicks)
::   gui_draw.cpp            - панели ImGui (Draft/Picks/Meta Heroes), кэш портретов/меты
::   portrait_runner.cpp     - захват портретов + позиций (250мс цикл)
::   overlay_button.cpp      - прозрачная кнопка [D] поверх Dota 2
::   common.cpp              - логирование, HTTP (curl)
::   playerdatafetcher.cpp   - OpenDota / STRATZ / SQLite (история матчей игрока)
::   hero_meta_stats.cpp     - живая мета-статистика героев (STRATZ heroStats, фаза 1a)
::   clouddatafetcher.cpp    - PostgreSQL -> SQLite
::   datafetcher.cpp         - runDataFetcher (фаза 1)
::   livestatsfetcher.cpp    - runGsiServer   (фаза 2)
::   dota_picker.cpp         - runPickerGui   (фаза 3, вывод в GUI)
::   dota2_capture.cpp       - захват окна Dota 2
:: -----------------------------------------------------------------------------

:: -- Читаем версию приложения из version.h (kAppVersion) ----------------------
set "PS_VER=%TEMP%\dd_read_ver.ps1"
(
    echo $c = Get-Content 'version.h' -Raw
    echo $m = $c -match 'kAppVersion\s*=\s*"([^^\x22]+)"'
    echo Write-Output $Matches[1]
) > "%PS_VER%"
for /f "usebackq delims=" %%V in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_VER%"`) do set "APP_VER=%%V"
del "%PS_VER%" >nul 2>&1

if not defined APP_VER (
    echo [ERROR] Не удалось прочитать kAppVersion из version.h
    exit /b 1
)

for /f "tokens=1,2,3 delims=." %%a in ("%APP_VER%") do (
    set "VER_MAJOR=%%a"
    set "VER_MINOR=%%b"
    set "VER_PATCH=%%c"
)
set "VER_BUILD=0"
echo [INFO] App version: %APP_VER% -^> resource %VER_MAJOR%.%VER_MINOR%.%VER_PATCH%.%VER_BUILD%

:: -- Компилируем version.rc -> version.res -------------------------------------
rc.exe /nologo /dVER_MAJOR=%VER_MAJOR% /dVER_MINOR=%VER_MINOR% /dVER_PATCH=%VER_PATCH% /dVER_BUILD=%VER_BUILD% /fo "%OUT_DIR%\version.res" version.rc
if %ERRORLEVEL% neq 0 (
    echo [FAIL] rc.exe: version.rc
    exit /b 1
)

cl.exe ^
    /nologo ^
    /std:c++17 ^
    /EHsc ^
    /MT ^
    %OPT_FLAGS% ^
    /W3 ^
    /DWIN32 /D_WINDOWS /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DCURL_STATICLIB ^
    /Fe:"%OUT_DIR%\%TARGET%.exe" ^
    /Fo:"%OUT_DIR%\\" ^
    /I"%VCPKG%\include" ^
    /I"%VCPKG%\include\postgresql" ^
    /I"%CATBOOST%" ^
    /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\cppwinrt" ^
    mainGUI.cpp ^
    app_state.cpp ^
    update_window.cpp ^
    orchestrator.cpp ^
    gui_draw.cpp ^
    portrait_runner.cpp ^
    overlay_button.cpp ^
    common.cpp ^
    playerdatafetcher.cpp ^
    hero_meta_stats.cpp ^
    clouddatafetcher.cpp ^
    datafetcher.cpp ^
    livestatsfetcher.cpp ^
    dota_picker.cpp ^
    dota2_capture.cpp ^
    version_utils.cpp ^
    updater.cpp ^
    /link ^
    /LIBPATH:"%VCPKG%\lib" ^
    /LIBPATH:"%CATBOOST%" ^
    /LIBPATH:"%WINSDK_LIB%" ^
    "%OUT_DIR%\version.res" ^
    imgui.lib ^
    d3d11.lib dxgi.lib d3dcompiler.lib ^
    dwmapi.lib ^
    libcurl.lib ^
    sqlite3.lib ^
    libpq.lib libpgcommon.lib libpgport.lib ^
    libssl.lib libcrypto.lib ^
    catboostmodel.lib ^
    lz4.lib zlib.lib ^
    gdiplus.lib ^
    Ws2_32.lib Crypt32.lib Wldap32.lib Normaliz.lib ^
    advapi32.lib bcrypt.lib ^
    user32.lib gdi32.lib shell32.lib ^
    Iphlpapi.lib Secur32.lib ^
    WindowsApp.lib ^
    /SUBSYSTEM:WINDOWS ^
    /MACHINE:X64

if %ERRORLEVEL% neq 0 (
    echo(
    echo [FAIL] Сборка не удалась.
    exit /b 1
)

:: -- Подставляем версию в manifest (копия в OUT_DIR, исходный app.manifest не трогаем) --
set "PS_MANIFEST=%TEMP%\dd_patch_manifest.ps1"
(
    echo $c = Get-Content 'app.manifest' -Raw
    echo $c = $c -replace 'name=\x22DotaDrafter\.DraftAssistant\x22\s+version=\x22[^^\x22]*\x22', 'name="DotaDrafter.DraftAssistant" version="%VER_MAJOR%.%VER_MINOR%.%VER_PATCH%.%VER_BUILD%"'
    echo Set-Content '%OUT_DIR%\app.manifest' $c -Encoding utf8 -NoNewline
) > "%PS_MANIFEST%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_MANIFEST%"
del "%PS_MANIFEST%" >nul 2>&1

:: -- Встраиваем manifest ------------------------------------------------------
mt.exe -nologo -manifest "%OUT_DIR%\app.manifest" -outputresource:"%OUT_DIR%\%TARGET%.exe;1"
if %ERRORLEVEL% neq 0 (
    echo [WARN] mt.exe: не удалось встроить manifest
)

:: Копируем catboostmodel.dll рядом с exe
if exist "%CATBOOST%\catboostmodel.dll" (
    copy /Y "%CATBOOST%\catboostmodel.dll" "%OUT_DIR%\" >nul
    echo [INFO] Copied catboostmodel.dll
)
if exist "%CATBOOST%\catboostmodel-windows-x86_64-1.2.10.dll" (
    copy /Y "%CATBOOST%\catboostmodel-windows-x86_64-1.2.10.dll" "%OUT_DIR%\" >nul
)

:: Копируем exe в корневую папку проекта
copy /Y "%OUT_DIR%\%TARGET%.exe" "%TARGET%.exe" >nul
echo [INFO] Copied %TARGET%.exe to project root

echo(
echo [OK] %OUT_DIR%\%TARGET%.exe
echo [OK] %TARGET%.exe
echo(
echo  Запуск:
echo    %OUT_DIR%\%TARGET%.exe
echo    %TARGET%.exe
echo(
echo  При первом запуске введи Steam ID (32-bit) прямо в интерфейсе.
echo  Env vars (опционально):
echo    set STRATZ_API_KEY=eyJ...
echo(

endlocal