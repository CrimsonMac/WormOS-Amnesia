CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-files

wormos-files: files.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ files.c $(LIBS)

install: wormos-files
	install -Dm755 wormos-files $(DESTDIR)$(BINDIR)/wormos-files
	install -Dm644 files.css $(DESTDIR)$(DATADIR)/wormos/files/files.css
	install -Dm644 org.wormos.Files.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Files.desktop

clean:
	rm -f wormos-files

.PHONY: all install clean
