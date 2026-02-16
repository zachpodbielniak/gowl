# config.mk - Gowl Configuration
# GObject Wayland Compositor
#
# This file contains all configurable build options.
# Override any variable on the command line:
#   make DEBUG=1
#   make PREFIX=/usr/local

# Version
VERSION_MAJOR := 0
VERSION_MINOR := 1
VERSION_MICRO := 0
VERSION := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)

# Installation directories
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
# Auto-detect lib directory suffix (lib vs lib64)
# 64-bit distros (Fedora, RHEL, SUSE) use lib64; override with LIBDIR=...
LIBSUFFIX := $(shell [ -d /usr/lib64 ] && echo lib64 || echo lib)
LIBDIR ?= $(PREFIX)/$(LIBSUFFIX)
INCLUDEDIR ?= $(PREFIX)/include
DATADIR ?= $(PREFIX)/share
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
GIRDIR ?= $(DATADIR)/gir-1.0
TYPELIBDIR ?= $(LIBDIR)/girepository-1.0
MODULEDIR ?= $(LIBDIR)/gowl/modules
BAR_MODULEDIR ?= $(LIBDIR)/gowlbar/modules
SYSCONFDIR ?= /etc

# Build directories
BUILDDIR := build
OBJDIR_DEBUG := $(BUILDDIR)/debug/obj
OBJDIR_RELEASE := $(BUILDDIR)/release/obj
BINDIR_DEBUG := $(BUILDDIR)/debug
BINDIR_RELEASE := $(BUILDDIR)/release

# Build options (0 or 1)
DEBUG ?= 0
ASAN ?= 0
BUILD_GIR ?= 0
BUILD_TESTS ?= 1
BUILD_MODULES ?= 1
BUILD_XWAYLAND ?= 1
MCP ?= 0

# Select build directories based on DEBUG
ifeq ($(DEBUG),1)
    OBJDIR := $(OBJDIR_DEBUG)
    OUTDIR := $(BINDIR_DEBUG)
    BUILD_TYPE := debug
else
    OBJDIR := $(OBJDIR_RELEASE)
    OUTDIR := $(BINDIR_RELEASE)
    BUILD_TYPE := release
endif

# Compiler and tools
CC := gcc
AR := ar
PKG_CONFIG ?= pkg-config
GIR_SCANNER ?= g-ir-scanner
GIR_COMPILER ?= g-ir-compiler
WAYLAND_SCANNER ?= $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)
INSTALL := install
INSTALL_PROGRAM := $(INSTALL) -m 755
INSTALL_DATA := $(INSTALL) -m 644
MKDIR_P := mkdir -p

# C standard and dialect
CSTD := -std=gnu89

# Base compiler flags
CFLAGS_BASE := $(CSTD) -Wall -Wextra -Wno-unused-parameter
CFLAGS_BASE += -fPIC
CFLAGS_BASE += -DGOWL_VERSION=\"$(VERSION)\"
CFLAGS_BASE += -DGOWL_VERSION_MAJOR=$(VERSION_MAJOR)
CFLAGS_BASE += -DGOWL_VERSION_MINOR=$(VERSION_MINOR)
CFLAGS_BASE += -DGOWL_VERSION_MICRO=$(VERSION_MICRO)
CFLAGS_BASE += -DGOWL_MODULEDIR=\"$(MODULEDIR)\"
CFLAGS_BASE += -DGOWL_SYSCONFDIR=\"$(SYSCONFDIR)\"
CFLAGS_BASE += -DGOWL_DATADIR=\"$(DATADIR)\"
CFLAGS_BASE += -DWLR_USE_UNSTABLE
CFLAGS_BASE += -DG_LOG_USE_STRUCTURED
CFLAGS_BASE += -DG_LOG_DOMAIN=\"gowl\"
CFLAGS_BASE += -DGOWL_DEV_INCLUDE_DIR=\"$(CURDIR)/$(BUILDDIR)/include\"

# Debug/Release flags
ifeq ($(DEBUG),1)
    CFLAGS_BUILD := -g -O0 -DDEBUG
    ifeq ($(ASAN),1)
        CFLAGS_BUILD += -fsanitize=address -fsanitize=undefined
        LDFLAGS_ASAN := -fsanitize=address -fsanitize=undefined
    endif
else
    CFLAGS_BUILD := -O2 -DNDEBUG
endif

# Required dependencies
DEPS_REQUIRED := glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0
DEPS_REQUIRED += wlroots-0.19 wayland-server wayland-protocols
DEPS_REQUIRED += xkbcommon libinput
DEPS_REQUIRED += yaml-0.1 json-glib-1.0

# Optional XWayland dependencies
ifeq ($(BUILD_XWAYLAND),1)
DEPS_XWAYLAND := xcb xcb-icccm
XWAYLAND_AVAILABLE := $(shell $(PKG_CONFIG) --exists $(DEPS_XWAYLAND) 2>/dev/null && echo 1 || echo 0)
else
XWAYLAND_AVAILABLE := 0
endif

# Optional MCP dependencies (requires mcp-glib submodule at deps/mcp-glib)
ifeq ($(MCP),1)
DEPS_MCP := libsoup-3.0 libdex-1 libpng
MCP_AVAILABLE := $(shell $(PKG_CONFIG) --exists $(DEPS_MCP) 2>/dev/null && echo 1 || echo 0)
else
MCP_AVAILABLE := 0
endif

# Wayland protocols directory
WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null)

# Check for required dependencies
define check_dep
$(if $(shell $(PKG_CONFIG) --exists $(1) && echo yes),,$(error Missing dependency: $(1)))
endef

# Get flags from pkg-config
CFLAGS_DEPS := $(shell $(PKG_CONFIG) --cflags $(DEPS_REQUIRED) 2>/dev/null)
LDFLAGS_DEPS := $(shell $(PKG_CONFIG) --libs $(DEPS_REQUIRED) 2>/dev/null)

# Add XWayland flags if available
ifeq ($(XWAYLAND_AVAILABLE),1)
    CFLAGS_BASE += -DGOWL_HAVE_XWAYLAND=1
    CFLAGS_DEPS += $(shell $(PKG_CONFIG) --cflags $(DEPS_XWAYLAND) 2>/dev/null)
    LDFLAGS_DEPS += $(shell $(PKG_CONFIG) --libs $(DEPS_XWAYLAND) 2>/dev/null)
endif

# Include paths
CFLAGS_INC := -I. -Isrc -Ideps/yaml-glib/src

# Combine all CFLAGS
CFLAGS := $(CFLAGS_BASE) $(CFLAGS_BUILD) $(CFLAGS_INC) $(CFLAGS_DEPS)

# Linker flags
LDFLAGS := $(LDFLAGS_DEPS) $(LDFLAGS_ASAN)
LDFLAGS_SHARED := -shared -Wl,-soname,libgowl.so.$(VERSION_MAJOR)

# Library names
LIB_NAME := gowl
LIB_STATIC := lib$(LIB_NAME).a
LIB_SHARED := lib$(LIB_NAME).so
LIB_SHARED_FULL := lib$(LIB_NAME).so.$(VERSION)
LIB_SHARED_MAJOR := lib$(LIB_NAME).so.$(VERSION_MAJOR)

# GIR settings
GIR_NAMESPACE := Gowl
GIR_VERSION := $(VERSION_MAJOR).$(VERSION_MINOR)
GIR_FILE := $(GIR_NAMESPACE)-$(GIR_VERSION).gir
TYPELIB_FILE := $(GIR_NAMESPACE)-$(GIR_VERSION).typelib

# Test framework
TEST_CFLAGS := $(CFLAGS) $(shell $(PKG_CONFIG) --cflags glib-2.0)
TEST_LDFLAGS := $(LDFLAGS) -L$(OUTDIR) -lgowl -Wl,-rpath,$(OUTDIR)

# Module flags
MODULE_CFLAGS_INC := -I$(CURDIR) -I$(CURDIR)/src -I$(CURDIR)/deps/yaml-glib/src
MODULE_CFLAGS := $(CFLAGS_BASE) $(CFLAGS_BUILD) $(MODULE_CFLAGS_INC) $(CFLAGS_DEPS)
MODULE_LDFLAGS := -shared -fPIC

# Bar dependencies (standalone Wayland client)
BAR_DEPS := glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 wayland-client pangocairo yaml-0.1 json-glib-1.0
BAR_CFLAGS_DEPS := $(shell $(PKG_CONFIG) --cflags $(BAR_DEPS) 2>/dev/null)
BAR_LDFLAGS_DEPS := $(shell $(PKG_CONFIG) --libs $(BAR_DEPS) 2>/dev/null)

# Bar compiler flags
BAR_CFLAGS := $(CSTD) -Wall -Wextra -Wno-unused-parameter -fPIC
BAR_CFLAGS += -DGOWL_VERSION=\"$(VERSION)\"
BAR_CFLAGS += -DGOWL_SYSCONFDIR=\"$(SYSCONFDIR)\"
BAR_CFLAGS += -DGOWL_DATADIR=\"$(DATADIR)\"
BAR_CFLAGS += -DGOWLBAR_MODULEDIR=\"$(BAR_MODULEDIR)\"
BAR_CFLAGS += -DGOWL_DEV_INCLUDE_DIR=\"$(CURDIR)/$(BUILDDIR)/include\"
BAR_CFLAGS += $(CFLAGS_BUILD)
BAR_CFLAGS += -I. -Isrc/bar -Ideps/yaml-glib/src
BAR_CFLAGS += $(BAR_CFLAGS_DEPS)

BAR_LDFLAGS := $(BAR_LDFLAGS_DEPS) $(LDFLAGS_ASAN)

# Print configuration
.PHONY: show-config
show-config:
	@echo "Gowl Build Configuration"
	@echo "========================"
	@echo "Version:         $(VERSION)"
	@echo "Build type:      $(BUILD_TYPE)"
	@echo "Compiler:        $(CC)"
	@echo "CFLAGS:          $(CFLAGS)"
	@echo "LDFLAGS:         $(LDFLAGS)"
	@echo "PREFIX:          $(PREFIX)"
	@echo "LIBDIR:          $(LIBDIR)"
	@echo "MODULEDIR:       $(MODULEDIR)"
	@echo "DEBUG:           $(DEBUG)"
	@echo "ASAN:            $(ASAN)"
	@echo "BUILD_GIR:       $(BUILD_GIR)"
	@echo "BUILD_TESTS:     $(BUILD_TESTS)"
	@echo "BUILD_MODULES:   $(BUILD_MODULES)"
	@echo "BUILD_XWAYLAND:  $(BUILD_XWAYLAND)"
	@echo "XWAYLAND_AVAILABLE: $(XWAYLAND_AVAILABLE)"
	@echo "MCP:             $(MCP)"
	@echo "MCP_AVAILABLE:   $(MCP_AVAILABLE)"
