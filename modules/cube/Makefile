MODULE_NAME = cube
CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
# egl/glesv2: the cube borrows the wlroots GLES2 renderer's context rather
# than creating one, but it still calls into both libraries directly.
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 pixman-1 egl glesv2 $(WLROOTS_PC))
PKG_CFLAGS = $(shell pkg-config --cflags pixman-1 egl glesv2 libdrm $(WLROOTS_PC))
OUTDIR ?= .
LIBDIR ?= .
SRC = gowl-module-cube.c gowl-cube-plan.c gowl-cube-gl.c
OUT = $(OUTDIR)/$(MODULE_NAME).so
all: $(OUT)
$(OUT): $(SRC) $(wildcard *.h ../../src/core/*.h ../../src/interfaces/*.h) $(LIBDIR)/libgowl.so
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(SRC) $(LDFLAGS) -Wl,-soname,cube.so -L$(LIBDIR) -lgowl $(PKG_LIBS) -lm
clean:
	rm -f $(OUT)
.PHONY: all clean
