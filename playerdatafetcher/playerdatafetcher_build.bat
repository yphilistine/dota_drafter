@echo off
setlocal

set VCPKG=C:\vcpkg\installed\x64-windows-static
set BUILD=build

if not exist %BUILD% mkdir %BUILD%

cl.exe ^
  /EHsc /std:c++17 /MT /O2 /W3 ^
  /D_CRT_SECURE_NO_WARNINGS ^
  /Fe:%BUILD%\datafetcher.exe ^
  /I"%VCPKG%\include" ^
  /I"%VCPKG%\include\postgresql" ^
  common.cpp ^
  playerdatafetcher.cpp ^
  clouddatafetcher.cpp ^
  datafetcher.cpp ^
  /link ^
  /LIBPATH:"%VCPKG%\lib" ^
  libcurl.lib ^
  sqlite3.lib ^
  libpq.lib ^
  libpgcommon.lib ^
  libpgport.lib ^
  libssl.lib ^
  libcrypto.lib ^
  lz4.lib ^
  zlib.lib ^
  Ws2_32.lib ^
  Crypt32.lib ^
  Wldap32.lib ^
  Normaliz.lib ^
  advapi32.lib ^
  bcrypt.lib ^
  user32.lib ^
  gdi32.lib ^
  shell32.lib ^
  Iphlpapi.lib ^
  Secur32.lib

if %ERRORLEVEL% neq 0 (
    echo Build failed.
    exit /b %ERRORLEVEL%
)

echo Build OK: %BUILD%\datafetcher.exe
endlocal
