@echo off
setlocal

set "OUT_DIR=build"
set "TARGET=dota2_portraits"

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
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%V (
        set "VCVARS=%%~V"
        goto :found_vcvars
    )
)

echo [ERROR] vcvarsall.bat not found.
echo         Run this script from a "Developer Command Prompt" or add your VS path manually.
exit /b 1

:found_vcvars
echo [INFO] Using: %VCVARS%

call "%VCVARS%" x64 >nul 2>&1

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

cl.exe ^
    /nologo ^
    /std:c++17 ^
    /EHsc ^
    /MT ^
    /O2 ^
    /W3 ^
    /DWIN32 /D_WINDOWS /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fe:"%OUT_DIR%\%TARGET%.exe" ^
    /Fo:"%OUT_DIR%\\" ^
    main.cpp dota2_capture.cpp ^
    /link ^
    gdiplus.lib gdi32.lib user32.lib ^
    /SUBSYSTEM:CONSOLE ^
    /MACHINE:X64

if %ERRORLEVEL% == 0 (
    echo.
    echo [OK] Build succeeded: %OUT_DIR%\%TARGET%.exe
    echo.
    echo Usage:
    echo   %OUT_DIR%\%TARGET%.exe              -- interactive mode
    echo   %OUT_DIR%\%TARGET%.exe --once        -- single capture
    echo   %OUT_DIR%\%TARGET%.exe --loop 500    -- loop every 500ms
    echo   %OUT_DIR%\%TARGET%.exe --out C:\imgs -- custom output dir
) else (
    echo.
    echo [FAIL] Build failed. See errors above.
    exit /b 1
)

endlocal
