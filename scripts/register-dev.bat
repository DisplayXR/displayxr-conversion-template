@echo off
REM Copyright 2026, The DisplayXR Project
REM SPDX-License-Identifier: BSL-1.0
REM
REM Register the template provider with a DisplayXR runtime on a DEV box, the
REM way an installer would (conversion-provider-discovery.md section 3.1 / 3.8):
REM   HKLM\Software\DisplayXR\ConversionProviders\example-conversion
REM     Binary, DisplayName, Vendor, ProbeOrder, then Version LAST.
REM
REM   scripts\register-dev.bat [register] [path\to\DisplayXR-ExampleConversion.dll] [--probe-order N]
REM   scripts\register-dev.bat unregister
REM
REM Needs an elevated prompt (HKLM). Then, from a NON-elevated prompt:
REM   displayxr-cli conv list        (selection order, why each row was kept or dropped)
REM   displayxr-cli conv status      (readiness)
REM   displayxr-cli selftest         (conversion_providers check)
REM
REM This is not an installer: it does not take the maintenance mutex or run
REM Restart Manager. See README.md "Updating a registered provider".
REM
REM VENDOR TODO: change ID / NAME / VENDOR / the DLL name to yours.

setlocal EnableExtensions
set "ID=example-conversion"
set "NAME=Example Conversion Provider (template)"
set "VENDOR=Example"
set "DLLNAME=DisplayXR-ExampleConversion.dll"
set "ORDER=50"
set "KEY=HKLM\Software\DisplayXR\ConversionProviders\%ID%"

net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: run this from an elevated prompt ^(it writes HKLM^).
    exit /b 1
)

set "MODE=%~1"
if /I "%MODE%"=="unregister" goto :unregister
if /I "%MODE%"=="register" (
    set "DLL=%~2"
    set "OPT=%~3"
    set "OPTV=%~4"
) else (
    set "DLL=%~1"
    set "OPT=%~2"
    set "OPTV=%~3"
)
if /I "%DLL%"=="--probe-order" (
    set "OPT=%DLL%"
    set "OPTV=%OPT%"
    set "DLL="
)
if /I "%OPT%"=="--probe-order" (
    if "%OPTV%"=="" (
        echo ERROR: --probe-order needs a number.
        exit /b 1
    )
    set "ORDER=%OPTV%"
)

if "%DLL%"=="" (
    if exist "%~dp0..\build\Release\%DLLNAME%" (
        set "DLL=%~dp0..\build\Release\%DLLNAME%"
    ) else (
        set "DLL=%~dp0..\build\%DLLNAME%"
    )
)
if not exist "%DLL%" (
    echo ERROR: %DLL% not found. Build first: cmake -S . -B build ^&^& cmake --build build --config Release
    exit /b 1
)
REM Absolute path with backslashes: the runtime hands it to LoadLibraryExW.
for %%F in ("%DLL%") do set "DLL=%%~fF"

reg add "%KEY%" /reg:64 /v Binary      /t REG_SZ    /d "%DLL%"    /f >nul || goto :regfail
reg add "%KEY%" /reg:64 /v DisplayName /t REG_SZ    /d "%NAME%"   /f >nul || goto :regfail
reg add "%KEY%" /reg:64 /v Vendor      /t REG_SZ    /d "%VENDOR%" /f >nul || goto :regfail
reg add "%KEY%" /reg:64 /v ProbeOrder  /t REG_DWORD /d %ORDER%    /f >nul || goto :regfail
REM Version LAST: the "row complete" marker the service re-describes on.
reg add "%KEY%" /reg:64 /v Version     /t REG_SZ    /d "dev"      /f >nul || goto :regfail
echo Registered conversion provider '%ID%' (ProbeOrder %ORDER%):
echo   %DLL%
echo Settings go under %KEY%\Settings (see examples\settings\example-conversion.reg).
exit /b 0

:unregister
reg delete "%KEY%" /reg:64 /f >nul 2>&1
echo Removed conversion provider registration '%ID%'.
exit /b 0

:regfail
echo ERROR: writing %KEY% failed.
exit /b 1
