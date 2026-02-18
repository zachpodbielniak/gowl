# Makefile - Gowl (GObject Wayland Compositor)
# A GObject-based Wayland compositor with modular extensibility
#
# Usage:
#   make           - Build all (lib, gowl, modules)
#   make lib       - Build static and shared libraries
#   make gowl      - Build the gowl executable
#   make gir       - Generate GIR/typelib for introspection
#   make modules   - Build all modules
#   make test      - Run the test suite
#   make install   - Install to PREFIX
#   make clean     - Clean build artifacts
#   make DEBUG=1   - Build with debug symbols
#   make ASAN=1    - Build with AddressSanitizer

.DEFAULT_GOAL := all
.PHONY: all lib gowl-bin gir modules test deps check-deps install-deps

# Include configuration
include config.mk

# Check dependencies before anything else (skip for bootstrap targets)
SKIP_DEP_CHECK_GOALS := install-deps install-debug-session help show-config check-deps clean
ifeq ($(filter $(SKIP_DEP_CHECK_GOALS),$(MAKECMDGOALS)),)
$(foreach dep,$(DEPS_REQUIRED),$(call check_dep,$(dep)))
$(foreach dep,$(BAR_DEPS),$(call check_dep,$(dep)))
endif

# Source files - Library
LIB_SRCS := \
	src/gowl-enums.c \
	src/boxed/gowl-geometry.c \
	src/boxed/gowl-color.c \
	src/boxed/gowl-key-combo.c \
	src/boxed/gowl-tag-mask.c \
	src/boxed/gowl-gaps.c \
	src/boxed/gowl-border-spec.c \
	src/boxed/gowl-rule.c \
	src/boxed/gowl-output-mode.c \
	src/module/gowl-module.c \
	src/module/gowl-module-manager.c \
	src/module/gowl-module-info.c \
	src/interfaces/gowl-layout-provider.c \
	src/interfaces/gowl-keybind-handler.c \
	src/interfaces/gowl-mouse-handler.c \
	src/interfaces/gowl-client-decorator.c \
	src/interfaces/gowl-client-placer.c \
	src/interfaces/gowl-focus-policy.c \
	src/interfaces/gowl-monitor-configurator.c \
	src/interfaces/gowl-rule-provider.c \
	src/interfaces/gowl-startup-handler.c \
	src/interfaces/gowl-shutdown-handler.c \
	src/interfaces/gowl-ipc-handler.c \
	src/interfaces/gowl-tag-manager.c \
	src/interfaces/gowl-gap-provider.c \
	src/interfaces/gowl-bar-provider.c \
	src/interfaces/gowl-scratchpad-handler.c \
	src/interfaces/gowl-swallow-handler.c \
	src/interfaces/gowl-sticky-handler.c \
	src/interfaces/gowl-cursor-provider.c \
	src/interfaces/gowl-wallpaper-provider.c \
	src/interfaces/gowl-lock-handler.c \
	src/config/gowl-config.c \
	src/config/gowl-config-compiler.c \
	src/config/gowl-keybind.c \
	src/layout/gowl-layout-tile.c \
	src/layout/gowl-layout-monocle.c \
	src/layout/gowl-layout-float.c \
	src/ipc/gowl-ipc.c \
	src/util/gowl-log.c \
	src/core/gowl-compositor.c \
	src/core/gowl-monitor.c \
	src/core/gowl-client.c \
	src/core/gowl-seat.c \
	src/core/gowl-keyboard-group.c \
	src/core/gowl-cursor.c \
	src/core/gowl-layer-surface.c \
	src/core/gowl-bar.c \
	src/core/gowl-session-lock.c \
	src/core/gowl-idle-manager.c

# Header files (for GIR scanner and installation)
LIB_HDRS := \
	src/gowl.h \
	src/gowl-types.h \
	src/gowl-enums.h \
	src/gowl-version.h \
	src/boxed/gowl-geometry.h \
	src/boxed/gowl-color.h \
	src/boxed/gowl-key-combo.h \
	src/boxed/gowl-tag-mask.h \
	src/boxed/gowl-gaps.h \
	src/boxed/gowl-border-spec.h \
	src/boxed/gowl-rule.h \
	src/boxed/gowl-output-mode.h \
	src/module/gowl-module.h \
	src/module/gowl-module-manager.h \
	src/module/gowl-module-info.h \
	src/interfaces/gowl-layout-provider.h \
	src/interfaces/gowl-keybind-handler.h \
	src/interfaces/gowl-mouse-handler.h \
	src/interfaces/gowl-client-decorator.h \
	src/interfaces/gowl-client-placer.h \
	src/interfaces/gowl-focus-policy.h \
	src/interfaces/gowl-monitor-configurator.h \
	src/interfaces/gowl-rule-provider.h \
	src/interfaces/gowl-startup-handler.h \
	src/interfaces/gowl-shutdown-handler.h \
	src/interfaces/gowl-ipc-handler.h \
	src/interfaces/gowl-tag-manager.h \
	src/interfaces/gowl-gap-provider.h \
	src/interfaces/gowl-bar-provider.h \
	src/interfaces/gowl-scratchpad-handler.h \
	src/interfaces/gowl-swallow-handler.h \
	src/interfaces/gowl-sticky-handler.h \
	src/interfaces/gowl-cursor-provider.h \
	src/interfaces/gowl-wallpaper-provider.h \
	src/interfaces/gowl-lock-handler.h \
	src/config/gowl-config.h \
	src/config/gowl-config-compiler.h \
	src/config/gowl-keybind.h \
	src/layout/gowl-layout-tile.h \
	src/layout/gowl-layout-monocle.h \
	src/layout/gowl-layout-float.h \
	src/ipc/gowl-ipc.h \
	src/util/gowl-log.h \
	src/core/gowl-compositor.h \
	src/core/gowl-monitor.h \
	src/core/gowl-client.h \
	src/core/gowl-seat.h \
	src/core/gowl-keyboard-group.h \
	src/core/gowl-cursor.h \
	src/core/gowl-layer-surface.h \
	src/core/gowl-bar.h \
	src/core/gowl-session-lock.h \
	src/core/gowl-idle-manager.h

# yaml-glib sources (built-in dependency)
YAMLGLIB_SRCS := \
	deps/yaml-glib/src/yaml-builder.c \
	deps/yaml-glib/src/yaml-document.c \
	deps/yaml-glib/src/yaml-generator.c \
	deps/yaml-glib/src/yaml-gobject.c \
	deps/yaml-glib/src/yaml-mapping.c \
	deps/yaml-glib/src/yaml-node.c \
	deps/yaml-glib/src/yaml-parser.c \
	deps/yaml-glib/src/yaml-schema.c \
	deps/yaml-glib/src/yaml-sequence.c \
	deps/yaml-glib/src/yaml-serializable.c

# crispy sources (built-in dependency for C config compilation)
CRISPY_SRCS := \
	deps/crispy/src/interfaces/crispy-compiler.c \
	deps/crispy/src/interfaces/crispy-cache-provider.c \
	deps/crispy/src/core/crispy-gcc-compiler.c \
	deps/crispy/src/core/crispy-file-cache.c \
	deps/crispy/src/core/crispy-plugin-engine.c \
	deps/crispy/src/core/crispy-script.c \
	deps/crispy/src/core/crispy-source-utils-private.c \
	deps/crispy/src/core/crispy-config-context.c \
	deps/crispy/src/core/crispy-config-loader.c

# Test sources
TEST_SRCS := $(wildcard tests/test-*.c)

# Module directories
MODULE_DIRS := $(wildcard modules/*)
ifneq ($(MCP_AVAILABLE),1)
MODULE_DIRS := $(filter-out modules/mcp,$(MODULE_DIRS))
endif

# Bar source files (standalone Wayland client)
BAR_SRCS := \
	src/bar/gowlbar-main.c \
	src/bar/gowlbar-app.c \
	src/bar/gowlbar-output.c \
	src/bar/gowlbar-config.c \
	src/bar/gowlbar-config-compiler.c \
	src/bar/gowlbar-widget.c \
	src/bar/gowlbar-ipc.c \
	src/bar/gowlbar-tag-widget.c \
	src/bar/gowlbar-layout-widget.c \
	src/bar/gowlbar-title-widget.c \
	src/bar/gowlbar-status-widget.c \
	src/bar/gowlbar-module.c \
	src/bar/gowlbar-module-manager.c \
	src/bar/interfaces/gowlbar-widget-provider.c \
	src/bar/interfaces/gowlbar-status-provider.c \
	src/bar/interfaces/gowlbar-click-handler.c \
	src/bar/interfaces/gowlbar-startup-handler.c \
	src/bar/interfaces/gowlbar-shutdown-handler.c

# Object files
LIB_OBJS := $(patsubst src/%.c,$(OBJDIR)/%.o,$(LIB_SRCS))
YAMLGLIB_OBJS := $(patsubst deps/%.c,$(OBJDIR)/deps/%.o,$(YAMLGLIB_SRCS))
CRISPY_OBJS := $(patsubst deps/%.c,$(OBJDIR)/deps/%.o,$(CRISPY_SRCS))
MAIN_OBJ := $(OBJDIR)/main.o
BAR_OBJS := $(patsubst src/bar/%.c,$(OBJDIR)/bar/%.o,$(BAR_SRCS))
TEST_OBJS := $(patsubst tests/%.c,$(OBJDIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(OUTDIR)/%,$(TEST_SRCS))

# Include build rules
include rules.mk

# Build the bar executable
bar: deps $(OUTDIR)/gowlbar

# Default target
all: deps lib gowl-bin bar
ifeq ($(BUILD_MODULES),1)
all: modules
endif
ifeq ($(BUILD_GIR),1)
all: gir
endif
ifeq ($(MCP_AVAILABLE),1)
all: gowl-mcp
endif

# Build dependencies (yaml-glib, crispy)
deps: $(YAMLGLIB_OBJS) $(CRISPY_OBJS)

# Build the library
lib: src/gowl-version.h $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL) $(OUTDIR)/gowl.pc

# Build the executable
gowl-bin: lib $(OUTDIR)/gowl

# Build GIR/typelib
gir: $(OUTDIR)/$(GIR_FILE) $(OUTDIR)/$(TYPELIB_FILE)

# Build gowl-mcp relay binary (only if MCP=1)
ifeq ($(MCP_AVAILABLE),1)
.PHONY: gowl-mcp
gowl-mcp: $(OUTDIR)/gowl-mcp

$(OUTDIR)/gowl-mcp: tools/gowl-mcp/gowl-mcp.c | $(OUTDIR)
	$(MAKE) -C tools/gowl-mcp OUTDIR=$(abspath $(OUTDIR))
endif

# Build all modules
modules: lib $(OUTDIR)/modules
	@for dir in $(MODULE_DIRS); do \
		if [ -d "$$dir" ] && [ -f "$$dir/Makefile" ]; then \
			echo "Building module: $$(basename $$dir)"; \
			$(MAKE) -C "$$dir" \
				OUTDIR=$(abspath $(OUTDIR)/modules) \
				LIBDIR=$(abspath $(OUTDIR)) \
				CFLAGS="$(MODULE_CFLAGS)" \
				LDFLAGS="$(MODULE_LDFLAGS)"; \
		fi \
	done

# Build and run tests
test: lib $(TEST_BINS)
	@echo "Running tests..."
	@failed=0; \
	for test in $(TEST_BINS); do \
		echo "  Running $$(basename $$test)..."; \
		if LD_LIBRARY_PATH=$(OUTDIR) $$test; then \
			echo "    PASS"; \
		else \
			echo "    FAIL"; \
			failed=$$((failed + 1)); \
		fi \
	done; \
	if [ $$failed -gt 0 ]; then \
		echo "$$failed test(s) failed"; \
		exit 1; \
	else \
		echo "All tests passed"; \
	fi

# Build individual test binaries
$(OUTDIR)/test-%: $(OBJDIR)/tests/test-%.o $(OUTDIR)/$(LIB_SHARED_FULL)
	$(CC) -o $@ $< $(TEST_LDFLAGS)

# Check dependencies
check-deps:
	@echo "Checking dependencies..."
	@for dep in $(DEPS_REQUIRED); do \
		if $(PKG_CONFIG) --exists $$dep; then \
			ver=$$($(PKG_CONFIG) --modversion $$dep 2>/dev/null); \
			echo "  $$dep: OK ($$ver)"; \
		else \
			echo "  $$dep: MISSING"; \
		fi \
	done
	@echo ""
	@echo "Optional dependencies (XWayland):"
	@for dep in xcb xcb-icccm; do \
		if $(PKG_CONFIG) --exists $$dep; then \
			ver=$$($(PKG_CONFIG) --modversion $$dep 2>/dev/null); \
			echo "  $$dep: OK ($$ver)"; \
		else \
			echo "  $$dep: MISSING"; \
		fi \
	done
	@echo ""
	@echo "Optional dependencies (MCP=1):"
	@for dep in libsoup-3.0 libdex-1 libpng; do \
		if $(PKG_CONFIG) --exists $$dep; then \
			ver=$$($(PKG_CONFIG) --modversion $$dep 2>/dev/null); \
			echo "  $$dep: OK ($$ver)"; \
		else \
			echo "  $$dep: MISSING"; \
		fi \
	done

# Fedora package names for MCP dependencies
FEDORA_DEPS_MCP := libsoup3-devel libdex-devel libpng-devel

# Install dependencies (Fedora 41+)
install-deps:
	@echo "Installing gowl dependencies (Fedora)..."
	sudo dnf install -y \
		gcc make pkg-config \
		glib2-devel \
		json-glib-devel \
		wayland-devel \
		wayland-protocols-devel \
		wlroots-devel \
		libxkbcommon-devel \
		libinput-devel \
		libyaml-devel \
		gobject-introspection-devel \
		libxcb-devel \
		xcb-util-wm-devel \
		libdrm-devel \
		pixman-devel \
		pango-devel \
		cairo-devel \
		pam-devel \
		gdk-pixbuf2-devel \
		libasan libubsan \
		$(if $(filter 1,$(MCP)),$(FEDORA_DEPS_MCP))

# Install a debug .desktop session file pointing at the local debug build
.PHONY: install-debug-session
install-debug-session: $(OUTDIR)/gowl
	@echo "Installing debug session file..."
	@$(MKDIR_P) $(DESTDIR)$(DATADIR)/wayland-sessions
	@printf '%s\n' \
		'[Desktop Entry]' \
		'Name=Gowl (Debug)' \
		'Comment=Gowl debug build from $(CURDIR)' \
		'Exec=$(CURDIR)/build/debug/gowl --debug' \
		'Type=Application' \
		'Icon=gowl' \
		'DesktopNames=gowl' \
		> $(DESTDIR)$(DATADIR)/wayland-sessions/gowl-debug.desktop
	@echo "Installed: $(DESTDIR)$(DATADIR)/wayland-sessions/gowl-debug.desktop"
	@echo "  Exec: $(CURDIR)/build/debug/gowl --debug"

# Help target
.PHONY: help
help:
	@echo "Gowl - GObject Wayland Compositor"
	@echo ""
	@echo "Build targets:"
	@echo "  all          - Build library, executable, bar, and modules (default)"
	@echo "  lib          - Build static and shared libraries"
	@echo "  gowl-bin     - Build the gowl executable"
	@echo "  bar          - Build the gowlbar status bar"
	@echo "  gir          - Generate GObject Introspection data"
	@echo "  modules      - Build all modules"
	@echo "  test         - Build and run the test suite"
	@echo "  install      - Install to PREFIX ($(PREFIX))"
	@echo "  uninstall    - Remove installed files"
	@echo "  clean        - Remove build artifacts"
	@echo "  clean-all    - Remove all build directories"
	@echo ""
	@echo "Build options (set on command line):"
	@echo "  DEBUG=1          - Enable debug build"
	@echo "  ASAN=1           - Enable AddressSanitizer"
	@echo "  PREFIX=path      - Set installation prefix"
	@echo "  BUILD_GIR=0      - Disable GIR generation"
	@echo "  BUILD_TESTS=0    - Disable test building"
	@echo "  BUILD_XWAYLAND=0 - Disable XWayland support"
	@echo "  MCP=1            - Enable MCP server module (AI compositor control)"
	@echo ""
	@echo "Utility targets:"
	@echo "  check-deps            - Check for required dependencies"
	@echo "  install-deps          - Install dependencies (Fedora dnf)"
	@echo "  install-debug-session - Install a .desktop session file for the local debug build"
	@echo "  show-config           - Show current build configuration"
	@echo "  help                  - Show this help message"

# Dependency tracking
-include $(LIB_OBJS:.o=.d)
-include $(MAIN_OBJ:.o=.d)
