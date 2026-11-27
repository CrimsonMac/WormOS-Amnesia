CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4 poppler-glib)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4 poppler-glib)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-viewer

wormos-viewer: viewer.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ viewer.c $(LIBS)

install: wormos-viewer
	install -Dm755 wormos-viewer $(DESTDIR)$(BINDIR)/wormos-viewer
	install -Dm644 org.wormos.Viewer.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Viewer.desktop

clean:
	rm -f wormos-viewer

.PHONY: all install clean
