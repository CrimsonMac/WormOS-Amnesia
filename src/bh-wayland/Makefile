CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
override CFLAGS += -std=gnu17 -Wall -Wextra -I. $(shell $(PKG_CONFIG) --cflags wayland-client)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs wayland-client)

PREFIX   ?= /usr

all: wayland

security-context-v1-client-protocol.h: protocol/security-context-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

security-context-v1-protocol.c: protocol/security-context-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

wayland: wayland.c security-context-v1-client-protocol.h security-context-v1-protocol.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ wayland.c security-context-v1-protocol.c $(LIBS)

install: wayland
	install -Dm755 wayland $(DESTDIR)$(PREFIX)/lib/bh/wayland

clean:
	rm -f wayland security-context-v1-client-protocol.h security-context-v1-protocol.c

.PHONY: all install clean
