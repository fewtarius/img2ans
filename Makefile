CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c99
LDFLAGS ?= -lm

# Version from git tag, or 0.0.0-dev
VERSION := $(shell git describe --tags --match 'v*' --abbrev=0 2>/dev/null | sed 's/^v//')
ifeq ($(VERSION),)
VERSION := 0.0.0-dev
endif
CFLAGS += -DIMG2ANS_VERSION='"$(VERSION)"'

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
MANDIR   = $(PREFIX)/share/man/man1

TARGET   = img2ans
SOURCES  = img2ans.c
HEADERS  = stb_image.h

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 img2ans.1 $(DESTDIR)$(MANDIR)/img2ans.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/img2ans.1

clean:
	rm -f $(TARGET) *.o

debug: CFLAGS = -g -DDEBUG -Wall -Wextra -std=c99
debug: $(TARGET)
