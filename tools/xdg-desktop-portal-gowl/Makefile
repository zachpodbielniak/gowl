# xdg-desktop-portal-gowl -- InputCapture + RemoteDesktop portal backend.
#
# Standalone binary (links no libgowl): a Wayland client that binds the
# private gowl-input-capture protocol and bridges it to a libeis EIS
# server and the freedesktop InputCapture/RemoteDesktop D-Bus backends.
# Built only when libeis-1.0 is present (the parent Makefile gates this).

CC ?= gcc
CSTD = -std=gnu89

PKG_DEPS = glib-2.0 gio-2.0 gio-unix-2.0 wayland-client libeis-1.0 xkbcommon
PKG_CFLAGS = $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS = $(shell pkg-config --libs $(PKG_DEPS))

# ScreenCast needs PipeWire.  Optional: without it the backend still
# serves InputCapture and RemoteDesktop, and the frontend routes casting
# to whatever else is installed.  With it, a single window can be shared
# -- the thing portal-wlr cannot do, because wlr-screencopy addresses
# outputs and ext-image-capture-source addresses toplevels.
PW_DEPS = libpipewire-0.3
HAVE_PW := $(shell pkg-config --exists $(PW_DEPS) && echo 1 || echo 0)
ifeq ($(HAVE_PW),1)
PW_CFLAGS = $(shell pkg-config --cflags $(PW_DEPS)) -DGOWL_HAVE_PIPEWIRE=1
PW_LIBS = $(shell pkg-config --libs $(PW_DEPS))
SC_SRCS = portal-capture.c portal-pw.c portal-screencast.c
SC_GEN = ext-image-capture-source-v1-client-protocol.h \
         ext-image-capture-source-v1-protocol.c \
         ext-image-copy-capture-v1-client-protocol.h \
         ext-image-copy-capture-v1-protocol.c \
         ext-foreign-toplevel-list-v1-client-protocol.h \
         ext-foreign-toplevel-list-v1-protocol.c
SC_PROTO_OBJS = ext-image-capture-source-v1-protocol.o \
                ext-image-copy-capture-v1-protocol.o \
                ext-foreign-toplevel-list-v1-protocol.o
else
PW_CFLAGS =
PW_LIBS =
SC_SRCS =
SC_GEN =
SC_PROTO_OBJS =
endif

WP_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)

# libeis 1.6 added EIS_EVENT_SEAT_DEVICE_REQUESTED (and
# EIS_EVENT_DEVICE_READY).  Fedora 43 ships 1.5, where naming that
# enumerator is a compile error -- and F43 is still a supported
# Immutablue target, so this cannot simply be required.  Probed rather
# than assumed; see the comment at the case label in portal-eis.c for
# why dropping it on 1.5 is behaviour-neutral.
EIS_CPPFLAGS = $(shell pkg-config --atleast-version=1.6 libeis-1.0 \
	&& echo -DGOWL_HAVE_EIS_SEAT_DEVICE_REQUESTED=1)

WAYLAND_SCANNER = $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)

OUTDIR ?= .
PROTO_XML = ../../protocols/gowl-input-capture-unstable-v1.xml

SRCS = main.c portal-eis.c portal-wayland.c portal-dbus.c $(SC_SRCS)
GEN = gowl-input-capture-v1-client-protocol.h \
      gowl-input-capture-v1-protocol.c $(SC_GEN)
OBJS = $(SRCS:.c=.o) gowl-input-capture-v1-protocol.o $(SC_PROTO_OBJS)
OUT = $(OUTDIR)/xdg-desktop-portal-gowl

all: $(OUT)

# Generated client-side protocol glue.
gowl-input-capture-v1-client-protocol.h: $(PROTO_XML)
	$(WAYLAND_SCANNER) client-header $< $@

gowl-input-capture-v1-protocol.c: $(PROTO_XML) \
                                  gowl-input-capture-v1-client-protocol.h
	$(WAYLAND_SCANNER) private-code $< $@

# The staging protocols the ScreenCast backend speaks.  They ship with
# wayland-protocols; the scanner turns each into a client header plus
# the private code that implements its requests.
ext-image-capture-source-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
	    $(WP_DIR)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml $@

ext-image-capture-source-v1-protocol.c: ext-image-capture-source-v1-client-protocol.h
	$(WAYLAND_SCANNER) private-code \
	    $(WP_DIR)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml $@

ext-image-copy-capture-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
	    $(WP_DIR)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@

ext-image-copy-capture-v1-protocol.c: ext-image-copy-capture-v1-client-protocol.h
	$(WAYLAND_SCANNER) private-code \
	    $(WP_DIR)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@

ext-foreign-toplevel-list-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
	    $(WP_DIR)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml $@

ext-foreign-toplevel-list-v1-protocol.c: ext-foreign-toplevel-list-v1-client-protocol.h
	$(WAYLAND_SCANNER) private-code \
	    $(WP_DIR)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml $@

# Sources depend on the generated client headers.
$(SRCS:.c=.o): gowl-input-capture-v1-client-protocol.h $(SC_GEN)

%.o: %.c
	$(CC) $(CSTD) -Wall -Wextra -Wno-unused-parameter -I. \
	    $(PKG_CFLAGS) $(PW_CFLAGS) $(EIS_CPPFLAGS) -c $< -o $@

gowl-input-capture-v1-protocol.o: gowl-input-capture-v1-protocol.c
	$(CC) $(CSTD) -Wall -Wno-unused-parameter -I. $(PKG_CFLAGS) \
	    -c $< -o $@

ext-%-v1-protocol.o: ext-%-v1-protocol.c
	$(CC) $(CSTD) -Wall -Wno-unused-parameter -I. $(PKG_CFLAGS) \
	    -c $< -o $@

$(OUT): $(OBJS)
	$(CC) $(CSTD) -o $@ $(OBJS) $(PKG_LIBS) $(PW_LIBS)

clean:
	rm -f $(OBJS) $(GEN) $(OUT)

.PHONY: all clean
