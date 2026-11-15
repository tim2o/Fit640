# Fit640 — design notes (working name)

> These are the original planning notes. The README describes what the
> current build actually does; where the two differ, the README is right.

A System 7.5–8.1 extension that gives a 68040-upgraded Color Classic a
**virtual 640×480 main screen**, scaled ×0.8 onto the built-in 512×384 CRT,
so games that refuse to run below 640×480 will launch and play — with no
logic board or analog board modification.

## Target machines

| Setup | Board | CPU | Notes |
|---|---|---|---|
| Color Classic + LC/Performa 550 board (+ Sonnet Presto Plus) | LC 550 / Color Classic II | 68040 w/ FPU (via Presto) | primary dev/test machine |
| Color Classic + "Mystic" upgrade | LC 575 | 68LC040 33 MHz | second target |

Both: built-in CRT at 512×384, 8-bit color, Mac OS 7.5–8.1, 68040 MMU and
copyback cache available. Stock 68030 Color Classics are explicitly out of
scope (too slow); 512×342 compact Macs are out of scope.

## Why software only (and why we never touch video timing)

The 550/575 boards *can* generate 640×480 timing, but the Color Classic CRT
needs a raised deflection voltage to display it — that's what the analog-board
half of the hardware mod is for. Driving 640×480 timing into an unmodified
analog board is not something software should ever do. Fit640 leaves the real
display in its stock 512×384 mode and only lies to software about the size.

## Architecture

### 1. Virtual screen (the lie)
At INIT time, before the Window Manager and Finder come up:

1. Check: 68040 present, main screen is 512×384, extension enabled, Shift not held.
2. Allocate the virtual framebuffer in the system heap
   (640×480 × depth: 300 KB at 8-bit, 600 KB at 16-bit).
3. Retarget the main GDevice: `gdRect`, `gdPMap` bounds / rowBytes / baseAddr.
4. Update low memory that QuickDraw and the cursor read:
   `ScrnBase`, `ScreenRow`, cursor pin rect, mouse bounds.
5. Leave `gdRefNum` pointing at the real video driver, so palette calls
   (`SetEntries`, gamma) still reach the real CLUT unchanged.

Result: every application — including the Finder — sees a genuine 640×480
main device. This is the approach Stepping Out used; doing it before
`InitWindows` avoids having to resize `GrayRgn` / WMgrPort after the fact.

v1 is **boot-time on/off** (control panel toggle, takes effect at restart),
like the hardware mod's switch. See "Later" for live switching.

### 2. Scaler (the presentation)
Copies the virtual buffer to real VRAM at ×0.8 (5 source px → 4 dest px,
5 source rows → 4 dest rows).

- Driven by a VBL task on the built-in video slot; does work every Nth VBL
  (configurable; default every 2nd ≈ 30 fps).
- Hand-written 68040 inner loop: read 5 bytes, write 4, unrolled per row;
  precomputed row map, no division.
- **Dirty-rect tracking** to avoid full-frame work where possible:
  patch the QuickDraw bottlenecks that put pixels on screen (`StdBits`/CopyBits,
  `CopyMask`, shape drawing via the grafProcs) and accumulate a dirty region in
  virtual-screen coordinates. Games that write straight to the framebuffer
  defeat this, so a per-app "always full frame" fallback exists.
- Interrupt-time budget: a full frame is ~196 K output pixels. Estimate on a
  25 MHz-bus 040 is tens of ms, too long to do every VBL at interrupt level.
  The scaler therefore processes in horizontal bands across consecutive VBLs
  when the dirty area is large (accepting some tearing over stalling sound and
  serial interrupts). **Measuring this on real hardware is milestone 1.**

### 2b. Crop presenter (alternative to the scaler)
Shows a 1:1 512×384 window of the 640×480 virtual screen instead of shrinking.

- Present = 384 row copies of 512 bytes (8-bit), using `MOVE16` on the 040
  (source offset 64 bytes, destination row start — both 16-byte aligned).
  No per-pixel work; dirty rects map 1:1.
- VRAM write volume is the **same** as the scaler (one 512×384 frame), so the
  saving is the CPU gather work only — expect roughly 2–3×, not 10×.
- Loses 128 px horizontally and 96 px vertically. Viewport position is
  per-app: centre, anchored (e.g. right-anchored to keep Escape Velocity's
  status panel visible), or follows the mouse / a hotkey (pan).
- The cursor can still move into the hidden border; the viewport should pan
  to keep it visible so off-screen UI stays clickable.

### 3. Cursor
QuickDraw's software cursor draws into the virtual buffer and gets scaled with
everything else (cursor is 16×16 → ~13×13). Mouse coordinates are already in
virtual space because the cursor pin rect is 640×480; mouse movement feels
×1.25 faster, which is acceptable. Watch for cursor flicker caused by sampling
while QuickDraw has it hidden.

### 4. Quality modes
- Nearest-neighbour (default, fastest): drops every 5th row/column; thin lines
  and small text can vanish.
- Blended (optional, later): average via a 64 KB index×index → index table
  rebuilt whenever the CLUT changes. Slower; best for slow games.

### 5. Configuration
Control panel (or resource in the extension) with:
enabled, frame divisor, quality mode, per-app "full frame" list.

## Compatibility risks

- Games that query the video driver/slot directly for modes or VRAM address.
- Games that switch depth (8 → 16-bit). v1 supports only the depth active at
  boot; the virtual buffer must be reallocated on depth change.
- Display Manager-aware games that try to switch resolution themselves.
- Games doing direct framebuffer writes get correct output, but always full-frame cost.
- VM / RAM Doubler interaction with a 300–600 KB system-heap buffer.

## Milestones

1. **Proof of concept**: INIT that retargets the main GDevice and scales full
   frames on VBL. Measure fps on the 550+Presto and Mystic machines with the
   Finder and with Escape Velocity (1996).
2. Banded scaling + dirty-rect tracking via QuickDraw patches.
3. Control panel + per-app settings.
4. Depth changes; blended quality mode.

### Later (v2 ideas)
- **Live switching**: wrap the built-in video driver to advertise an extra
  "640×480 (scaled)" mode through the Display Manager, so the Monitors control
  panel — and games that use the Display Manager — can switch to it on the fly.
- **MMU dirty tracking**: write-protect the virtual framebuffer's pages on the
  040 and catch the first write per page per frame. This gives exact dirty
  regions even for direct-to-screen games. Powerful, but invasive.

## Toolchain (proposed)

- **Retro68** (GCC cross-compiler for classic Mac, builds INIT/cdev code
  resources) on the modern Mac.
- Test loop in an emulator with a 68040 and a 512×384 8-bit screen
  (Basilisk II allows a custom screen size; QEMU `q800` is another option),
  then real hardware.
- Copy builds to the Color Classic via disk image / BlueSCSI / AppleShare.
