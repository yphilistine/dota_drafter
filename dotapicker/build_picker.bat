@echo off
setlocal

set VCPKG=C:\vcpkg\installed\x64-windows-static
set CATBOOST=C:\catboost
set OUT=build

if not exist %OUT% mkdir %OUT%

cl.exe /EHsc /std:c++17 /MT /O2 /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:%OUT%\dota_picker.exe ^
   /I"%VCPKG%\include" ^
   /I"%CATBOOST%" ^
   dota_picker.cpp ^
   /link ^
   /LIBPATH:"%VCPKG%\lib" ^
   /LIBPATH:"%CATBOOST%" ^
   sqlite3.lib ^
   catboostmodel.lib

if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] Build failed.
    exit /b 1
)

echo.
echo [OK] build\dota_picker.exe
