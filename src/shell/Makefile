CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
# gtk4-layer-shell must come before libwayland-client when linking.
PKGS      = gtk4-layer-shell-0 gtk4 gtk4-wayland gio-unix-2.0 wayland-client
override CFLAGS += -std=gnu17 -Wall -Wextra -I. $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

PROTOCOLS = wlr-foreign-toplevel-management-unstable-v1 wlr-output-management-unstable-v1
HEADERS   = $(PROTOCOLS:%=%-client-protocol.h)
PROTO_SRC = $(PROTOCOLS:%=%-protocol.c)
SRCS      = main.c wayland.c toplevels.c outputs.c status.c panel.c launcher.c notify.c \
	    power.c apps.c volume.c config.c
OBJS      = $(SRCS:.c=.o) $(PROTO_SRC:.c=.o)

all: worm-shell

%-client-protocol.h: protocol/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

%-protocol.c: protocol/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

$(SRCS:.c=.o): shell.h $(HEADERS)

worm-shell: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

install: worm-shell
	install -Dm755 worm-shell $(DESTDIR)$(BINDIR)/worm-shell
	install -Dm644 shell.css $(DESTDIR)$(DATADIR)/wormos/shell/shell.css

clean:
	rm -f worm-shell *.o $(HEADERS) $(PROTO_SRC)

.PHONY: all install clean
