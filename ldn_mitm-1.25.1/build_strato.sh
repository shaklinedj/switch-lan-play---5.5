#!/bin/bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/opt/devkitpro/portlibs/switch/bin:$PATH
cd /home/Dell/Desktop/switch-lan-play/switch-lan-play/ldn_mitm-1.25.1
make -C Atmosphere-libs/libstratosphere -j4 2>&1
echo "BUILD_EXIT=$?"