#!/bin/bash
# Metroid: Zero Mission on bananaOS with the second screen on the bottom panel.
# Put this file in the ports folder (/roms/ports, which is /data/roms/ports on the device) next to the folder zeromission/
# holding banana-zm.so. Change ROM below if your ROM has another name or place.
D=/data/roms/ports/zeromission
ROM="/data/roms/gba/Metroid - Zero Mission (USA).gba"

export LD_PRELOAD=$D/banana-zm.so
# bananaOS uses the core chosen under Settings > Other systems for GBA (gpSP by default) whatever is named here
BANANA_SYSTEM=gba /usr/libexec/banana/run-retroarch gpsp "$ROM" 2> $D/log.txt
