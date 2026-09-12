MODULE_NAME = notifyd

CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 wayland-server)
PKG_CFLAGS = $(shell pkg-config --cflags gio-2.0 wayland-server $(WLROOTS_PC))

OUTDIR ?= .
LIBDIR ?= .

SRC = gowl-module-$(MODULE_NAME).c
OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

$(OUT): $(SRC) $(wildcard ../../src/core/*.h ../../src/interfaces/*.h ../../src/module/*.h) $(LIBDIR)/libgowl.so
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(SRC) $(LDFLAGS) -Wl,-soname,$(MODULE_NAME).so -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
