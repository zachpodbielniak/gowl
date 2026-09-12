MODULE_NAME = mirrortile

CC ?= gcc
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0)

OUTDIR ?= .
LIBDIR ?= .

SRC = gowl-module-$(MODULE_NAME).c
OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

$(OUT):
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) -Wl,-soname,$(MODULE_NAME).so -L$(LIBDIR) -lgowl $(PKG_LIBS) -lm

clean:
	rm -f $(OUT)

.PHONY: all clean
