@echo off
setlocal
cd /d %~dp0

set PATH=C:\devkitPro\devkitA64\bin;C:\devkitPro\tools\bin;C:\devkitPro\portlibs\switch\bin;%PATH%
set DEVKITPRO=/opt/devkitpro
set DEVKITARM=/opt/devkitpro/devkitARM
set DEVKITPPC=/opt/devkitpro/devkitPPC

echo [1/3] Building libstratosphere...
C:\devkitPro\msys2\usr\bin\make.exe -C Atmosphere-libs/libstratosphere -j4
if errorlevel 1 goto :fail

echo [2/3] Building ldn_mitm NSP...
C:\devkitPro\msys2\usr\bin\make.exe -C ldn_mitm -j4
if errorlevel 1 goto :fail

echo [3/3] Packing SD layout...
C:\devkitPro\msys2\usr\bin\make.exe PACK
if errorlevel 1 goto :fail

echo BUILD_OK
exit /b 0

:fail
echo BUILD_FAIL errorlevel=%errorlevel%
exit /b %errorlevel%
