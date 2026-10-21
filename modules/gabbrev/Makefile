MODULE_NAME = gabbrev

CC ?= gcc
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 gio-2.0 json-glib-1.0 wayland-server xkbcommon)

OUTDIR ?= .
LIBDIR ?= .

# The model (loader), engine (matching) and renderer (variables) are
# files of their own, pure GLib, so the tests compile them without the
# module's compositor half.
SRC = gowl-module-$(MODULE_NAME).c gowl-gabbrev-model.c \
      gowl-gabbrev-builtin.c gowl-gabbrev-engine.c gowl-gabbrev-render.c \
      gowl-gabbrev-cvar.c gowl-gabbrev-stats.c gowl-gabbrev-learn.c
OUT = $(OUTDIR)/$(MODULE_NAME).so

# The in-tree headers a `c' variable compiles against in a development
# build (the macro module's note on why this is spelled here rather than
# inherited from the top-level CFLAGS applies).
DEV_INCLUDE := $(abspath $(LIBDIR)/../include)

all: $(OUT)

# The sources too: the top level calls this without dependencies.mk,
# and a rule naming only headers leaves a stale .so after a .c edit.
$(OUT): $(SRC) gowl-gabbrev-model.h gowl-gabbrev-engine.h \
        gowl-gabbrev-render.h gowl-gabbrev-cvar.h gowl-gabbrev-stats.h \
        gowl-gabbrev-learn.h
	$(CC) $(CFLAGS) \
		-DGABBREV_DEV_INCLUDE='"$(DEV_INCLUDE)"' \
		-o $@ $(SRC) $(LDFLAGS) -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
