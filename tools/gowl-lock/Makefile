# gowl-lock -- the screen lock, as a program of its own.
#
# Standalone binary (links no libgowl): a Wayland client that holds the
# session through ext-session-lock-v1, draws the prompt with cairo and
# authenticates with PAM.  See lock.h for why the lock is not a module
# in the compositor.
#
# The one file it borrows from the tree is util/gowl-wallpaper-scale.c,
# compiled in directly: it is pure integer arithmetic with no
# dependencies, and sharing it is what makes a backdrop crop the same
# way the desktop wallpaper under it does.

CC ?= gcc
CSTD = -std=gnu89

PKG_DEPS = glib-2.0 gio-2.0 wayland-client xkbcommon cairo gdk-pixbuf-2.0
PKG_CFLAGS = $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS = $(shell pkg-config --libs $(PKG_DEPS))

WP_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER = $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)

LOCK_XML = $(WP_DIR)/staging/ext-session-lock/ext-session-lock-v1.xml

OUTDIR ?= .
SRCDIR ?= ../../src

SRCS = main.c lock-wayland.c lock-render.c lock-pam.c
GEN = ext-session-lock-v1-client-protocol.h ext-session-lock-v1-protocol.c
OBJS = $(SRCS:.c=.o) ext-session-lock-v1-protocol.o \
       gowl-wallpaper-scale.o
OUT = $(OUTDIR)/gowl-lock

all: $(OUT)

ext-session-lock-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header $(LOCK_XML) $@

ext-session-lock-v1-protocol.c: ext-session-lock-v1-client-protocol.h
	$(WAYLAND_SCANNER) private-code $(LOCK_XML) $@

$(SRCS:.c=.o): ext-session-lock-v1-client-protocol.h

%.o: %.c
	$(CC) $(CSTD) -Wall -Wextra -Wno-unused-parameter -I. -I$(SRCDIR) \
	    $(PKG_CFLAGS) -c $< -o $@

# The scaling maths, straight out of the tree.
gowl-wallpaper-scale.o: $(SRCDIR)/util/gowl-wallpaper-scale.c
	$(CC) $(CSTD) -Wall -Wextra -Wno-unused-parameter -I. -I$(SRCDIR) \
	    $(PKG_CFLAGS) -c $< -o $@

ext-session-lock-v1-protocol.o: ext-session-lock-v1-protocol.c
	$(CC) $(CSTD) -Wall -Wno-unused-parameter -I. $(PKG_CFLAGS) \
	    -c $< -o $@

$(OUT): $(OBJS)
	$(CC) $(CSTD) -o $@ $(OBJS) $(PKG_LIBS) -lpam -lm

clean:
	rm -f $(OBJS) $(GEN) $(OUT)

.PHONY: all clean
