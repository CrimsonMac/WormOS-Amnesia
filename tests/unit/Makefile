# Unit tests for small pieces of WormOS's own code, built and run with the
# build machine's compiler (no image, no QEMU): make -C tests/unit
CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra -Werror -Wno-error=format-truncation -I../../src/common
TESTS = skew

check: $(TESTS)
	@status=0; for t in $(TESTS); do ./$$t || status=1; done; exit $$status

skew: skew.c unit.h ../../src/worm/anon.c ../../src/worm/worm.h
	$(CC) $(CFLAGS) -o $@ skew.c

clean:
	rm -f $(TESTS)

.PHONY: check clean
