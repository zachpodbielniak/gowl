MODULE_NAME = expo
CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 pixman-1 $(WLROOTS_PC))
PKG_CFLAGS = $(shell pkg-config --cflags pixman-1 libdrm $(WLROOTS_PC))
OUTDIR ?= .
LIBDIR ?= .
SRC = gowl-module-expo.c gowl-expo-layout.c
OUT = $(OUTDIR)/$(MODULE_NAME).so
all: $(OUT)
$(OUT): $(SRC) $(wildcard *.h ../../src/core/*.h ../../src/interfaces/*.h) $(LIBDIR)/libgowl.so
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(SRC) $(LDFLAGS) -Wl,-soname,expo.so -L$(LIBDIR) -lgowl $(PKG_LIBS) -lm
clean:
	rm -f $(OUT)
.PHONY: all clean
