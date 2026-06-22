MODULE_NAME = recording

CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 $(WLROOTS_PC))
PKG_CFLAGS = $(shell pkg-config --cflags $(WLROOTS_PC))

OUTDIR ?= .
LIBDIR ?= .

SRC = gowl-module-$(MODULE_NAME).c
OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

$(OUT):
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(SRC) $(LDFLAGS) -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
