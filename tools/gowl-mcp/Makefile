CC ?= gcc
CSTD = -std=gnu89

PKG_DEPS = glib-2.0 gio-2.0 gio-unix-2.0
PKG_CFLAGS = $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS = $(shell pkg-config --libs $(PKG_DEPS))

OUTDIR ?= .

SRC = gowl-mcp.c
OUT = $(OUTDIR)/gowl-mcp

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CSTD) -Wall -Wextra -Wno-unused-parameter $(PKG_CFLAGS) \
	    -o $@ $(SRC) $(PKG_LIBS)

clean:
	rm -f $(OUT)

.PHONY: all clean
