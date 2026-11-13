#!/bin/bash
# Metroid Fusion on bananaOS with the second screen on the bottom panel.
# Put this file in the ports folder (/roms/ports, which is /data/roms/ports on the device) next to the folder fusion/
# holding banana-fusion.so. Change ROM below if your ROM has another name or place.
D=/data/roms/ports/fusion
ROM="/data/roms/gba/Metroid Fusion (USA).gba"

export LD_PRELOAD=$D/banana-fusion.so
# bananaOS uses the core chosen under Settings > Other systems for GBA (gpSP by default) whatever is named here
# BananaOS 0.5.3 runs the GBA and SNES on BananaFront unless Settings > Other systems > Frontend says RetroArch, and the
# companion hooks RetroArch's DRM calls. So the setting is switched to RetroArch for this game and put back afterwards.
SET=/data/banana/settings.conf
OLDFE=$(sed -n 's/^ra_frontend = //p' $SET 2>/dev/null)
if [ -f $SET ] && grep -q '^ra_frontend = ' $SET; then sed -i 's/^ra_frontend = .*/ra_frontend = retroarch/' $SET
else echo 'ra_frontend = retroarch' >> $SET; OLDFE=auto; fi
BANANA_SYSTEM=gba /usr/libexec/banana/run-retroarch gpsp "$ROM" 2> $D/log.txt
sed -i "s/^ra_frontend = .*/ra_frontend = ${OLDFE:-auto}/" $SET
