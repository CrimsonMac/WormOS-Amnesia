CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu17 -Wall -Wextra -D_FORTIFY_SOURCE=3 -fstack-protector-strong
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
LIBDIR  ?= $(PREFIX)/lib/wormrd

all: wormrd mkinitramfs

wormrd: wormrd.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

mkinitramfs: mkinitramfs.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lzstd

install: wormrd mkinitramfs
	install -Dm755 wormrd $(DESTDIR)$(LIBDIR)/init
	install -Dm755 mkinitramfs $(DESTDIR)$(LIBDIR)/mkinitramfs
	install -Dm644 initramfs.hook $(DESTDIR)$(PREFIX)/share/wh/hooks/initramfs.hook

clean:
	rm -f wormrd mkinitramfs

.PHONY: all install clean
