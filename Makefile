CC = cc
CFLAGS = -std=c11 -Wall -O2
prefix = /usr/local
PREFIX = $(prefix)

oqtf:
	$(X)$(CC) $(EXTRA) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) oqtf.c -o $@

install: oqtf
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mv -f oqtf $(DESTDIR)$(PREFIX)/bin/oqtf

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/oqtf

clean:
	rm -f oqtf

.PHONY: oqtf install uninstall clean
