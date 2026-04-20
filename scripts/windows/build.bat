@echo off
REM scripts/windows/build.bat
REM
REM Builds fractalsql.dll on Windows with the MSVC toolchain using
REM static CRT (/MT) and whole-program optimization (/GL), matching
REM the Linux posture — zero runtime dependency on the Visual C++
REM Redistributable, and zero dependency on libluajit at load time.
REM
REM One DLL per (MariaDB major, arch) cell: the UDF ABI is stable
REM across 10.6 / 10.11 / 11.4, but we still build per-major on
REM Windows because the install target path differs per server
REM installation (C:\Program Files\MariaDB ^<VER^>\lib\plugin\).
REM
REM Prerequisites
REM   * Visual Studio Build Tools (cl.exe on PATH — invoke from a
REM     Developer Command Prompt, or `call vcvarsall.bat ^<arch^>` first).
REM   * A static LuaJIT archive (lua51.lib) built with msvcbuild.bat
REM     static against the same host arch as cl.exe.
REM   * A MariaDB Windows binaries tree (mariadb-^<VER^>-winx64.zip
REM     from archive.mariadb.org), unpacked so that:
REM         %MARIADB_DIR%\include\mysql\mysql.h
REM     exists. Only headers are needed — UDFs don't link against a
REM     server import lib.
REM
REM Environment overrides
REM   LUAJIT_DIR    directory with lua.h / lualib.h / lauxlib.h + lua51.lib
REM   MARIADB_DIR   directory with MariaDB binaries tree
REM                 (the "mariadb-^<VER^>-winx64" root from the ZIP)
REM   MARIADB_MAJOR MariaDB major version being targeted (e.g. 10.6, 10.11, 11.4, 12.2)
REM   OUT_DIR       output directory for fractalsql.dll
REM
REM Invocation
REM   set LUAJIT_DIR=%CD%\deps\LuaJIT\src
REM   set MARIADB_DIR=%CD%\deps\mariadb\root
REM   set MARIADB_MAJOR=11.4
REM   set OUT_DIR=dist\windows\mdb11.4
REM   scripts\windows\build.bat

setlocal ENABLEEXTENSIONS ENABLEDELAYEDEXPANSION

if "%LUAJIT_DIR%"=="" set LUAJIT_DIR=%CD%\deps\LuaJIT\src
if "%MARIADB_DIR%"=="" (
    echo ==^> ERROR: MARIADB_DIR must point at an unpacked MariaDB binaries tree
    exit /b 1
)
if "%MARIADB_MAJOR%"==""   (
    echo ==^> ERROR: MARIADB_MAJOR must be set ^(10.6 ^| 10.11 ^| 11.4 ^| 12.2^)
    exit /b 1
)
if "%OUT_DIR%"==""    set OUT_DIR=dist\windows\mdb%MARIADB_MAJOR%

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

echo ==^> LUAJIT_DIR     = %LUAJIT_DIR%
echo ==^> MARIADB_DIR    = %MARIADB_DIR%
echo ==^> MARIADB_MAJOR  = %MARIADB_MAJOR%
echo ==^> OUT_DIR        = %OUT_DIR%

REM LuaJIT's msvcbuild.bat static emits lua51.lib; accept the
REM Makefile-style libluajit-5.1.lib name too if present.
set LUAJIT_LIB=%LUAJIT_DIR%\libluajit-5.1.lib
if not exist "%LUAJIT_LIB%" (
    if exist "%LUAJIT_DIR%\lua51.lib" set LUAJIT_LIB=%LUAJIT_DIR%\lua51.lib
)
if not exist "%LUAJIT_LIB%" (
    echo ==^> ERROR: no LuaJIT static library in %LUAJIT_DIR%
    echo         ^(expected libluajit-5.1.lib or lua51.lib^)
    exit /b 1
)
echo ==^> LUAJIT_LIB     = %LUAJIT_LIB%

REM MariaDB header tree. A UDF doesn't call server-exported symbols,
REM so no server import lib is needed — the server loads this DLL and
REM calls the exported UDF entry points by name via GetProcAddress.
set MARIADB_INC=%MARIADB_DIR%\include\mysql
if not exist "%MARIADB_INC%\mysql.h" (
    echo ==^> ERROR: %MARIADB_INC%\mysql.h not found — check MARIADB_DIR layout
    exit /b 1
)
echo ==^> MARIADB_INC    = %MARIADB_INC%

REM cl.exe flags:
REM   /MT     static CRT (no MSVC runtime DLL dependency)
REM   /GL     whole-program optimization (paired with /LTCG at link)
REM   /O2     optimize for speed
REM   /LD     build a DLL
REM   /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS
REM
REM The UDF entry points (fractal_search / fractalsql_edition /
REM fractalsql_version plus their _init/_deinit) carry
REM __declspec(dllexport) via the FRACTAL_EXPORT macro in
REM src/fractalsql.c, so no .def file is needed and candle/light
REM downstream won't need to play with export tables.
cl.exe /nologo /MT /GL /O2 ^
    /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
    /I"%LUAJIT_DIR%" ^
    /I"%MARIADB_INC%" ^
    /Iinclude ^
    /LD src\fractalsql.c ^
    /Fo"%OUT_DIR%\\" ^
    /Fe"%OUT_DIR%\fractalsql.dll" ^
    /link /LTCG ^
        "%LUAJIT_LIB%"

if errorlevel 1 (
    echo.
    echo ==^> BUILD FAILED for MariaDB %MARIADB_MAJOR%
    exit /b 1
)

echo.
echo ==^> Built %OUT_DIR%\fractalsql.dll ^(MariaDB %MARIADB_MAJOR%^)
dir "%OUT_DIR%\fractalsql.dll"

endlocal
