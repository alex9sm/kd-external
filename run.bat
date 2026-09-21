@echo off
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo Requesting admin privileges...
    powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

set "REPO=%~dp0"
set "BUILD=%REPO%build"

if not exist "%BUILD%\kernelmode.sys" (
    echo build\kernelmode.sys not found. Rebuild the driver first.
    pause
    exit /b 1
)
if not exist "%BUILD%\kdmapper_Release.exe" (
    echo build\kdmapper_Release.exe not found.
    pause
    exit /b 1
)
if not exist "%BUILD%\usermode.exe" (
    echo build\usermode.exe not found. Rebuild usermode first.
    pause
    exit /b 1
)

cd /d "%BUILD%"

echo === mapping driver ===
kdmapper_Release.exe kernelmode.sys
if errorlevel 1 (
    echo kdmapper reported failure.
    pause
    exit /b 1
)

echo.
echo === updating offsets ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO%update_offsets.ps1"

echo.
echo === starting usermode ===
usermode.exe

pause
endlocal
