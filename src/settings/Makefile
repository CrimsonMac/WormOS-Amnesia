CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
PKGS      = gtk4 json-c
override CFLAGS += -std=gnu17 -Wall -Wextra -I../common $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs $(PKGS))

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-settings

wormos-settings: settings.c ../common/sound.c ../common/sound.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ settings.c ../common/sound.c $(LIBS) -lm

install: wormos-settings
	install -Dm755 wormos-settings $(DESTDIR)$(BINDIR)/wormos-settings
	install -Dm644 settings.css $(DESTDIR)$(DATADIR)/wormos/settings/settings.css
	install -Dm644 org.wormos.Settings.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Settings.desktop
	install -Dm644 org.wormos.Settings.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/org.wormos.Settings.svg
	install -Dm644 org.wormos.Settings.About.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/org.wormos.Settings.About.svg

clean:
	rm -f wormos-settings

.PHONY: all install clean
