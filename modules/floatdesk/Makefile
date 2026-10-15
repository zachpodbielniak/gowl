MODULE_NAME = floatdesk

CC ?= gcc
WLROOTS_PC ?= $(shell pkg-config --exists wlroots-0.20 && echo wlroots-0.20 || echo wlroots-0.19)
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0 gmodule-2.0 $(WLROOTS_PC) cairo pangocairo wayland-server xkbcommon) -lm
PKG_CFLAGS = $(shell pkg-config --cflags gio-unix-2.0 $(WLROOTS_PC) cairo pangocairo xkbcommon)

# Our own log domain: the top level's -DG_LOG_DOMAIN loses its quotes
# one make-into-shell level down (see modules/menu/Makefile).
PKG_CFLAGS += -UG_LOG_DOMAIN -DG_LOG_DOMAIN=\"gowl-floatdesk\"

OUTDIR ?= .
LIBDIR ?= .

# The state machine and the app catalog are separate files so the tests
# can compile them on their own; the module links all of them.
SRC = $(wildcard *.c)
OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

$(OUT): $(SRC) $(wildcard *.h)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $(SRC) $(LDFLAGS) -Wl,-soname,floatdesk.so -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
