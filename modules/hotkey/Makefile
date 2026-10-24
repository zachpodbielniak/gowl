MODULE_NAME = hotkey

CC ?= gcc
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 gio-2.0 json-glib-1.0 wayland-server xkbcommon)

OUTDIR ?= .
LIBDIR ?= .

# The model (YAML), engine (key decisions), Send parser and hotstring
# translator are files of their own, pure GLib, so the tests compile them
# without the module's compositor half.
SRC = gowl-module-$(MODULE_NAME).c gowl-hotkey-model.c gowl-hotkey-engine.c \
      gowl-hotkey-send.c gowl-hotkey-hotstring.c gowl-hotkey-script.c \
      gowl-hotkey-runner.c gowl-hotkey-dbus.c
OUT = $(OUTDIR)/$(MODULE_NAME).so

# The in-tree headers a `c' action or a hotkey script compiles against in
# a development build (as gabbrev's `c' variables do).
DEV_INCLUDE := $(abspath $(LIBDIR)/../include)

all: $(OUT)

# The sources too: the top level calls this without dependencies.mk,
# and a rule naming only headers leaves a stale .so after a .c edit.
$(OUT): $(SRC) gowl-hotkey-model.h gowl-hotkey-engine.h gowl-hotkey-send.h \
        gowl-hotkey-hotstring.h gowl-hotkey-script.h gowl-hotkey-module.h
	$(CC) $(CFLAGS) \
		-DHOTKEY_DEV_INCLUDE='"$(DEV_INCLUDE)"' \
		-o $@ $(SRC) $(LDFLAGS) -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
