# rules.mk - Gowl Build Rules
# Pattern rules and common build recipes

# Wayland protocol headers (must be defined before use in dependency rules)
PROTO_HDRS := \
	xdg-shell-protocol.h \
	wlr-layer-shell-unstable-v1-protocol.h \
	cursor-shape-v1-protocol.h \
	pointer-constraints-unstable-v1-protocol.h \
	tablet-v2-protocol.h \
	ext-image-copy-capture-v1-protocol.h \
	ext-workspace-v1-protocol.h \
	wlr-output-power-management-unstable-v1-protocol.h \
	gowl-input-capture-v1-protocol.h

# All source objects depend on generated version header and protocol headers
$(LIB_OBJS) $(MAIN_OBJ): src/gowl-version.h $(PROTO_HDRS)

# Object file compilation
$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/core/%.o: src/core/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/config/%.o: src/config/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/module/%.o: src/module/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/boxed/%.o: src/boxed/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/interfaces/%.o: src/interfaces/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/barkit/%.o: src/barkit/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/ipc/%.o: src/ipc/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/util/%.o: src/util/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/protocols/%.o: src/protocols/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Generated wayland-scanner private-code lives in the project root
# (alongside the *-protocol.h files it needs).  The LIB_SRCS entry
# is `ext-workspace-v1-protocol.c' (no src/ prefix), so route it to
# a dedicated pattern rule at the root.
$(OBJDIR)/ext-workspace-v1-protocol.o: ext-workspace-v1-protocol.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/gowl-input-capture-v1-protocol.o: gowl-input-capture-v1-protocol.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Bar source compilation
$(OBJDIR)/bar/%.o: src/bar/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(BAR_CFLAGS) -c $< -o $@

# Bar client-side protocol code generation
wlr-layer-shell-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/wlr-layer-shell-unstable-v1.xml $@

xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

# Bar protocol objects
$(OBJDIR)/bar/wlr-layer-shell-unstable-v1-protocol.o: wlr-layer-shell-unstable-v1-protocol.c wlr-layer-shell-unstable-v1-client-protocol.h | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(BAR_CFLAGS) -c $< -o $@

$(OBJDIR)/bar/xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-client-protocol.h | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(BAR_CFLAGS) -c $< -o $@

# Bar sources depend on client-side protocol headers
$(BAR_OBJS): wlr-layer-shell-unstable-v1-client-protocol.h

# Bar executable linking
BAR_PROTO_OBJS := \
	$(OBJDIR)/bar/wlr-layer-shell-unstable-v1-protocol.o \
	$(OBJDIR)/bar/xdg-shell-protocol.o

$(OUTDIR)/gowlbar: $(BAR_OBJS) $(BAR_PROTO_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_OBJS)
	@$(MKDIR_P) $(dir $@)
	$(CC) -o $@ $^ $(BAR_LDFLAGS) -rdynamic

# Test compilation
$(OBJDIR)/tests/%.o: tests/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

# Module compilation (generic rule)
$(OUTDIR)/modules/%.so: modules/%/*.c | $(OUTDIR)/modules
	@$(MKDIR_P) $(dir $@)
	$(CC) $(MODULE_CFLAGS) $(MODULE_LDFLAGS) -o $@ $^ $(LDFLAGS) -L$(OUTDIR) -lgowl

# yaml-glib dependency compilation (suppress warnings in vendored code)
$(OBJDIR)/deps/yaml-glib/src/%.o: $(YAMLGLIB_DIR)/src/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

# crispy dependency compilation (suppress warnings in vendored code)
$(OBJDIR)/deps/crispy/src/interfaces/%.o: $(CRISPY_DIR)/src/interfaces/%.c $(CRISPY_DIR)/src/crispy-version.h | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(OBJDIR)/deps/crispy/src/core/%.o: $(CRISPY_DIR)/src/core/%.c $(CRISPY_DIR)/src/crispy-version.h | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

# crispy version header generation
$(CRISPY_DIR)/src/crispy-version.h: $(CRISPY_DIR)/src/crispy-version.h.in
	sed \
		-e 's|@CRISPY_VERSION_MAJOR@|0|g' \
		-e 's|@CRISPY_VERSION_MINOR@|1|g' \
		-e 's|@CRISPY_VERSION_MICRO@|0|g' \
		-e 's|@CRISPY_VERSION@|0.1.0|g' \
		$< > $@

# Static library creation
$(OUTDIR)/$(LIB_STATIC): $(LIB_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_OBJS)
	@$(MKDIR_P) $(dir $@)
	$(RM) $@
	$(AR) rcs $@ $^

# Shared library creation
$(OUTDIR)/$(LIB_SHARED_FULL): $(LIB_OBJS) $(YAMLGLIB_OBJS) $(CRISPY_OBJS)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(LDFLAGS_SHARED) -o $@ $^ $(LDFLAGS)
	cd $(OUTDIR) && ln -sf $(LIB_SHARED_FULL) $(LIB_SHARED_MAJOR)
	cd $(OUTDIR) && ln -sf $(LIB_SHARED_MAJOR) $(LIB_SHARED)

# Executable linking
$(OUTDIR)/gowl: $(OBJDIR)/main.o $(OUTDIR)/$(LIB_SHARED_FULL)
	$(CC) -o $@ $(OBJDIR)/main.o -L$(OUTDIR) -lgowl $(LDFLAGS) -rdynamic -Wl,-rpath,'$$ORIGIN'

# Wayland protocol header generation rules
# wlroots headers include these via bare #include "...-protocol.h"
# so they must be on the include path. Generated into project root
# which is already covered by -I. in CFLAGS_INC.
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml $@

wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@

cursor-shape-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS_DIR)/staging/cursor-shape/cursor-shape-v1.xml $@

# The next three exist for wlroots 0.19 ONLY, and their absence is
# invisible on a machine that also has 0.20 installed.
#
# 0.19's public headers pull generated protocol headers in by bare
# name -- wlr_pointer_constraints_v1.h does
# `#include "pointer-constraints-unstable-v1-protocol.h"' -- so every
# consumer has to run wayland-scanner over the XML itself.  0.20
# replaced those with `#include <wayland-protocols/...-enum.h>', a
# header wayland-protocols now installs, so the includes resolve with
# no generation step and these rules are dead weight there.
#
# gowl picks the newest wlroots present, so a developer box carrying
# both versions never compiles the 0.19 path.  This surfaced only on
# Fedora 43, which has 0.19 and nothing else:
#
#   wlr_pointer_constraints_v1.h:17:10: fatal error:
#       pointer-constraints-unstable-v1-protocol.h: No such file
#
# tests/test-protocol-headers.sh checks every INSTALLED wlroots
# version rather than the selected one, so the next such divergence
# fails here instead of in a container on a distro nobody builds on.
#
# server-header rather than enum-header because these headers name
# interface structs as well as enums.  Generating them under 0.20 is
# harmless -- nothing includes them there.
pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@

# tablet-v2 graduated from unstable/ to stable/ in wayland-protocols
# 1.41, under a different file name.  Take whichever this machine has
# rather than pinning a path that breaks on one side of that release.
TABLET_V2_XML := $(firstword $(wildcard \
	$(WAYLAND_PROTOCOLS_DIR)/stable/tablet/tablet-v2.xml \
	$(WAYLAND_PROTOCOLS_DIR)/unstable/tablet/tablet-unstable-v2.xml))

tablet-v2-protocol.h:
	$(WAYLAND_SCANNER) server-header $(TABLET_V2_XML) $@

ext-image-copy-capture-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS_DIR)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@

# ext-workspace-v1: staging protocol for workspace discovery /
# activation by external bars.  Generates BOTH a server header and
# the private-code dispatcher; consumed by gowl-ext-workspace.c.
ext-workspace-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS_DIR)/staging/ext-workspace/ext-workspace-v1.xml $@

# wlr-output-power-management: wlroots's own header includes the
# generated server header by bare name on every version, so the XML
# vendored in protocols/ is run through the scanner here.
wlr-output-power-management-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@

ext-workspace-v1-protocol.c: ext-workspace-v1-protocol.h
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS_DIR)/staging/ext-workspace/ext-workspace-v1.xml $@

# gowl-input-capture-v1: gowl-private protocol for the InputCapture /
# RemoteDesktop portal backend.  Generates BOTH a server header and the
# private-code dispatcher from the in-tree XML; consumed by
# gowl-input-capture-protocol.c.
# The XML is a prerequisite, not just the recipe's argument.  Without it
# make only checks that the header EXISTS, so a protocol change -- a new
# request, a version bump -- silently never reaches the generated code and
# the failure surfaces as "no member named ..." in a file nobody edited.
gowl-input-capture-v1-protocol.h: protocols/gowl-input-capture-unstable-v1.xml
	$(WAYLAND_SCANNER) server-header $< $@

gowl-input-capture-v1-protocol.c: protocols/gowl-input-capture-unstable-v1.xml \
                                  gowl-input-capture-v1-protocol.h
	$(WAYLAND_SCANNER) private-code $< $@

# GIR generation
$(OUTDIR)/$(GIR_FILE): $(LIB_SRCS) $(LIB_HDRS) | $(OUTDIR)/$(LIB_SHARED_FULL)
	$(GIR_SCANNER) \
		--namespace=$(GIR_NAMESPACE) \
		--nsversion=$(GIR_VERSION) \
		--library=gowl \
		--library-path=$(OUTDIR) \
		--include=GLib-2.0 \
		--include=GObject-2.0 \
		--include=Gio-2.0 \
		--pkg=glib-2.0 \
		--pkg=gobject-2.0 \
		--pkg=gio-2.0 \
		--output=$@ \
		--warn-all \
		$(GIR_CPPFLAGS) \
		-Isrc \
		$(LIB_HDRS) $(LIB_SRCS)

# Typelib compilation
$(OUTDIR)/$(TYPELIB_FILE): $(OUTDIR)/$(GIR_FILE)
	$(GIR_COMPILER) --output=$@ $<

# Development include symlink for C config compilation
# Creates $(BUILDDIR)/include/gowl -> src/ so that
# #include <gowl/gowl.h> resolves during development
$(BUILDDIR):
	@$(MKDIR_P) $(BUILDDIR)

$(BUILDDIR)/include/gowl: | $(BUILDDIR)
	@$(MKDIR_P) $(BUILDDIR)/include
	@ln -sfn $(CURDIR)/src $(BUILDDIR)/include/gowl

# Directory creation
$(OBJDIR): | $(BUILDDIR)/include/gowl
	@$(MKDIR_P) $(OBJDIR)
	@$(MKDIR_P) $(OBJDIR)/core
	@$(MKDIR_P) $(OBJDIR)/config
	@$(MKDIR_P) $(OBJDIR)/module
	@$(MKDIR_P) $(OBJDIR)/boxed
	@$(MKDIR_P) $(OBJDIR)/interfaces
	@$(MKDIR_P) $(OBJDIR)/ipc
	@$(MKDIR_P) $(OBJDIR)/util
	@$(MKDIR_P) $(OBJDIR)/bar
	@$(MKDIR_P) $(OBJDIR)/bar/interfaces
	@$(MKDIR_P) $(OBJDIR)/tests
	@$(MKDIR_P) $(OBJDIR)/deps/yaml-glib/src
	@$(MKDIR_P) $(OBJDIR)/deps/crispy/src/interfaces
	@$(MKDIR_P) $(OBJDIR)/deps/crispy/src/core

$(OUTDIR):
	@$(MKDIR_P) $(OUTDIR)

$(OUTDIR)/modules:
	@$(MKDIR_P) $(OUTDIR)/modules

# pkg-config file generation
$(OUTDIR)/gowl.pc: gowl.pc.in | $(OUTDIR)
	sed \
		-e 's|@PREFIX@|$(PREFIX)|g' \
		-e 's|@LIBDIR@|$(LIBDIR)|g' \
		-e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
		-e 's|@VERSION@|$(VERSION)|g' \
		-e 's|@WLROOTS_PC@|$(WLROOTS_PC)|g' \
		$< > $@

# Version header generation
src/gowl-version.h: src/gowl-version.h.in
	sed \
		-e 's|@GOWL_VERSION_MAJOR@|$(VERSION_MAJOR)|g' \
		-e 's|@GOWL_VERSION_MINOR@|$(VERSION_MINOR)|g' \
		-e 's|@GOWL_VERSION_MICRO@|$(VERSION_MICRO)|g' \
		-e 's|@GOWL_VERSION@|$(VERSION)|g' \
		$< > $@

# Clean rules
.PHONY: clean clean-all
clean:
	rm -rf $(BUILDDIR)/$(BUILD_TYPE)
	rm -f src/gowl-version.h
	rm -f $(CRISPY_DIR)/src/crispy-version.h
	rm -f $(PROTO_HDRS)
	rm -f wlr-layer-shell-unstable-v1-client-protocol.h
	rm -f wlr-layer-shell-unstable-v1-protocol.c
	rm -f xdg-shell-client-protocol.h
	rm -f xdg-shell-protocol.c

clean-all:
	rm -rf $(BUILDDIR)
	rm -f src/gowl-version.h
	rm -f $(CRISPY_DIR)/src/crispy-version.h
	rm -f $(PROTO_HDRS)
	rm -f wlr-layer-shell-unstable-v1-client-protocol.h
	rm -f wlr-layer-shell-unstable-v1-protocol.c
	rm -f xdg-shell-client-protocol.h
	rm -f xdg-shell-protocol.c

# Installation rules
.PHONY: install install-lib install-bin install-bar install-bar-configs install-headers install-pc install-gir install-modules install-mcp install-portal install-desktop install-systemd

install: install-lib install-bin install-bar install-bar-configs install-headers install-pc install-desktop install-systemd
ifeq ($(BUILD_GIR),1)
install: install-gir
endif
ifeq ($(BUILD_MODULES),1)
install: install-modules
endif
ifeq ($(MCP_AVAILABLE),1)
install: install-mcp
endif
ifeq ($(LIBEIS_AVAILABLE),1)
install: install-portal
endif

install-bin: $(OBJDIR)/main.o $(OUTDIR)/$(LIB_SHARED_FULL)
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(CC) -o $(DESTDIR)$(BINDIR)/gowl $(OBJDIR)/main.o \
		-L$(OUTDIR) -lgowl $(LDFLAGS) -rdynamic \
		-Wl,-rpath,$(LIBDIR)
	chmod 755 $(DESTDIR)$(BINDIR)/gowl

install-lib: $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL)
	$(MKDIR_P) $(DESTDIR)$(LIBDIR)
	$(INSTALL_DATA) $(OUTDIR)/$(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	$(INSTALL_DATA) $(OUTDIR)/$(LIB_SHARED_FULL) $(DESTDIR)$(LIBDIR)/
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SHARED_FULL) $(LIB_SHARED_MAJOR)
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SHARED_MAJOR) $(LIB_SHARED)
	@if [ -z "$(DESTDIR)" ] && command -v ldconfig >/dev/null 2>&1; then \
		echo "  Running ldconfig..."; \
		ldconfig || true; \
	fi

install-headers:
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl
	$(INSTALL_DATA) src/gowl.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	$(INSTALL_DATA) src/gowl-types.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	$(INSTALL_DATA) src/gowl-enums.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	$(INSTALL_DATA) src/gowl-version.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	@# One loop over every source subdirectory rather than a line
	@# apiece.  Two reasons, both learned the hard way:
	@#
	@#   * A directory that no longer has headers -- src/layout, whose
	@#     layouts became modules -- made `install src/layout/*.h' fail
	@#     on the unexpanded glob, and install-headers died there.
	@#     Every directory listed AFTER it was therefore never
	@#     installed: src/ipc and src/util silently went missing from
	@#     /usr/include/gowl for as long as the rule existed.
	@#   * A new subdirectory used to need its own two lines.  src/fx
	@#     and src/barkit did not get them, so a bar plugin built
	@#     against an INSTALLED gowl could not find gowl/barkit/*.h --
	@#     which is the whole contract a plugin compiles against.
	@#
	@# Empty directories are skipped, so this stays correct whether or
	@# not a given subdirectory has headers today.
	@set -e; \
	for d in $(HEADER_SUBDIRS); do \
		set -- src/$$d/*.h; \
		[ -e "$$1" ] || continue; \
		$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/$$d; \
		$(INSTALL_DATA) "$$@" $(DESTDIR)$(INCLUDEDIR)/gowl/$$d/; \
	done

install-pc: $(OUTDIR)/gowl.pc
	$(MKDIR_P) $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL_DATA) $(OUTDIR)/gowl.pc $(DESTDIR)$(PKGCONFIGDIR)/

install-gir: $(OUTDIR)/$(GIR_FILE) $(OUTDIR)/$(TYPELIB_FILE)
	$(MKDIR_P) $(DESTDIR)$(GIRDIR)
	$(MKDIR_P) $(DESTDIR)$(TYPELIBDIR)
	$(INSTALL_DATA) $(OUTDIR)/$(GIR_FILE) $(DESTDIR)$(GIRDIR)/
	$(INSTALL_DATA) $(OUTDIR)/$(TYPELIB_FILE) $(DESTDIR)$(TYPELIBDIR)/

install-desktop:
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/wayland-sessions
	$(INSTALL_DATA) data/gowl.desktop $(DESTDIR)$(DATADIR)/wayland-sessions/
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps
	$(INSTALL_DATA) data/logo-256.png $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/gowl.png
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/gowl
	$(INSTALL_DATA) data/default-config.yaml $(DESTDIR)$(DATADIR)/gowl/default-config.yaml
	$(MKDIR_P) $(DESTDIR)$(SYSCONFDIR)/gowl
	@if [ ! -f $(DESTDIR)$(SYSCONFDIR)/gowl/config.yaml ]; then \
		$(INSTALL_DATA) data/default-config.yaml $(DESTDIR)$(SYSCONFDIR)/gowl/config.yaml; \
	fi

install-systemd:
	$(MKDIR_P) $(DESTDIR)$(SYSTEMD_USERUNITDIR)
	$(INSTALL_DATA) data/gowl-session.target $(DESTDIR)$(SYSTEMD_USERUNITDIR)/

install-modules:
	$(MKDIR_P) $(DESTDIR)$(MODULEDIR)
	@for mod in $(OUTDIR)/modules/*.so; do \
		if [ -f "$$mod" ]; then \
			$(INSTALL_DATA) "$$mod" $(DESTDIR)$(MODULEDIR)/; \
		fi \
	done

install-bar: $(OUTDIR)/gowlbar
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(OUTDIR)/gowlbar $(DESTDIR)$(BINDIR)/gowlbar
	$(MKDIR_P) $(DESTDIR)$(BAR_MODULEDIR)

install-mcp: $(OUTDIR)/gowl-mcp
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(OUTDIR)/gowl-mcp $(DESTDIR)$(BINDIR)/gowl-mcp

# InputCapture/RemoteDesktop portal backend: the binary, its .portal
# registration, the routing portals.conf, and the D-Bus .service so
# xdg-desktop-portal can activate it.  The .service @BINDIR@ is expanded
# at install time.
install-portal: $(OUTDIR)/xdg-desktop-portal-gowl
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(OUTDIR)/xdg-desktop-portal-gowl \
		$(DESTDIR)$(BINDIR)/xdg-desktop-portal-gowl
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals
	$(INSTALL_DATA) data/gowl.portal \
		$(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals/gowl.portal
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/xdg-desktop-portal
	$(INSTALL_DATA) data/gowl-portals.conf \
		$(DESTDIR)$(DATADIR)/xdg-desktop-portal/gowl-portals.conf
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/dbus-1/services
	sed 's|@BINDIR@|$(BINDIR)|g' \
		data/org.freedesktop.impl.portal.desktop.gowl.service.in \
		> $(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.impl.portal.desktop.gowl.service

install-bar-configs:
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/gowl
	$(INSTALL_DATA) data/default-bar.yaml $(DESTDIR)$(DATADIR)/gowl/default-bar.yaml
	$(INSTALL_DATA) data/example-bar.c $(DESTDIR)$(DATADIR)/gowl/example-bar.c

# Uninstall
.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/gowl
	rm -f $(DESTDIR)$(BINDIR)/gowlbar
	rm -f $(DESTDIR)$(BINDIR)/gowl-mcp
	rm -f $(DESTDIR)$(BINDIR)/xdg-desktop-portal-gowl
	rm -f $(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals/gowl.portal
	rm -f $(DESTDIR)$(DATADIR)/xdg-desktop-portal/gowl-portals.conf
	rm -f $(DESTDIR)$(DATADIR)/dbus-1/services/org.freedesktop.impl.portal.desktop.gowl.service
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_STATIC)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SHARED_FULL)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SHARED_MAJOR)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_SHARED)
	rm -rf $(DESTDIR)$(INCLUDEDIR)/gowl
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/gowl.pc
	rm -f $(DESTDIR)$(GIRDIR)/$(GIR_FILE)
	rm -f $(DESTDIR)$(TYPELIBDIR)/$(TYPELIB_FILE)
	rm -rf $(DESTDIR)$(MODULEDIR)
	rm -rf $(DESTDIR)$(BAR_MODULEDIR)
	rm -f $(DESTDIR)$(DATADIR)/gowl/default-bar.yaml
	rm -f $(DESTDIR)$(DATADIR)/gowl/example-bar.c
	rm -f $(DESTDIR)$(DATADIR)/wayland-sessions/gowl.desktop
	rm -f $(DESTDIR)$(DATADIR)/wayland-sessions/gowl-debug.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/256x256/apps/gowl.png

# Always out of date, so the stamps above are re-evaluated every run.
.PHONY: FORCE
FORCE:
