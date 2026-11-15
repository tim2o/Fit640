#!/bin/sh
# Boots a throwaway copy of the test system in Basilisk II with the freshly
# built Fit640 files installed, grabs a picture of the emulator window, then
# stops the emulator and prints the report the Info app saved.
#
#   tools/emutest.sh [init|noinit] [seconds] [depth] [variant]
#
# variant is any combination of the words scale, slow and depth, e.g.
# "scaleslow": they drop test-hook files next to the Info app.
# Environment: BASE (system image), CPU (4 = 68040, 3 = 68030), ROM and
# MODELID (a 68030 needs a 68030-era ROM, e.g. ROM=maciici.rom MODELID=5).
#
# Expects in $EMU (default ~/Retro68/emu/test): quadra650.rom, winid and the
# system disk image named by $BASE (default base753.dsk).
set -e
MODE="${1:-init}"
SECS="${2:-50}"
DEPTH="${3:-8}"
VARIANT="${4:-crop}"
EMU="${EMU:-$HOME/Retro68/emu/test}"
BASE="${BASE:-base753.dsk}"
CPU="${CPU:-4}"            # 4 = 68040, 3 = 68030
ROM="${ROM:-quadra650.rom}"
MODELID="${MODELID:-14}"   # 14 = Quadra 900, 5 = Mac IIci
BIN="${RETRO68:-$HOME/Retro68/build/toolchain}/bin"
BASILISK="$HOME/Retro68/emu/macemu/BasiliskII/src/Unix/BasiliskII"
BUILD="$(cd "$(dirname "$0")/../build" && pwd)"

stop_emulator() {
    pkill BasiliskII 2>/dev/null || true
    n=0
    while pgrep -q BasiliskII && [ $n -lt 50 ]; do sleep 0.2; n=$((n+1)); done
    pkill -9 BasiliskII 2>/dev/null || true
}

stop_emulator
rm -f "$EMU/run.dsk"
cp -c "$EMU/$BASE" "$EMU/run.dsk" 2>/dev/null || cp "$EMU/$BASE" "$EMU/run.dsk"
"$BIN/hmount" "$EMU/run.dsk" > /dev/null
"$BIN/hdel" ":System Folder:Startup Items:Energy Saver" 2>/dev/null || true
if [ "$MODE" = "init" ]; then
    "$BIN/hcopy" -m "$BUILD/Fit640.bin" ":System Folder:Extensions:"
fi
"$BIN/hcopy" -m "$BUILD/Fit640Info.bin" ":System Folder:Startup Items:"
: > "$EMU/empty.txt"
case "$VARIANT" in *scale*)
    "$BIN/hcopy" -r "$EMU/empty.txt" ":System Folder:Startup Items:Fit640 Scale" ;;
esac
case "$VARIANT" in *depth*)
    "$BIN/hcopy" -r "$EMU/empty.txt" ":System Folder:Startup Items:Fit640 Depth" ;;
esac
case "$VARIANT" in *slow*)
    "$BIN/hcopy" -r "$EMU/empty.txt" ":System Folder:Startup Items:Fit640 Slow" ;;
esac
"$BIN/humount"

cat > "$EMU/prefs" <<PREFS
rom $EMU/$ROM
disk $EMU/run.dsk
screen win/512/384
displaycolordepth $DEPTH
ramsize 33554432
cpu $CPU
fpu true
modelid $MODELID
nosound true
nogui true
frameskip 1
ignoresegv true
PREFS

"$BASILISK" --config "$EMU/prefs" > "$EMU/basilisk.log" 2>&1 &
sleep "$SECS"
WID="$("$EMU/winid" 2>/dev/null || true)"
SHOT="$EMU/shot-${BASE%.dsk}-cpu$CPU-$MODE-$DEPTH-$VARIANT.png"
if [ -n "$WID" ] && screencapture -x -o -l "$WID" "$SHOT" 2>/dev/null; then
    echo "Picture: $SHOT"
else
    echo "(no picture: could not capture the emulator window)"
fi
pgrep -q BasiliskII || echo "Basilisk II had already quit"
stop_emulator
sleep 1
"$BIN/hmount" "$EMU/run.dsk" > /dev/null
echo "---- Fit640 Log ----"
"$BIN/hcopy" -t ":System Folder:Startup Items:Fit640 Log" - 2>/dev/null || echo "(no log file)"
"$BIN/humount"
