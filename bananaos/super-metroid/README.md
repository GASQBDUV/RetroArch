# Super Metroid second screen for bananaOS (Anbernic RG DS)

The MetroidArch second screen, but for the **Anbernic RG DS** running **bananaOS**: Super Metroid runs in the
device's own, unmodified RetroArch (snes9x core) on the top panel, and the **bottom panel** becomes the
second screen, with the same tabs as the Android app:

- **Status strip** on every tab: energy tanks and HP, missile / super missile / power bomb icons (decoded from your
  own ROM) with counts. Tap an ammo icon to select that ammo, tap it again to deselect.
- **MAP**: the current area's map drawn from the game's own pause-screen art, tinted per area, with Samus as a red
  dot, or the **world map** with all areas in their true places, the doors between them and the area names. Buttons
  switch between room and world view and zoom in and out; drag to pan. **Long-press** to drop a flag or remove one;
  flags are kept per ROM and per save slot.
- **ITEMS**: ITEMS percentage (from the game's own tables), play time, the SUIT / MISC / BOOTS / BEAM lists and the
  Samus picture. Things you haven't found yet show as `???` so nothing is spoiled.
- **SETUP**: show or hide the status strip per tab, **HIDE MAIN HUD** (off / blank / cut) and **CLEAR MAP MARKERS**
  (tap twice).
- The panel **pulses red** while energy is below 30, when the game beeps.
- It goes dark with a dimmed METROID logo on the title screen, in menus and in cutscenes.

Nothing in RetroArch or in bananaOS is replaced. The companion is a small library that the launcher script loads
into RetroArch with `LD_PRELOAD`.

## Tested on

| | |
|---|---|
| Device | Anbernic RG DS Plus |
| OS | bananaOS **0.5.0.1** (Buildroot, rk3568, glibc 2.41) |
| RetroArch | 1.22.2, the one that ships with bananaOS (KMS/GL) |
| Core | snes9x (`/usr/lib/libretro/snes9x_libretro.so`) |
| ROM | `Super Metroid (Japan, USA) (En,Ja).sfc`, unzipped |

Not tested: other bananaOS versions, other devices, other cores (the memory and HUD tricks are written for snes9x),
Super Metroid Redux or other hacks, Zero Mission. It needs the unzipped ROM file because the map and item art is read
from it.

## Install

1. Get `banana-metroid.so`: build it (below), or take it from a release if one is attached to this repository.
2. On the SD card, in the ports folder (`/roms/ports`, which is `/data/roms/ports` on the device):
   - create the folder `metroid2s` and put `banana-metroid.so` in it;
   - put `Super Metroid - 2 Screens.sh` next to the folder (not inside it).
3. Put your ROM at `/roms/snes/Super Metroid (Japan, USA) (En,Ja).sfc`, or open the `.sh` file and change `ROM=`.
4. Refresh the menu and start **Super Metroid - 2 Screens** from the ports list.

Avoid parentheses in the script's name: the bananaOS menu hides text in parentheses.

### Build

On Debian or Ubuntu (this is how the release build was made, in an arm64 Debian trixie chroot):

```
apt install gcc-aarch64-linux-gnu libdrm-dev:arm64
sh bananaos/super-metroid/build.sh
```

You can also build on the device itself with `CC=gcc sh build.sh` if gcc and libdrm are installed there.

## Files it writes

All next to the library, in `metroid2s/`: `prefs.txt` (tab and settings), `pins-<rom>-slot<N>.txt` (map flags) and
`log.txt` (written by the launcher).

## About your save

The companion reads the game's memory and has exactly one **write**: tapping an ammo icon sets the selected-ammo
value, the same one the Android app writes. It never touches the `.srm` or save states. Hiding the
HUD changes the picture only on its way to RetroArch, not the game. Still, take a backup of
`/data/saves/Snes9x/` and `/data/states/Snes9x/` before trying it on a save you care about.

## How it works, short

- RetroArch's DRM file descriptor is picked up when it first flips a page; the bottom panel (DSI-1) gets its own CRTC
  and buffer and is drawn in software from a thread. The backlight is lit through bananaOS's own `one-panel` helper.
- The game's RAM comes from the loaded core (`retro_get_memory_data`); the core path and ROM path come from
  RetroArch's command line.
- Touch is read from the bottom panel's Goodix device (`/dev/input/event1`).
- HIDE MAIN HUD finds the core's video callback pointer in its data segment and puts a wrapper in its place that
  blanks or cuts away the top 32 lines while the game is in play. If that pointer is not found, the setting does
  nothing and `log.txt` says `video hook not found`.

## Credits

This is a port, and most of the design is other people's work.

- **Raekwon1603** and the **[MetroidArch](https://github.com/Raekwon1603/RetroArch)** second screen: the layout,
  colours, tabs, 5x7 pixel font, Samus picture data, map decoding, world-map layout, pins and the memory addresses
  all come from this fork's Android code, rewritten in C. That in turn is based on Raekwon1603's
  **[super_metroid-android](https://github.com/Raekwon1603/super_metroid-android)**.
- **[Super Metroid Redux](https://www.romhacking.net/hacks/4963/)** by ShadowOne333: the Samus picture on the ITEMS
  tab (`suit_data.h` is generated from the fork's `ReduxSuitData.java`).
- **[RetroArch](https://github.com/libretro/RetroArch)** and **[snes9x](https://github.com/snes9xgit/snes9x)**.
- **bananaOS** and the Anbernic RG DS hardware.
- Written with Claude Code (Anthropic) and tested on a real device by GASQBDUV.

No game data is included. Every picture except the Samus data is read from your own ROM at run time. Licensed under
GPL-3.0 like the rest of this repository.
