#!/bin/bash
# Super Metroid on bananaOS with the second screen on the bottom panel.
# Put this file in the ports folder (/roms/ports, which is /data/roms/ports on the device) next to the folder metroid2s/
# holding banana-metroid.so. Change ROM below if your ROM has another name or place.
D=/data/roms/ports/metroid2s
ROM="/data/roms/snes/Super Metroid (Japan, USA) (En,Ja).sfc"

export LD_PRELOAD=$D/banana-metroid.so
BANANA_SYSTEM=snes /usr/libexec/banana/run-retroarch snes9x "$ROM" 2> $D/log.txt
