# Metroid Fusion second screen for bananaOS (Anbernic RG DS)

The MetroidArch second screen for Metroid Fusion, on the **Anbernic RG DS** running **bananaOS**: the game runs in the
device's own, unmodified RetroArch (gpSP core) on the top panel and the **bottom panel** becomes the second screen:

- **Status strip** on every tab: energy tanks and HP (red while below 30), and the missile (`MIS`) and power bomb (`PB`)
  counts once you have them.
- **MAP**: the game's own sector map, drawn with the game's own map tiles and colours (read from your ROM), as the
  game shows it: only the rooms it has revealed, the pink **objective path**, elevator numbers and the **security
  levels L1-L4** (OPEN / LOCK) in the corner, Samus blinking yellow.
  - **TARGET**: a pulsing ring around the objective. Can be turned off under SETUP.
  - Buttons zoom in and out and re-centre; drag to pan.
  - Tap the **SECTOR** box to browse the sectors (Main Deck, SRX, TRO, PYR, AQA, ARC, NOC). The game only keeps the
    sector you are in, so the others are remembered from the last time you were in them.
  - The header also shows the explored percentage and the play time.
- **ITEMS**: the BEAM / MISSILE / BOMB / SUIT / MISC. lists with the suit picture. Things you haven't found yet show
  as `???` so nothing is spoiled.
- **SETUP**: show or hide the status strip per tab, and the **MAP TARGET MARKER**.
- The panel **pulses red** while energy is below 30, when the game beeps.
- It goes dark with a dimmed METROID logo on the title screen, in menus and in cutscenes.

Nothing in RetroArch or in bananaOS is replaced. The companion is a small library that the launcher script loads into
RetroArch with `LD_PRELOAD`. It is the same program as [`../zero-mission`](../zero-mission) and
[`../super-metroid`](../super-metroid) with Fusion's memory map and art.

## Tested on

| | |
|---|---|
| Device | Anbernic RG DS Plus |
| OS | bananaOS **0.5.0.1** (Buildroot, rk3568, glibc 2.41) |
| RetroArch | 1.22.2, the one that ships with bananaOS (KMS/GL) |
| Core | gpSP (`/usr/lib/libretro/gpsp_libretro.so`), the bananaOS default for GBA |
| ROM | `Metroid Fusion (USA).gba`, unzipped |

Not tested: other bananaOS versions, other devices, other regions or revisions of the ROM, the mGBA core, ROM hacks.
The **map needs the USA ROM**: the map tiles and palettes are read from fixed addresses in it (`0x561FA8` and
`0x5657A8`, checked at start; with another ROM the MAP tab says `ROM NOT SUPPORTED` and the rest still works). The
unzipped ROM is needed because the map tiles are read from it.

Known gaps:
- The item bits follow the layout on [Data Crystal](https://datacrystal.tcrf.net/wiki/Metroid_Fusion/RAM_map); the
  beam, suit and missile lists have not been checked item by item against a full playthrough.
- The explored percentage (`0x0300003B` plus the sector number) comes from the same page and is unverified.
- Doors are not drawn on the map, and the game's own `OBJECTIVE` caption is not copied.
- How the locks L1-L4 relate to the security level is read from two states (none, and all open); the steps in between
  have not been seen.

## Install

1. Get `banana-fusion.so`: build it (below), or take it from a release if one is attached to this repository.
2. On the SD card, in the ports folder (`/roms/ports`, which is `/data/roms/ports` on the device):
   - create the folder `fusion` and put `banana-fusion.so` in it;
   - put `Metroid Fusion - 2 Screens.sh` next to the folder (not inside it).
3. Put your ROM at `/roms/gba/Metroid Fusion (USA).gba`, or open the `.sh` file and change `ROM=`.
4. Refresh the menu and start **Metroid Fusion - 2 Screens** from the ports list. Avoid parentheses in the script's
   name: the bananaOS menu hides text in parentheses.

### Build

On Debian or Ubuntu (this is how the build was made, in an arm64 Debian trixie chroot):

```
apt install gcc-aarch64-linux-gnu libdrm-dev:arm64
sh bananaos/fusion/build.sh
```

Or build on the device with `CC=gcc sh build.sh` if gcc and libdrm are installed there.

## Files it writes

All next to the library, in `fusion/`: `prefs.txt` (tab and settings), `maps2.bin` (the sector maps and the objective
position, remembered per sector) and `log.txt` (written by the launcher). Delete `maps2.bin` to forget the remembered
sectors.

## About your save

The companion only **reads** the game's memory. It never writes to it, and never touches the `.srm` or save states.

## How it works, short

- RetroArch's DRM file descriptor is picked up when it first flips a page; the bottom panel (DSI-1) gets its own CRTC
  and buffer and is drawn in software from a thread. The backlight is lit through bananaOS's own `one-panel` helper.
- gpSP only hands out the 256 KB of EWRAM through libretro. The 32 KB of IWRAM, where most of the game's variables live,
  sits 0x20000 bytes below it in the core's data, and that is where it is read.
- The core path and ROM path come from RetroArch's command line; touch is read from the bottom panel's Goodix device
  (`/dev/input/event1`).
- **The map** is read the way the game draws it, not rebuilt:
  - `0x02034000` is a 32x32 tilemap of what the game shows (entry = `x + y * 32`, bits 0-9 tile, bit 10 / 11 flips,
    bits 12-15 palette). The objective path is in it as tiles in palette 1.
  - `0x02034800` is the whole sector, hidden rooms included; only its small label tiles (elevator numbers) are used.
  - The 8x8 4bpp tile graphics and the 16-colour palettes come from the ROM. The position of Samus is `0x03000031/32`.
  - The security levels come from `0x0300131D` (255 = none).
- **The objective ring** is a sprite, not a map tile. Its position is in a list at `0x03001690` (x, y, 0, type), the
  objective first and Samus second, and that list only holds real data while the game's own map screen is open
  (`0x03000BDE` = 3). The companion picks it up then and remembers it per sector; it is dropped when the story event
  counter (`0x03000B87`) changes, and picked up again the next time you open the game's map.
- The game is only drawn on when it is running a level (energy and its maximum make sense and the area is a real one).

## Credits

This is a port, and most of the design is other people's work.

- **Raekwon1603** and the **[MetroidArch](https://github.com/Raekwon1603/RetroArch)** second screen: the layout,
  colours, tabs and pixel font all come from this fork's Android code, rewritten in C.
- **[Data Crystal](https://datacrystal.tcrf.net/wiki/Metroid_Fusion/RAM_map)**, whose Metroid Fusion RAM map gave most
  of the memory addresses.
- **[RetroArch](https://github.com/libretro/RetroArch)** and **[gpSP](https://github.com/libretro/gpsp)**.
- **bananaOS** and the Anbernic RG DS hardware.
- Written with Claude Code (Anthropic) and tested on a real device by GASQBDUV.

No game data apart from one picture is included. The map tiles and palettes are read from your own ROM at run time. The
exception is `fz_suit.h`, the green suit wireframe on the ITEMS tab, which was traced from the game's own status screen
and is baked in as data. Licensed under GPL-3.0 like the rest of this repository.
