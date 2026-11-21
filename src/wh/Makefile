CC      ?= cc
CFLAGS  ?= -O2
# override: these must survive a CFLAGS given on the command line.
override CFLAGS += -std=gnu17 -Wall -Wextra -I../snap -I../common
LDFLAGS += -Wl,-z,relro,-z,now

# Monocypher: the static library by default. A build for the build host
# passes its sources instead (see tools/wrepo).
MONO_CFLAGS ?=
MONO_LIBS   ?= -lmonocypher

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

SRCS = wh.c repo.c db.c pkg.c foreign.c hooks.c util.c sig.c ../snap/sha256.c

all: wh whc keep-accounts user

wh: $(SRCS) wh.h sig.h
	$(CC) $(CFLAGS) $(MONO_CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(MONO_LIBS)

keep-accounts: keep-accounts.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ keep-accounts.c

user: user.c wh.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ user.c

whc: whc.c ../common/ns.c ../common/ns.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ whc.c ../common/ns.c

install: wh whc keep-accounts user
	install -Dm755 wh $(DESTDIR)$(BINDIR)/wh
	install -Dm755 whc $(DESTDIR)$(BINDIR)/whc
	install -Dm755 keep-accounts $(DESTDIR)$(PREFIX)/lib/wh/keep-accounts
	install -Dm4755 user $(DESTDIR)$(PREFIX)/lib/wh/user
	install -Dm644 keep-accounts.hook $(DESTDIR)$(PREFIX)/share/wh/hooks/keep-accounts.hook

clean:
	rm -f wh whc keep-accounts user

.PHONY: all install clean
