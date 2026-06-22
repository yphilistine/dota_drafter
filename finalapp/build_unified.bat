@echo off
setlocal

:: =============================================================================
:: build.bat  —  Dota Draft Assistant (unified GUI)
::
:: Объединяет buildGUI.bat + build_unified.bat в один скрипт.
:: Собирает dota_assistant.exe: ImGui D3D11 GUI + все фазы (1-3) в одном exe.
::
:: Использование:
::   build.bat                — собрать
::   build.bat gui            — то же самое
::   build.bat unified        — то же самое (синоним)
::   build.bat debug          — с /Od /Zi без оптимизаций
:: =============================================================================

set "OUT_DIR=build"
set "TARGET=Dota_Drafter"
set "VCPKG=C:\vcpkg\installed\x64-windows-static"
set "CATBOOST=C:\catboost"

set "OPT_FLAGS=/O2"
if /i "%~1"=="debug" set "OPT_FLAGS=/Od /Zi"

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

:: ── Windows SDK (d3d11.lib, dxgi.lib и т.д.) ─────────────────────────────────
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

:: ─────────────────────────────────────────────────────────────────────────────
:: Компиляция
::
:: Исходники:
::   mainGUI.cpp             — GUI (ImGui/D3D11) + оркестратор фаз 1-3
::   portrait_runner.cpp     — захват портретов + overlay [D]
::   common.cpp              — логирование, HTTP (curl)
::   playerdatafetcher.cpp   — OpenDota / STRATZ / SQLite
::   clouddatafetcher.cpp    — PostgreSQL → SQLite
::   datafetcher.cpp         — runDataFetcher (фаза 1)
::   livestatsfetcher.cpp    — runGsiServer   (фаза 2)
::   dota_picker.cpp         — runPickerGui   (фаза 3, вывод в GUI)
::   dota2_capture.cpp       — захват окна Dota 2
:: ─────────────────────────────────────────────────────────────────────────────

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
    mainGUI.cpp ^
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
    /LIBPATH:"%WINSDK_LIB%" ^
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
    /SUBSYSTEM:WINDOWS ^
    /MACHINE:X64

if %ERRORLEVEL% neq 0 (
    echo(
    echo [FAIL] Сборка не удалась.
    exit /b 1
)

:: Копируем catboostmodel.dll рядом с exe
if exist "%CATBOOST%\catboostmodel.dll" (
    copy /Y "%CATBOOST%\catboostmodel.dll" "%OUT_DIR%\" >nul
    echo [INFO] Copied catboostmodel.dll
)
if exist "%CATBOOST%\catboostmodel-windows-x86_64-1.2.10.dll" (
    copy /Y "%CATBOOST%\catboostmodel-windows-x86_64-1.2.10.dll" "%OUT_DIR%\" >nul
)

echo(
echo [OK] %OUT_DIR%\%TARGET%.exe
echo(
echo  Запуск:
echo    %OUT_DIR%\%TARGET%.exe
echo(
echo  При первом запуске введи Steam ID (32-bit) прямо в интерфейсе.
echo  Env vars (опционально):
echo    set STRATZ_API_KEY=eyJ...
echo(

endlocal