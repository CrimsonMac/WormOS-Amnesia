CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-software

wormos-software: software.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ software.c $(LIBS)

install: wormos-software
	install -Dm755 wormos-software $(DESTDIR)$(BINDIR)/wormos-software
	install -Dm644 software.css $(DESTDIR)$(DATADIR)/wormos/software/software.css
	install -Dm644 org.wormos.Software.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Software.desktop
	install -Dm644 org.wormos.Software.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/org.wormos.Software.svg

clean:
	rm -f wormos-software

.PHONY: all install clean
