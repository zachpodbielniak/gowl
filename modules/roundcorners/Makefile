MODULE_NAME = roundcorners

CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 $(WLROOTS_PC) cairo)
PKG_CFLAGS = $(shell pkg-config --cflags $(WLROOTS_PC) cairo)

OUTDIR ?= .
LIBDIR ?= .

SRC = gowl-module-$(MODULE_NAME).c
OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

$(OUT):
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(SRC) $(LDFLAGS) -L$(LIBDIR) -lgowl $(PKG_LIBS) -lm

clean:
	rm -f $(OUT)

.PHONY: all clean
