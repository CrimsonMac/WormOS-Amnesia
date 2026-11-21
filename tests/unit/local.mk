# WormOS Amnesia's unit tests (included by tests/unit/Makefile).
TESTS += newer

newer: newer.c unit.h ../../src/amnesia/update.c
	$(CC) $(CFLAGS) -Wno-unused-function -I../../src/snap -I../../src/wh $(MONO_CFLAGS) -o $@ \
		newer.c ../../src/wh/sig.c ../../src/snap/sha256.c $(MONO_LIBS)
