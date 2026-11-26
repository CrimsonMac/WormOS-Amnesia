CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-welcome

wormos-welcome: welcome.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ welcome.c $(LIBS)

install: wormos-welcome
	install -Dm755 wormos-welcome $(DESTDIR)$(BINDIR)/wormos-welcome
	install -Dm644 welcome.css $(DESTDIR)$(DATADIR)/wormos/welcome/welcome.css
	install -Dm644 org.wormos.Welcome.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Welcome.desktop

clean:
	rm -f wormos-welcome

.PHONY: all install clean
