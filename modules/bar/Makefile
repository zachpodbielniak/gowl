MODULE_NAME = bar

CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 $(WLROOTS_PC) cairo pangocairo wayland-server xkbcommon)
PKG_CFLAGS = $(shell pkg-config --cflags $(WLROOTS_PC) cairo pangocairo xkbcommon)

OUTDIR ?= .
LIBDIR ?= .

# Every .c in this directory: the module is the host plus its shipped
# plugins, and a plugin file is added by dropping it in here rather
# than by editing a list.
SRC = $(wildcard *.c)
OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(SRC) $(LDFLAGS) -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
