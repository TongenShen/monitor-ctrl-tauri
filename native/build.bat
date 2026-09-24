@echo off
REM ===================================================================
REM  build.bat - 编译 monitor_rpc.exe (DDC/CI 后端)
REM
REM  依赖: MinGW-w64 gcc (需在 PATH 中, 或设置 MINGW 环境变量)
REM  产物: monitor_rpc.exe
REM ===================================================================
setlocal

set CC=gcc
if not "%MINGW%"=="" set CC=%MINGW%\bin\gcc.exe

set SRC=monitor_rpc.c monitor_core.c vcp_code.c mjson.c
set LIBS=-ldxva2 -luser32
set CFLAGS=-std=c99 -Wall -Wextra -O2

echo [build] %CC% %CFLAGS%
%CC% %CFLAGS% -o monitor_rpc.exe %SRC% %LIBS%
if errorlevel 1 (
    echo.
    echo [build] FAILED
    exit /b 1
)

echo [build] OK -^> monitor_rpc.exe
endlocal
