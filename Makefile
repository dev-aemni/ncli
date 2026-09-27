CC ?= clang
CFLAGS ?= -Wall -Wextra -O2
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET = ncli
SRC = ncli.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
