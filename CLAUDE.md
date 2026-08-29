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
- `test-focus-rules` -- Keyboard-focus arbitration + client close routing
- `test-input-recorder` -- Input recording: consent gate, bounded ring,
  motion coalescing, self-stop deadline, secret-suppression policy,
  payload shape, and the config-to-recorder wiring

`make test` also runs every `tests/*.sh` **source guard** before the compiled
tests. These assert invariants no unit test can reach:
- `test-no-libregnum.sh` -- gowl links no rendering engine (see below)
- `test-close-guard.sh` -- `wlr_xdg_toplevel_send_close` /
  `wlr_xwayland_surface_close` have exactly one call site each (inside
  `gowl_client_close()`), `gowl_compositor_focus_client()` still consults
  `gowl_focus_decide()`, and no new `wlr_seat_keyboard_notify_enter` call
  sites have appeared
- `test-record-guard.sh` -- the input recorder's taps are still wired to
  all six input hooks, the injection helpers and `motionnotify` are still
  *un*wired (they are reached by both the real and the synthetic path, so
  a tap there would record gowl's own injected input), `on_kb_key` still
  carries the Super+Shift+Escape force-stop, and the on-screen indicator
  is still raised from the recorder's own state change

> **Input *recording* is not input *injection*, and they must never share a
> switch.** `GowlInputRecorder` (`src/core/gowl-input-recorder.c`) observes
> real key and pointer events for teach-a-task. It is gated by its own
> `input-recording` config key, off by default, which nothing that enables
> `send_key` or the RemoteDesktop portal may imply -- an agent allowed to
> click must not thereby be allowed to watch somebody type. The ring is
> bounded and reports what it `dropped`; the recording stops itself; a red
> frame is drawn for as long as it runs; and `Super+Shift+Escape` stops one
> without its token. gowl **cannot** recognise a password field (Wayland
> gives a compositor no window-internal knowledge) -- only the lock screen
> and an app-id/title deny list. Say so rather than implying otherwise. See
> `docs/input-recording.org`.

> **Route every client close through `gowl_client_close()`, and every focus
> change through `gowl_compositor_focus_client()`.** An X11 client's
> `xdg_toplevel` is `NULL`, so a bare `wlr_xdg_toplevel_send_close()` crashes
> the compositor — which under `emacs --gowl` *is* the user's whole session.
> Likewise, a seat focus move that skips `gowl_focus_decide()` can steal the
> keyboard from a mapped launcher and leave it visible but deaf. Embedders that
> must move seat focus themselves have to call
> `gowl_compositor_has_exclusive_keyboard_layer()` first. See *Keyboard Focus
> Arbitration* in `docs/architecture.org`.

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
- `wlroots-0.19` **or** `wlroots-0.20` (newest present wins; `make WLROOTS=0.19`
  to pin — 0.20 adds per-window screencast capture, see `gowl-wlroots-compat.h`),
  `wayland-server`, `wayland-protocols`
- `xkbcommon`, `libinput`
- `yaml-0.1`, `json-glib-1.0`

### Optional
- `xcb`, `xcb-icccm` (XWayland support, controlled by `BUILD_XWAYLAND`)
- `gobject-introspection-1.0` (GIR generation, controlled by `BUILD_GIR`)
- `libdecor-0` (window decorations for nested Wayland sessions, auto-detected)

> **Do not add a rendering-engine dependency.** The raw-frame sink
> (`src/core/gowl-frame-sink.{c,h}`, see *Raw Frame Sink* in
> `docs/architecture.org`) lets external producers (e.g. cmacs screensavers)
> hand gowl finished ARGB8888 pixels — gowl must stay free of `libregnum` /
> `graylib` / `raylib` / GL. `tests/test-no-libregnum.sh` (run by `make test`)
> enforces this and will fail the build if those symbols/headers leak in.

## Architecture Notes

### Type Hierarchy

- All core types (`GowlCompositor`, `GowlMonitor`, `GowlClient`, etc.) are `G_DECLARE_FINAL_TYPE`.
- `GowlModule` is `G_DECLARE_DERIVABLE_TYPE` -- all modules subclass it.
- 18 interfaces (`GowlLayoutProvider`, `GowlKeybindHandler`, etc.) are `G_DECLARE_INTERFACE`.
- Boxed value types (`GowlGeometry`, `GowlColor`, `GowlInputZone`, `GowlInputBarrier`, etc.).
- 9 registered enum types (`GowlAction`, `GowlKeyMod`, `GowlHookPoint`, etc.).
- A gowl-private Wayland protocol `gowl-input-capture-unstable-v1` (alongside
  `ext-workspace`) backs the InputCapture/RemoteDesktop portals; the
  `GowlInputCapture` state machine + the `xdg-desktop-portal-gowl` binary
  implement them. See `docs/input-capture.org`.

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
