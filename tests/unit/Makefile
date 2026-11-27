# Unit tests for small pieces of WormOS's own code, built and run with the
# build machine's compiler (no image, no QEMU): make -C tests/unit
CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra -Werror -Wno-error=format-truncation -I../../src/common
TESTS = skew sha256 sig moat
# Monocypher (wh's signatures), for tests that need it: sources from
# scripts/ci-build.sh, or the library where one is installed.
MONO_CFLAGS ?=
MONO_LIBS   ?= -lmonocypher

# Editions built on WormOS add their own tests here (TESTS += ...).
-include local.mk

check: $(TESTS)
	@status=0; for t in $(TESTS); do ./$$t || status=1; done; exit $$status

skew: skew.c unit.h ../../src/worm/anon.c ../../src/worm/worm.h
	$(CC) $(CFLAGS) -o $@ skew.c

moat: moat.c unit.h ../../src/worm/anon.c ../../src/worm/worm.h
	$(CC) $(CFLAGS) -o $@ moat.c

sha256: sha256.c unit.h ../../src/snap/sha256.c ../../src/snap/sha256.h
	$(CC) $(CFLAGS) -o $@ sha256.c ../../src/snap/sha256.c

sig: sig.c unit.h ../../src/wh/sig.c ../../src/wh/sig.h
	$(CC) $(CFLAGS) $(MONO_CFLAGS) -o $@ sig.c ../../src/wh/sig.c $(MONO_LIBS)

clean:
	rm -f $(TESTS)

.PHONY: check clean
