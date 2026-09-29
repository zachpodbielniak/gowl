MODULE_NAME = macro

CC ?= gcc
PKG_LIBS = $(shell pkg-config --libs glib-2.0 gobject-2.0 gmodule-2.0 gio-2.0 json-glib-1.0 wayland-server xkbcommon)

OUTDIR ?= .
LIBDIR ?= .

# The loader and runner are files of their own so the tests can include
# them without the module's GObject half.
SRC = gowl-module-macro.c gowl-macro-loader.c gowl-macro-runner.c gowl-macro-dbus.c \
      gowl-macro-filter.c
OUT = $(OUTDIR)/$(MODULE_NAME).so

# Paths the module needs as C strings.  Spelled here, single-quoted in
# the recipe, because a -D"..." inside the CFLAGS the top level hands
# down loses its quotes on the way (the bar module has the same note):
#   - the in-tree headers a macro compiles against in a development
#     build ($(BUILDDIR)/include, next to LIBDIR's build type), and
#   - the shipped example macros, searched last so they run straight
#     from a build tree.
DEV_INCLUDE := $(abspath $(LIBDIR)/../include)
DEV_MACROS  := $(abspath ../../data/macros)

all: $(OUT)

$(OUT): gowl-macro-loader.h gowl-macro-runner.h gowl-module-macro.h
	$(CC) $(CFLAGS) \
		-DGOWL_MACRO_DEV_INCLUDE='"$(DEV_INCLUDE)"' \
		-DGOWL_MACRO_DEV_MACROS='"$(DEV_MACROS)"' \
		-o $@ $(SRC) $(LDFLAGS) -L$(LIBDIR) -lgowl $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
