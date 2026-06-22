# xdg-desktop-portal-gowl -- InputCapture + RemoteDesktop portal backend.
#
# Standalone binary (links no libgowl): a Wayland client that binds the
# private gowl-input-capture protocol and bridges it to a libeis EIS
# server and the freedesktop InputCapture/RemoteDesktop D-Bus backends.
# Built only when libeis-1.0 is present (the parent Makefile gates this).

CC ?= gcc
CSTD = -std=gnu89

PKG_DEPS = glib-2.0 gio-2.0 gio-unix-2.0 wayland-client libeis-1.0
PKG_CFLAGS = $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS = $(shell pkg-config --libs $(PKG_DEPS))

WAYLAND_SCANNER = $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)

OUTDIR ?= .
PROTO_XML = ../../protocols/gowl-input-capture-unstable-v1.xml

SRCS = main.c portal-eis.c portal-wayland.c portal-dbus.c
GEN = gowl-input-capture-v1-client-protocol.h \
      gowl-input-capture-v1-protocol.c
OBJS = $(SRCS:.c=.o) gowl-input-capture-v1-protocol.o
OUT = $(OUTDIR)/xdg-desktop-portal-gowl

all: $(OUT)

# Generated client-side protocol glue.
gowl-input-capture-v1-client-protocol.h: $(PROTO_XML)
	$(WAYLAND_SCANNER) client-header $< $@

gowl-input-capture-v1-protocol.c: $(PROTO_XML) \
                                  gowl-input-capture-v1-client-protocol.h
	$(WAYLAND_SCANNER) private-code $< $@

# Sources depend on the generated client header.
$(SRCS:.c=.o): gowl-input-capture-v1-client-protocol.h

%.o: %.c
	$(CC) $(CSTD) -Wall -Wextra -Wno-unused-parameter -I. \
	    $(PKG_CFLAGS) -c $< -o $@

gowl-input-capture-v1-protocol.o: gowl-input-capture-v1-protocol.c
	$(CC) $(CSTD) -Wall -Wno-unused-parameter -I. $(PKG_CFLAGS) \
	    -c $< -o $@

$(OUT): $(OBJS)
	$(CC) $(CSTD) -o $@ $(OBJS) $(PKG_LIBS)

clean:
	rm -f $(OBJS) $(GEN) $(OUT)

.PHONY: all clean
