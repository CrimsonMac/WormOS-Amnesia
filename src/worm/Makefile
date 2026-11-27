CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra -I../common
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: worm power snapshots install-helper install-system passwd powerkey guest-agent settings-helper software-helper elevate checkpass

OBJS = worm.o fw.o panic.o status.o anon.o kit.o time.o account.o vm.o

worm: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): worm.h

# The setuid helpers for the desktop.
power: power.c admin.c admin.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ power.c admin.c ../common/password.c -lcrypt

snapshots: snapshots.c admin.c admin.h ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ snapshots.c admin.c ../common/password.c -lcrypt

# The QEMU guest agent service.
guest-agent: guest-agent.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ guest-agent.c

# System settings for the desktop (the Settings program).
settings-helper: settings.c admin.c admin.h ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ settings.c admin.c ../common/password.c -lcrypt

# Package changes for the Software program.
software-helper: software.c admin.c admin.h ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ software.c admin.c ../common/password.c -lcrypt

# Root for the root terminal and sudo, with the administrator's own password.
elevate: elevate.c admin.c admin.h ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ elevate.c admin.c ../common/password.c -lcrypt

# The power button service.
powerkey: powerkey.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ powerkey.c

install-helper: install.c admin.c admin.h ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ install.c admin.c ../common/password.c -lcrypt

# The lock screen checking its user's own password.
checkpass: checkpass.c ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ checkpass.c ../common/password.c -lcrypt

# Users changing their own password (worm passwd).
passwd: passwd.c ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ passwd.c ../common/password.c -lcrypt

# The installer's backend, for the live system.
install-system: install-system.c admin.c admin.h ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ install-system.c admin.c ../common/password.c -lcrypt

install: worm power snapshots install-helper install-system passwd powerkey guest-agent settings-helper software-helper elevate checkpass
	install -Dm755 worm $(DESTDIR)$(BINDIR)/worm
	install -Dm4755 power $(DESTDIR)$(PREFIX)/lib/worm/power
	install -Dm4755 snapshots $(DESTDIR)$(PREFIX)/lib/worm/snapshots
	install -Dm4755 install-helper $(DESTDIR)$(PREFIX)/lib/worm/install
	install -Dm4755 install-system $(DESTDIR)$(PREFIX)/lib/worm/install-system
	install -Dm4755 passwd $(DESTDIR)$(PREFIX)/lib/worm/passwd
	install -Dm4755 checkpass $(DESTDIR)$(PREFIX)/lib/worm/checkpass
	install -Dm755 powerkey $(DESTDIR)$(PREFIX)/lib/worm/powerkey
	install -Dm755 guest-agent $(DESTDIR)$(PREFIX)/lib/worm/guest-agent
	install -Dm4755 settings-helper $(DESTDIR)$(PREFIX)/lib/worm/settings
	install -Dm4755 software-helper $(DESTDIR)$(PREFIX)/lib/worm/software
	install -Dm4755 elevate $(DESTDIR)$(PREFIX)/lib/worm/elevate

clean:
	rm -f worm power snapshots install-helper install-system passwd powerkey guest-agent settings-helper software-helper elevate checkpass $(OBJS)

.PHONY: all install clean
