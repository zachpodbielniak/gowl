MODULE_NAME = inputremap

CC ?= gcc
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 json-glib-1.0 wayland-server)

OUTDIR ?= .
LIBDIR ?= .

# The rule engine is its own file so the tests can link it without the
# module's compositor half.
SRC = gowl-module-$(MODULE_NAME).c gowl-inputremap-engine.c
OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

$(OUT): gowl-inputremap-engine.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
