MODULE_NAME = monocle
CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 $(WLROOTS_PC))
PKG_CFLAGS = $(shell pkg-config --cflags $(WLROOTS_PC))
OUTDIR ?= .
LIBDIR ?= .
SRC = gowl-module-monocle.c gowl-layout-monocle.c
OUT = $(OUTDIR)/$(MODULE_NAME).so
all: $(OUT)
$(OUT): $(SRC) $(wildcard *.h ../../src/core/*.h ../../src/interfaces/*.h) $(LIBDIR)/libgowl.so
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -I../../src/layout -o $@ $(SRC) $(LDFLAGS) -Wl,-soname,monocle.so -L$(LIBDIR) -lgowl $(PKG_LIBS) -lm
clean:
	rm -f $(OUT)
.PHONY: all clean
