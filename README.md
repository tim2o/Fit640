# Fit640 (proof of concept)

A system extension for 68040-upgraded Color Classics (LC/Performa 550 board,
or the "Mystic" LC 575 board) that makes the Mac believe its main screen is
640×480 while the built-in CRT stays in its stock 512×384 mode, so software
that insists on 640×480 will run without the hardware 640×480 modification.
See [DESIGN.md](DESIGN.md) for the background.

| Crop: a 512×384 window at full size | Scale: the whole 640×480 shrunk |
|---|---|
| ![crop](docs/crop.png) | ![scale](docs/scale.png) |

## Status

Early and experimental. One build covers System 7.5 through Mac OS 8.1.

- **Emulator (Basilisk II, 68040, 512×384):** boots and displays correctly on
  System 7.5.3 and Mac OS 8.1, in 256 and thousands of colors, crop and scale.
- **Real hardware:** loads and runs on a Color Classic (Performa 550 board,
  Sonnet Presto Plus 68040) under System 7.6.1. In thousands of colors the
  first version redrew too slowly to be usable; redraw throttling was added
  after that and has not been tried on real hardware yet. Speed in 256 colors
  on real hardware is not yet measured.
- **Not tested at all:** the hot keys and mouse-follow, any game, a depth
  change while running.

## What it does

- At startup it allocates a 640×480 screen buffer in the system heap, points
  the main screen and the window system's existing ports at it, and tells the
  system the screen is 640×480.
- A screen-refresh task copies part of that buffer to the real screen:
  - **Crop** (default): a 1:1 512×384 window. It starts at the top-left, so
    the menu bar is where you expect, and follows the mouse when the pointer
    reaches an edge.
  - **Scale**: the whole 640×480 picture shrunk to 512×384 (256 colors only).
    Small text suffers.
- At startup it times whole-screen redraws for each method (about 1.5 seconds;
  the picture shrinks briefly during the scale test). From that it sizes the
  redraw work so it takes at most about a third of the processor: a fast
  machine redraws the whole screen 30 times a second, a slow one redraws it in
  slices at a lower frame rate.

Hot keys:

| Keys | Effect |
|---|---|
| Control-Option-S | switch between crop and scale |
| Control-Option-C | lock the crop window on the centre of the screen; again to follow the mouse |
| Control held during startup | avoid the 68040 MOVE16 instruction (see below) |

**Fit640Info** is a small app that shows what screen size programs now see,
the extension's status, and the timing results, and saves the same report to
a text file named `Fit640 Log` next to itself. It can also switch modes: type
S or C, then Return.

Known limits:

- Changing color depth (Monitors control panel, or a game doing it) resets the
  screen to the real 512×384; Fit640 notices and goes dormant until restart.
- **Use 256 colors.** Thousands of colors works but doubles the redraw work.
- 32-bit addressing must be on.
- No control panel, no per-game settings, and it redraws the whole screen
  every time rather than only what changed.

## Install

Test on a system you can afford to break.

1. Set the screen to 256 colors.
2. Copy `Fit640` into the System Folder's Extensions folder. Restart.
3. Run `Fit640Info`.

If something goes wrong:

- **Freeze or garbage during startup:** restart holding **Control** from the
  moment extensions begin loading. That avoids the 68040 MOVE16 instruction,
  the most likely thing an accelerator card might not handle when writing to
  video memory.
- **Anything else:** restart holding **Shift** (extensions off) and remove
  `Fit640` from the Extensions folder.

## Build

Needs the [Retro68](https://github.com/autc04/Retro68) toolchain (expected in
`~/Retro68/build/toolchain`; set `RETRO68` to use another location) and
Python 3.

```bash
./build.sh
```

Output, each holding the `Fit640` extension and the `Fit640Info` app:

- `build/Fit640.sit`: a StuffIt archive in the original 1.5.1 format, stored
  without compression, so any version of StuffIt or StuffIt Expander opens it.
  This is the easiest one to move to a real Mac.
- `build/Fit640.dsk`: a raw 1.4 MB disk image for emulators and SCSI/floppy
  emulators. It has no Disk Copy header, so Disk Copy will not mount it.
- `build/Fit640.bin` and `build/Fit640Info.bin`: the two files in MacBinary form.

## Emulator testing

`tools/emutest.sh` boots a throwaway copy of a system disk image in
Basilisk II (68040, 512×384) with the freshly built extension installed and
the info app in Startup Items, saves a picture of the emulator window, and
prints the info app's report. You supply the ROM and the system disk image;
see the comments at the top of the script.

## Files

| File | What it is |
|---|---|
| `src/Fit640.c` | the extension |
| `src/Blit.s` | the copy and shrink loops, in 68k assembly |
| `src/Fit640.h` | the status block shared with the info app |
| `src/Fit640.r` | resource definitions |
| `info/Fit640Info.c` | the info app |
| `tools/mksit.py` | packs the built files into the `.sit` archive |
| `tools/emutest.sh` | automated emulator test |

## License

MIT; see [LICENSE](LICENSE). Built programs link the Retro68 runtime library,
which is GPLv3 with the GCC Runtime Library Exception.
