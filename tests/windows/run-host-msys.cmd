@echo off
setlocal

set "MSYS_BIN=C:\msys64\usr\bin"
if not exist "%MSYS_BIN%\msys-2.0.dll" (
    echo MSYS runtime not found at %MSYS_BIN% 1>&2
    exit /b 1
)

set "PATH=%MSYS_BIN%;%PATH%"

if "%~1"=="" (
    echo Usage: run-host-msys.cmd command [args ...] 1>&2
    echo Example: run-host-msys.cmd .\build\host-msys-posix-x86_64\ls.exe 1>&2
    exit /b 2
)

%*
exit /b %ERRORLEVEL%