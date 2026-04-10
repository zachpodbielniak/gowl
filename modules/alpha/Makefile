MODULE_NAME = alpha

CC ?= gcc
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 wlroots-0.19)
PKG_CFLAGS = $(shell pkg-config --cflags wlroots-0.19)

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
