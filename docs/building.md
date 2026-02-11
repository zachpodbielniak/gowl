# Building Gowl

## Dependencies

### Required (pkg-config names)

| Package | pkg-config name | Description |
|---------|----------------|-------------|
| GLib 2.0 | `glib-2.0` | Core utility library |
| GObject 2.0 | `gobject-2.0` | GObject type system |
| GIO 2.0 | `gio-2.0` | GLib I/O library |
| GModule 2.0 | `gmodule-2.0` | Dynamic module loading |
| wlroots 0.19 | `wlroots-0.19` | Wayland compositor library |
| wayland-server | `wayland-server` | Wayland server protocol |
| wayland-protocols | `wayland-protocols` | Wayland protocol XML files |
| xkbcommon | `xkbcommon` | XKB keyboard handling |
| libinput | `libinput` | Input device handling |
| libyaml | `yaml-0.1` | YAML parser (for yaml-glib) |
| json-glib | `json-glib-1.0` | JSON/GLib integration |

### Optional

| Package | pkg-config name | Description |
|---------|----------------|-------------|
| libxcb | `xcb` | XWayland support |
| xcb-icccm | `xcb-icccm` | XWayland ICCCM support |
| GObject Introspection | `gobject-introspection-1.0` | GIR/typelib generation |

### Build Tools

- `gcc` (GNU C Compiler)
- `make` (GNU Make)
- `pkg-config`
- `wayland-scanner` (from wayland-devel)

## Installing Dependencies

### Fedora (dnf)

This is the primary supported platform. If using Fedora Silverblue with a distrobox, run these inside the container.

```bash
sudo dnf install -y \
    gcc make pkg-config \
    glib2-devel \
    gobject-introspection-devel \
    json-glib-devel \
    wayland-devel \
    wayland-protocols-devel \
    wlroots-devel \
    libxkbcommon-devel \
    libinput-devel \
    libyaml-devel \
    libxcb-devel \
    xcb-util-wm-devel
```

Or use the built-in Makefile target:

```bash
make install-deps
```

### Verifying Dependencies

Check that all required dependencies are available:

```bash
make check-deps
```

This will print the version of each found dependency and flag any that are missing.

## Building

### Quick Build

```bash
make
```

This builds the shared library, static library, executable, and all bundled modules in release mode. Output goes to `build/release/`.

### Debug Build

```bash
make DEBUG=1
```

Builds with `-g -O0 -DDEBUG`. Output goes to `build/debug/`.

### Debug Build with AddressSanitizer

```bash
make DEBUG=1 ASAN=1
```

Adds `-fsanitize=address -fsanitize=undefined` to both compile and link flags.

### Without XWayland

```bash
make BUILD_XWAYLAND=0
```

Disables XWayland support. This removes the `xcb` and `xcb-icccm` dependencies.

### GObject Introspection

```bash
make BUILD_GIR=1
```

Generates `Gowl-0.1.gir` and `Gowl-0.1.typelib` for use with language bindings (Python, JavaScript, etc.).

### Running Tests

```bash
make test
```

Builds the test suite (using GLib's GTest framework) and runs all test binaries. Each test binary exercises a specific subsystem:

- `test-boxed` -- Boxed type copy/free, equality, serialization
- `test-config` -- Config property defaults, YAML loading, keybind/rule management
- `test-enums` -- Enum type registration and value validation
- `test-keybind` -- Keybind string parsing and serialization
- `test-layout` -- Layout provider interface dispatch
- `test-module` -- Module lifecycle, registration, activation

### Viewing Build Configuration

```bash
make show-config
```

Prints the current values of all build variables (compiler flags, paths, enabled features).

## Build Options Summary

| Variable | Default | Description |
|----------|---------|-------------|
| `DEBUG` | `0` | `1` enables debug symbols and `-O0` |
| `ASAN` | `0` | `1` enables AddressSanitizer (requires `DEBUG=1`) |
| `BUILD_XWAYLAND` | `1` | `0` disables XWayland support |
| `BUILD_GIR` | `0` | `1` enables GObject Introspection generation |
| `BUILD_TESTS` | `1` | `0` disables test building |
| `BUILD_MODULES` | `1` | `0` skips module building |
| `PREFIX` | `/usr/local` | Installation prefix |
| `CC` | `gcc` | C compiler |

All options can be set on the command line:

```bash
make DEBUG=1 BUILD_GIR=1 PREFIX=/usr
```

## Installation

```bash
make install
```

Installs to `$PREFIX` (default `/usr/local`):

- `$PREFIX/bin/gowl` -- Compositor executable
- `$PREFIX/lib/libgowl.so.*` -- Shared library
- `$PREFIX/lib/libgowl.a` -- Static library
- `$PREFIX/include/gowl/` -- Header files
- `$PREFIX/lib/pkgconfig/gowl.pc` -- pkg-config file
- `$PREFIX/lib/gowl/modules/` -- Module shared objects
- `$PREFIX/share/gir-1.0/` -- GIR file (if `BUILD_GIR=1`)
- `$PREFIX/lib/girepository-1.0/` -- Typelib (if `BUILD_GIR=1`)

Use `DESTDIR` for staged installs (packaging):

```bash
make install DESTDIR=/tmp/gowl-pkg
```

### Uninstall

```bash
make uninstall
```

## Cleaning

```bash
# Clean current build type (release or debug)
make clean

# Clean everything (both release and debug)
make clean-all
```

## Development Workflow

A typical development cycle:

```bash
# Build debug with all modules
make DEBUG=1

# Run tests
make test

# Run the compositor (in a nested Wayland session or TTY)
./build/debug/gowl

# Run with debug logging
./build/debug/gowl --debug

# Run with a custom config
./build/debug/gowl --config ./my-config.yaml
```
