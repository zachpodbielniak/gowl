# rules.mk - Gowl Build Rules
# Pattern rules and common build recipes

# Wayland protocol headers (must be defined before use in dependency rules)
PROTO_HDRS := \
	xdg-shell-protocol.h \
	wlr-layer-shell-unstable-v1-protocol.h \
	cursor-shape-v1-protocol.h

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

$(OBJDIR)/layout/%.o: src/layout/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/ipc/%.o: src/ipc/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/util/%.o: src/util/%.c | $(OBJDIR)
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

$(OUTDIR)/gowlbar: $(BAR_OBJS) $(BAR_PROTO_OBJS) $(YAMLGLIB_OBJS)
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

# yaml-glib dependency compilation
$(OBJDIR)/deps/yaml-glib/src/%.o: deps/yaml-glib/src/%.c | $(OBJDIR)
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Static library creation
$(OUTDIR)/$(LIB_STATIC): $(LIB_OBJS) $(YAMLGLIB_OBJS)
	@$(MKDIR_P) $(dir $@)
	$(AR) rcs $@ $^

# Shared library creation
$(OUTDIR)/$(LIB_SHARED_FULL): $(LIB_OBJS) $(YAMLGLIB_OBJS)
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
	@$(MKDIR_P) $(OBJDIR)/layout
	@$(MKDIR_P) $(OBJDIR)/ipc
	@$(MKDIR_P) $(OBJDIR)/util
	@$(MKDIR_P) $(OBJDIR)/bar
	@$(MKDIR_P) $(OBJDIR)/bar/interfaces
	@$(MKDIR_P) $(OBJDIR)/tests
	@$(MKDIR_P) $(OBJDIR)/deps/yaml-glib/src

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
	rm -f $(PROTO_HDRS)
	rm -f wlr-layer-shell-unstable-v1-client-protocol.h
	rm -f wlr-layer-shell-unstable-v1-protocol.c
	rm -f xdg-shell-client-protocol.h
	rm -f xdg-shell-protocol.c

clean-all:
	rm -rf $(BUILDDIR)
	rm -f src/gowl-version.h
	rm -f $(PROTO_HDRS)
	rm -f wlr-layer-shell-unstable-v1-client-protocol.h
	rm -f wlr-layer-shell-unstable-v1-protocol.c
	rm -f xdg-shell-client-protocol.h
	rm -f xdg-shell-protocol.c

# Installation rules
.PHONY: install install-lib install-bin install-bar install-bar-configs install-headers install-pc install-gir install-modules install-desktop

install: install-lib install-bin install-bar install-bar-configs install-headers install-pc install-desktop
ifeq ($(BUILD_GIR),1)
install: install-gir
endif
ifeq ($(BUILD_MODULES),1)
install: install-modules
endif

install-bin: $(OUTDIR)/gowl
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(OUTDIR)/gowl $(DESTDIR)$(BINDIR)/gowl

install-lib: $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_SHARED_FULL)
	$(MKDIR_P) $(DESTDIR)$(LIBDIR)
	$(INSTALL_DATA) $(OUTDIR)/$(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	$(INSTALL_DATA) $(OUTDIR)/$(LIB_SHARED_FULL) $(DESTDIR)$(LIBDIR)/
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SHARED_FULL) $(LIB_SHARED_MAJOR)
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SHARED_MAJOR) $(LIB_SHARED)

install-headers:
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl
	$(INSTALL_DATA) src/gowl.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	$(INSTALL_DATA) src/gowl-types.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	$(INSTALL_DATA) src/gowl-enums.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	$(INSTALL_DATA) src/gowl-version.h $(DESTDIR)$(INCLUDEDIR)/gowl/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/core
	$(INSTALL_DATA) src/core/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/core/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/boxed
	$(INSTALL_DATA) src/boxed/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/boxed/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/config
	$(INSTALL_DATA) src/config/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/config/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/module
	$(INSTALL_DATA) src/module/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/module/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/interfaces
	$(INSTALL_DATA) src/interfaces/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/interfaces/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/layout
	$(INSTALL_DATA) src/layout/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/layout/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/ipc
	$(INSTALL_DATA) src/ipc/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/ipc/
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/gowl/util
	$(INSTALL_DATA) src/util/*.h $(DESTDIR)$(INCLUDEDIR)/gowl/util/

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

install-bar-configs:
	$(MKDIR_P) $(DESTDIR)$(DATADIR)/gowl
	$(INSTALL_DATA) data/default-bar.yaml $(DESTDIR)$(DATADIR)/gowl/default-bar.yaml
	$(INSTALL_DATA) data/example-bar.c $(DESTDIR)$(DATADIR)/gowl/example-bar.c

# Uninstall
.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/gowl
	rm -f $(DESTDIR)$(BINDIR)/gowlbar
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
