@echo off
cd /d C:\Users\Dell\Desktop\switch-lan-play\switch-lan-play\ldn_mitm-1.25.1
set PATH=C:\devkitPro\devkitA64\bin;C:\devkitPro\tools\bin;C:\devkitPro\portlibs\switch\bin;%PATH%
set DEVKITPRO=/opt/devkitpro
set DEVKITARM=/opt/devkitpro/devkitARM
set DEVKITPPC=/opt/devkitpro/devkitPPC
C:\devkitPro\msys2\usr\bin\make.exe -C Atmosphere-libs/libstratosphere 2>&1
echo EXIT_CODE=%ERRORLEVEL%
