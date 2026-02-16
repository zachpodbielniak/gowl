MODULE_NAME = mcp

CC ?= gcc

# mcp-glib sources (built inline as .o files)
MCP_GLIB_DIR = ../../deps/mcp-glib
MCP_GLIB_SRCS = $(wildcard $(MCP_GLIB_DIR)/src/*.c)
MCP_GLIB_OBJS = $(patsubst $(MCP_GLIB_DIR)/src/%.c,$(OUTDIR)/mcp-glib-%.o,$(MCP_GLIB_SRCS))
MCP_GLIB_INC = -I$(MCP_GLIB_DIR)/src

# Additional deps beyond what MODULE_CFLAGS provides
# (glib, gobject, gio, json-glib are already in MODULE_CFLAGS)
MCP_PKG_DEPS = libsoup-3.0 libdex-1 libpng
MCP_PKG_CFLAGS = $(shell pkg-config --cflags $(MCP_PKG_DEPS) 2>/dev/null)
MCP_PKG_LIBS = $(shell pkg-config --libs $(MCP_PKG_DEPS) 2>/dev/null)

# Strip G_LOG_DOMAIN and G_LOG_USE_STRUCTURED from CFLAGS for mcp-glib
# (these cause issues because the command-line define loses its quotes)
MCP_GLIB_CFLAGS = $(filter-out -DG_LOG_DOMAIN=% -DG_LOG_USE_STRUCTURED,$(CFLAGS))

OUTDIR ?= .
LIBDIR ?= .

# Module sources
SRCS = $(wildcard *.c)
OBJS = $(patsubst %.c,$(OUTDIR)/mcp-mod-%.o,$(SRCS))

OUT = $(OUTDIR)/$(MODULE_NAME).so

all: $(OUT)

# Compile module .c files (they #undef G_LOG_DOMAIN internally)
$(OUTDIR)/mcp-mod-%.o: %.c
	$(CC) $(CFLAGS) $(MCP_GLIB_INC) $(MCP_PKG_CFLAGS) -c $< -o $@

# Compile mcp-glib .c files with sanitised CFLAGS
$(OUTDIR)/mcp-glib-%.o: $(MCP_GLIB_DIR)/src/%.c
	$(CC) $(MCP_GLIB_CFLAGS) $(MCP_GLIB_INC) $(MCP_PKG_CFLAGS) -c $< -o $@

# Link everything into the shared module
$(OUT): $(OBJS) $(MCP_GLIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(MCP_GLIB_OBJS) \
	    -L$(LIBDIR) -lgowl $(MCP_PKG_LIBS)

clean:
	rm -f $(OUT) $(OBJS) $(MCP_GLIB_OBJS)

.PHONY: all clean
