@echo off
rem Build self-contained 32-bit DOS executables (COSMO1.EXE, COSMO2.EXE, COSMO3.EXE)
rem on Windows using Open Watcom v2 + CauseWay DOS extender.
rem
rem Usage:
rem   set WATCOM=C:\watcom
rem   build.bat
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "OUT=%ROOT%\bin"

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%OUT%" mkdir "%OUT%"

if "%WATCOM%"=="" (
    if exist "C:\watcom\h" (
        set "WATCOM=C:\watcom"
    ) else if exist "%USERPROFILE%\watcom\h" (
        set "WATCOM=%USERPROFILE%\watcom"
    )
)

if "%WATCOM%"=="" (
    echo error: Open Watcom v2 not found.
    echo Set WATCOM=C:\path\to\watcom ^(download open-watcom-2_0-c-win-x64.exe or ow-snapshot.tar.xz
    echo from https://github.com/open-watcom/open-watcom-v2/releases/tag/Current-build^).
    exit /b 1
)

if exist "%WATCOM%\binnt64\wcc386.exe" (
    set "HOST_BIN=%WATCOM%\binnt64"
) else if exist "%WATCOM%\binnt\wcc386.exe" (
    set "HOST_BIN=%WATCOM%\binnt"
) else (
    echo error: wcc386.exe not found under %WATCOM%\binnt64 or %WATCOM%\binnt
    exit /b 1
)

set "PATH=%HOST_BIN%;%WATCOM%\binw;%PATH%"
set "INCLUDE=%WATCOM%\h"
set "EDPATH=%WATCOM%\eddat"

set "COMMON_FLAGS=-bt=dos -mf -5r -ox -za99 -zq -i=%ROOT%\include -i=%ROOT%\src\game -i=%WATCOM%\h"

echo === Compiling 32-bit DOS platform layer (Windows -^> DOS32) ===
wcc386 %COMMON_FLAGS% -w3 -we -fo="%BUILD%\dos_hw.obj"       "%ROOT%\src\platform\dos_hw.c"       || exit /b 1
wcc386 %COMMON_FLAGS% -w3 -we -fo="%BUILD%\ega.obj"          "%ROOT%\src\platform\ega.c"          || exit /b 1
wcc386 %COMMON_FLAGS% -w3 -we -fo="%BUILD%\dos_video.obj"    "%ROOT%\src\platform\dos_video.c"    || exit /b 1
wcc386 %COMMON_FLAGS% -w3 -we -fo="%BUILD%\dos_platform.obj" "%ROOT%\src\platform\dos_platform.c" || exit /b 1

for %%E in (1 2 3) do (
    echo === Building COSMO%%E.EXE ^(Episode %%E^) ===
    wcc386 %COMMON_FLAGS% -w0 -dEPISODE=%%E -fi="%ROOT%\include\cosmo\dos_compat.h" -fo="%BUILD%\game1_ep%%E.obj"    "%ROOT%\src\game\game1.c"            || exit /b 1
    wcc386 %COMMON_FLAGS% -w0 -dEPISODE=%%E -fi="%ROOT%\include\cosmo\dos_compat.h" -fo="%BUILD%\game2_ep%%E.obj"    "%ROOT%\src\game\game2.c"            || exit /b 1
    wcc386 %COMMON_FLAGS% -w0 -dEPISODE=%%E -fi="%ROOT%\include\cosmo\dos_compat.h" -fo="%BUILD%\lowlevel_ep%%E.obj" "%ROOT%\src\platform\lowlevel_ega.c" || exit /b 1

    wlink system causeway option quiet option stack=262144 ^
        name "%OUT%\COSMO%%E.EXE" ^
        file "%BUILD%\dos_hw.obj" ^
        file "%BUILD%\ega.obj" ^
        file "%BUILD%\dos_video.obj" ^
        file "%BUILD%\dos_platform.obj" ^
        file "%BUILD%\lowlevel_ep%%E.obj" ^
        file "%BUILD%\game1_ep%%E.obj" ^
        file "%BUILD%\game2_ep%%E.obj" || exit /b 1
)

echo === Built cosmo-smooth 32-bit DOS executables ===
dir /b "%OUT%\COSMO*.EXE"
endlocal
