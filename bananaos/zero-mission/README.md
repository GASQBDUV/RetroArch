# Metroid: Zero Mission second screen for bananaOS (Anbernic RG DS)

The MetroidArch second screen for Zero Mission, on the **Anbernic RG DS** running **bananaOS**: the game runs in the
device's own, unmodified RetroArch (gpSP core) on the top panel and the **bottom panel** becomes the second screen,
with the tabs of the Android app:

- **Status strip** on every tab: energy tanks and HP (red while below 30), missile / super missile / power bomb
  icons (decoded from your own ROM) with counts. Tap the missile or super missile icon to choose that weapon.
- **MAP**: the area map drawn from the game's own minimap tiles, only as far as you have explored it (or all of it
  once you have downloaded the area's map), Samus as a red dot. Buttons zoom in and out and re-centre; drag to pan.
  **Exit arrows** show where the doors to other areas are; tap one for a note about the destination area.
  **Statue hints** (a pink diamond) mark the Chozo statues whose item you haven't taken. **Long-press** drops a
  flag or removes one; flags are kept per ROM.
- **ITEMS**: item count, play time, and the BEAM / SUIT / MOVEMENT / MISC lists with the suit picture (Power, Varia,
  Gravity or Zero Suit). Things you haven't found yet show as `???` so nothing is spoiled.
- **SETUP**: show or hide the status strip per tab, and **CLEAR MAP MARKERS** (tap twice).
- The panel **pulses red** while energy is below 30, when the game beeps.
- It goes dark with a dimmed METROID logo on the title screen, in menus and in cutscenes.

Nothing in RetroArch or in bananaOS is replaced. The companion is a small library that the launcher script loads into
RetroArch with `LD_PRELOAD`. It is the same program as [`../super-metroid`](../super-metroid) with Zero Mission's
memory map and art.

## Tested on

| | |
|---|---|
| Device | Anbernic RG DS Plus |
| OS | bananaOS **0.5.0.1** (Buildroot, rk3568, glibc 2.41) |
| RetroArch | 1.22.2, the one that ships with bananaOS (KMS/GL) |
| Core | gpSP (`/usr/lib/libretro/gpsp_libretro.so`), the bananaOS default for GBA |
| ROM | `Metroid - Zero Mission (USA).gba`, unzipped |

Not tested: other bananaOS versions, other devices, other regions of the ROM (the art is read from fixed addresses in
the USA ROM), the mGBA core (bananaOS substitutes the core set under Settings > Other systems anyway), ROM hacks,
Metroid Fusion (it has its own build, see [`../fusion`](../fusion)). The unzipped ROM is needed because the map tiles and HUD icons are read
from it.

## Install

1. Get `banana-zm.so`: build it (below), or take it from a release if one is attached to this repository.
2. On the SD card, in the ports folder (`/roms/ports`, which is `/data/roms/ports` on the device):
   - create the folder `zeromission` and put `banana-zm.so` in it;
   - put `Metroid Zero Mission - 2 Screens.sh` next to the folder (not inside it).
3. Put your ROM at `/roms/gba/Metroid - Zero Mission (USA).gba`, or open the `.sh` file and change `ROM=`.
4. Refresh the menu and start **Metroid Zero Mission - 2 Screens** from the ports list. Avoid parentheses in the
   script's name: the bananaOS menu hides text in parentheses.

### Build

On Debian or Ubuntu (this is how the build was made, in an arm64 Debian trixie chroot):

```
apt install gcc-aarch64-linux-gnu libdrm-dev:arm64
sh bananaos/zero-mission/build.sh
```

Or build on the device with `CC=gcc sh build.sh` if gcc and libdrm are installed there.

## Files it writes

All next to the library, in `zeromission/`: `prefs.txt` (tab and settings), `pins-<rom>.txt` (map flags) and `log.txt`
(written by the launcher).

## About your save

The companion reads the game's memory and has exactly one **write**: tapping the missile or super missile icon sets the
lowest bit of the weapon selector (`0x03001417`), the value the game itself changes when you switch weapon. It never
touches the `.srm` or save states. Take a backup of `/data/saves/gba/` and `/data/states/gpSP/` before trying it on a
save you care about.

## How it works, short

- RetroArch's DRM file descriptor is picked up when it first flips a page; the bottom panel (DSI-1) gets its own CRTC
  and buffer and is drawn in software from a thread. The backlight is lit through bananaOS's own `one-panel` helper.
- gpSP only hands out the 256 KB of EWRAM through libretro. The 32 KB of IWRAM, where most of the game's variables live,
  sits 0x20000 bytes below it in the core's data, and that is where it is read. If your core exposes IWRAM itself
  (a 32 KB memory region), that is used instead.
- The core path and ROM path come from RetroArch's command line; touch is read from the bottom panel's Goodix device
  (`/dev/input/event1`).
- The game is only drawn on when its mode is "in game" (`0x03000C70` = 4).

## Credits

This is a port, and most of the design is other people's work.

- **Raekwon1603** and the **[MetroidArch](https://github.com/Raekwon1603/RetroArch)** second screen: the layout,
  colours, tabs, pixel font, room positions, door and statue tables, suit picture data, minimap decoding and the memory
  addresses all come from this fork's Android code, rewritten in C.
- The Zero Mission disassembly work that the ROM addresses rely on (the **Metroid Zero Mission decompilation**).
- **[RetroArch](https://github.com/libretro/RetroArch)** and **[gpSP](https://github.com/libretro/gpsp)**.
- **bananaOS** and the Anbernic RG DS hardware.
- Written with Claude Code (Anthropic) and tested on a real device by GASQBDUV.

No game data is included. The map tiles and HUD icons are read from your own ROM at run time; the suit pictures are
baked in as data from the fork. Licensed under GPL-3.0 like the rest of this repository.
