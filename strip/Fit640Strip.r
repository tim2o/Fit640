/* The module's code, as the 'sdev' resource the Control Strip loads. */
data 'sdev' (0, sysHeap, locked) {
	$$read("Fit640Strip.flt")
};
