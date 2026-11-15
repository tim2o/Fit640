#include "Retro68.r"

type 'INIT' {
	RETRO68_CODE_TYPE
};

/* Loaded into the system heap and locked: the code stays resident. */
resource 'INIT' (128, sysHeap, locked) {
	dontBreakAtEntry, $$read("Fit640.flt");
};

/* Ask for system heap room for the virtual screen and the copy used to
   find what changed: two buffers of 640x480 at 16 bits = 1.2 MB, plus
   slack. */
type 'sysz' {
	longint;
};

resource 'sysz' (0) {
	1400 * 1024
};
