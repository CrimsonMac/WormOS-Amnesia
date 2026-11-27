CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-editor

wormos-editor: editor.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ editor.c $(LIBS)

install: wormos-editor
	install -Dm755 wormos-editor $(DESTDIR)$(BINDIR)/wormos-editor
	install -Dm644 org.wormos.Editor.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Editor.desktop

clean:
	rm -f wormos-editor

.PHONY: all install clean
