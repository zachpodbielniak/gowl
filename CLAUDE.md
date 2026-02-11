# CLAUDE.md

This file provides guidance to Claude Code when working with the gowl codebase.

Gowl is a GObject-based Wayland compositor inspired by dwl and dwm. It is not a direct port -- it reimagines their tiling window management approach with a full GObject type system, modular plugin architecture, and introspection support.

## Build Commands

```bash
make                    # Build all (lib, executable, modules) in release mode
make DEBUG=1            # Build with debug symbols and -O0
make DEBUG=1 ASAN=1     # Build with AddressSanitizer
make test               # Build and run the test suite (GTest)
make clean              # Clean current build type
make clean-all          # Clean all build artifacts
make install            # Install to PREFIX (default /usr/local)
make install-deps       # Install Fedora dependencies via dnf
make check-deps         # Verify all pkg-config dependencies
make show-config        # Print current build configuration
make gir                # Generate GObject Introspection data
make modules            # Build only the modules
make help               # Show all available targets
```

## Test Commands

```bash
make test               # Run all tests
```

Test binaries are in `build/release/` (or `build/debug/` with DEBUG=1):
- `test-boxed` -- Boxed type tests (geometry, color, key combo, etc.)
- `test-config` -- Config property and YAML loading tests
- `test-enums` -- Enum type registration tests
- `test-keybind` -- Keybind parsing/serialization tests
- `test-layout` -- Layout provider interface tests
- `test-module` -- Module lifecycle and registration tests

## Code Style

- **C standard**: `gnu89` exclusively (`-std=gnu89`)
- **Compiler**: `gcc` only
- **Indentation**: TAB characters (not spaces)
- **Naming conventions**:
  - Defines/preprocessor: `UPPERCASE_SNAKE_CASE` (e.g., `GOWL_TYPE_MODULE`)
  - Types/structs: `PascalCase` (e.g., `GowlCompositor`, `GowlKeybindEntry`)
  - Variables/functions: `lowercase_snake_case` (e.g., `gowl_config_new`)
  - GObject type macros follow glib convention: `GOWL_TYPE_*`, `GOWL_IS_*`, `GOWL_*_CLASS`
- **Comments**: Always `/* */`, never `//`. Use GObject Introspection compatible doc comments.
- **Memory management**: Use `g_autoptr()`, `g_autofree`, `g_steal_pointer()`, `g_clear_object()`, `g_clear_pointer()`.
- **Error handling**: Use `GError` pattern. Log errors to stderr via `g_warning()` / `g_printerr()`.
- **Function style**:
  ```c
  GowlConfig *
  gowl_config_new(void)
  {
      return (GowlConfig *)g_object_new(GOWL_TYPE_CONFIG, NULL);
  }
  ```
- **Variable declarations at top of block** (gnu89 requirement).

## Source Organization

```
src/
  gowl.h                   # Master include header
  gowl-types.h             # Forward declarations for all types
  gowl-enums.h             # All GLib-registered enumerations
  gowl-enums.c             # Enum type registration implementations
  gowl-version.h.in        # Version header template (sed-expanded)
  main.c                   # Entry point, CLI parsing, compositor startup

  boxed/                    # Boxed value types (GowlGeometry, GowlColor, etc.)
  interfaces/               # 18 GObject interfaces (GowlLayoutProvider, etc.)
  module/                   # Module system (GowlModule, GowlModuleManager, GowlModuleInfo)
  config/                   # Configuration (GowlConfig, GowlConfigCompiler, GowlKeybind)
  core/                     # Core compositor objects (GowlCompositor, GowlMonitor, GowlClient, etc.)
  layout/                   # Built-in layouts (tile, monocle, float)
  ipc/                      # IPC socket handling
  util/                     # Utilities (logging)
  protocols/                # Generated Wayland protocol headers

data/                       # Default config files (YAML and C)
deps/yaml-glib/             # yaml-glib submodule (built-in dependency)
modules/                    # Bundled module sources (autostart, vanitygaps, pertag, etc.)
tests/                      # GTest test sources
docs/                       # Documentation (architecture, building, configuration, modules)
```

## Dependencies (pkg-config names)

### Required
- `glib-2.0`, `gobject-2.0`, `gio-2.0`, `gmodule-2.0`
- `wlroots-0.19`, `wayland-server`, `wayland-protocols`
- `xkbcommon`, `libinput`
- `yaml-0.1`, `json-glib-1.0`

### Optional
- `xcb`, `xcb-icccm` (XWayland support, controlled by `BUILD_XWAYLAND`)
- `gobject-introspection-1.0` (GIR generation, controlled by `BUILD_GIR`)

## Architecture Notes

### Type Hierarchy

- All core types (`GowlCompositor`, `GowlMonitor`, `GowlClient`, etc.) are `G_DECLARE_FINAL_TYPE`.
- `GowlModule` is `G_DECLARE_DERIVABLE_TYPE` -- all modules subclass it.
- 18 interfaces (`GowlLayoutProvider`, `GowlKeybindHandler`, etc.) are `G_DECLARE_INTERFACE`.
- 8 boxed types for value types (`GowlGeometry`, `GowlColor`, etc.).
- 9 registered enum types (`GowlAction`, `GowlKeyMod`, `GowlHookPoint`, etc.).

### Module System

- Modules are `.so` files loaded via `g_module_open()`.
- Each exports `gowl_module_register()` returning a GType.
- `GowlModuleManager` maintains per-interface dispatch arrays sorted by priority.
- Dispatch is either consumable (first TRUE return wins) or broadcast (all called).

### Config System

- `GowlConfig` holds all settings as GObject properties.
- YAML config searched in: `./data/`, `~/.config/gowl/`, `/etc/gowl/`, `/usr/local/gowl/`.
- C config (`config.c`) compiled to `.so` via `GowlConfigCompiler`, cached in `$XDG_CACHE_HOME/gowl/`.
- C config entry point: `G_MODULE_EXPORT gboolean gowl_config_init(void)`.
- Precedence: defaults < YAML < C config < CLI args.

### Build System

- `config.mk` -- All variables and pkg-config logic.
- `rules.mk` -- Pattern rules, linking, installation.
- `Makefile` -- Source lists, top-level targets, test runner.
- yaml-glib is built as part of the project (submodule in `deps/yaml-glib/`).

## Things to Avoid

- Never use `//` comments -- always `/* */`.
- Never use meson or cmake -- this project uses GNU Make exclusively.
- Never declare variables mid-block -- declarations go at the top (gnu89).
- Never use `camelCase` for C identifiers.
- Do not run `make install` without explicit user request.
