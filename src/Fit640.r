#include "Retro68.r"

type 'INIT' {
	RETRO68_CODE_TYPE
};

/* Loaded into the system heap and locked: the code stays resident. */
resource 'INIT' (128, sysHeap, locked) {
	dontBreakAtEntry, $$read("Fit640.flt");
};

/* Ask for system heap room for the virtual screen
   (640x480 at 16 bits = 600K, plus slack). */
type 'sysz' {
	longint;
};

resource 'sysz' (0) {
	700 * 1024
};
