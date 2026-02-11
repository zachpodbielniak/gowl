# Gowl Architecture

## Overview

Gowl is a GObject-based Wayland compositor built on wlroots 0.19. Inspired by dwl and dwm, it reimagines their tiling window management philosophy with a full GObject type system, providing a modular, introspectable compositor with a plugin architecture based on GModule and GObject interfaces.

The design philosophy is to expose every compositor subsystem as a well-typed GObject, making it possible for modules to hook into any part of the compositor lifecycle through a set of clearly defined interfaces.

## GObject Type Hierarchy

The codebase is organized into several layers, each built on GObject fundamentals.

### Core Objects

These are the main compositor objects, all `G_DECLARE_FINAL_TYPE` singletons or per-resource objects:

| Type | Description |
|------|-------------|
| `GowlCompositor` | Main singleton. Owns the Wayland display, wlroots backend, renderer, allocator, and scene graph. |
| `GowlMonitor` | Represents a physical output (monitor). Holds the wlr_output, layout geometry, tag state, and active layout. |
| `GowlClient` | Represents a toplevel surface (window). Holds surface state, geometry, tags, floating/fullscreen state. |
| `GowlSeat` | Represents the input seat. Manages keyboard, pointer, and touch state. |
| `GowlKeyboardGroup` | Keyboard group management. Handles XKB keymap and repeat settings. |
| `GowlCursor` | Cursor management. Handles pointer motion, image, and interactive move/resize grabs. |
| `GowlLayerSurface` | Layer shell surface. Manages panels, bars, overlays per the wlr-layer-shell protocol. |
| `GowlBar` | Built-in status bar surface (optional). |
| `GowlSessionLock` | Session lock management (ext-session-lock-v1). |
| `GowlIdleManager` | Idle timeout and inhibitor management. |

### Configuration Objects

| Type | Description |
|------|-------------|
| `GowlConfig` | Holds all compositor configuration as GObject properties. Supports YAML loading, generation, keybinds, and rules. Emits `changed` and `reloaded` signals. |
| `GowlConfigCompiler` | Compiles user C config files to shared objects and loads them via `g_module_open()`. |
| `GowlKeybind` | Utility for parsing and serializing keybind strings (e.g., `"Super+Shift+Return"`). |

### Module System Objects

| Type | Description |
|------|-------------|
| `GowlModule` | Abstract base class (`G_DECLARE_DERIVABLE_TYPE`). All modules subclass this, overriding virtual methods: `activate`, `deactivate`, `get_name`, `get_description`, `get_version`, `configure`. |
| `GowlModuleManager` | Manages module lifecycle: loading `.so` files, registering types, activating/deactivating, and dispatching events through interface-specific arrays sorted by priority. |
| `GowlModuleInfo` | Boxed type holding module metadata (name, description, version). |

### Boxed Types

Small value types registered with `g_boxed_type_register_static()`:

| Type | Description |
|------|-------------|
| `GowlGeometry` | Rectangle: x, y, width, height. |
| `GowlColor` | RGBA color (float components). |
| `GowlKeyCombo` | Modifier bitmask + keysym pair. |
| `GowlTagMask` | 32-bit tag bitmask with utility functions. |
| `GowlGaps` | Inner/outer gap sizes for vanity gaps. |
| `GowlBorderSpec` | Border width + color triplet (focus, unfocus, urgent). |
| `GowlRule` | Window rule: app_id, title, tags, floating, monitor. |
| `GowlOutputMode` | Monitor mode: width, height, refresh rate. |

### Enumerations

All enums are registered as GLib enum types for introspection:

| Enum | Description |
|------|-------------|
| `GowlHookPoint` | 39 compositor event hook points (startup, shutdown, key press, client map, etc.). |
| `GowlCursorMode` | Normal, Move, Resize. |
| `GowlDirection` | Up, Down, Left, Right. |
| `GowlLayerShellLayer` | Background, Bottom, Top, Overlay. |
| `GowlKeyMod` | Bitmask flags for modifier keys (Shift, Ctrl, Alt, Logo, etc.). |
| `GowlClientState` | Tiled, Floating, Fullscreen. |
| `GowlAction` | 21 compositor actions (spawn, kill_client, focus_stack, tag_view, quit, etc.). |
| `GowlConfigSource` | Builtin, YAML, C Module, Merged. |
| `GowlIdleState` | Active, Idle, Locked. |

### Layout Objects

| Type | Description |
|------|-------------|
| `GowlLayoutTile` | Master-stack tiling layout (implements `GowlLayoutProvider`). |
| `GowlLayoutMonocle` | Monocle (fullscreen stacking) layout. |
| `GowlLayoutFloat` | Floating layout (no automatic arrangement). |

## Module / Plugin System

### How Modules Work

Modules are shared objects (`.so` files) that export a single entry point:

```c
G_MODULE_EXPORT GType
gowl_module_register(void)
{
    return MY_TYPE_MODULE;
}
```

The `GowlModuleManager` loads the `.so` via `g_module_open()`, calls `gowl_module_register()` to get the `GType`, instantiates the module, and classifies it into dispatch arrays based on which interfaces the module implements.

### Module Lifecycle

1. **Load** -- `g_module_open()` on the `.so` file.
2. **Register** -- `gowl_module_register()` returns the module's GType.
3. **Instantiate** -- `g_object_new()` creates the module instance.
4. **Classify** -- The module manager checks which interfaces the instance implements and adds it to the corresponding dispatch arrays.
5. **Activate** -- `gowl_module_activate()` calls the virtual `activate` method.
6. **Dispatch** -- Events are dispatched to interface-specific arrays in priority order.
7. **Deactivate** -- `gowl_module_deactivate()` on shutdown or removal.

### Module Priority

Each module has a `priority` property (integer). Dispatch arrays are sorted in ascending priority order (lower value = dispatched first). This allows modules to control ordering, for example a keybind handler module with priority 0 runs before one with priority 100.

### Module Signals

`GowlModuleManager` emits:
- `module-loaded` -- after successful registration
- `module-activated` -- after successful activation
- `module-deactivated` -- after deactivation
- `module-error` -- on any module operation failure

### Module Search Paths

Modules are loaded from:
1. `$GOWL_MODULEDIR` (compile-time default: `/usr/local/lib/gowl/modules/`)
2. `~/.local/lib/gowl/modules/`
3. The build output directory `build/release/modules/` (for development)

## Interface-Based Hook System

Gowl defines 18 GObject interfaces. A module implements one or more of these to hook into compositor events. The `GowlModuleManager` maintains a dispatch array per interface.

### Interface List

| Interface | Methods | Dispatch Semantics |
|-----------|---------|-------------------|
| `GowlLayoutProvider` | `arrange(monitor, clients, area)`, `get_symbol()` | First matching provider arranges the monitor. |
| `GowlKeybindHandler` | `handle_key(modifiers, keysym, pressed)` | First handler returning TRUE consumes the event. |
| `GowlMouseHandler` | `handle_button(button, state, modifiers)`, `handle_motion(x, y)`, `handle_axis(value, orientation)` | First handler returning TRUE consumes the event. |
| `GowlClientDecorator` | `decorate(client)`, `undecorate(client)` | All decorators are called (broadcast). |
| `GowlClientPlacer` | `place(client, monitor)` | First placer returning TRUE claims placement. |
| `GowlFocusPolicy` | `should_focus(client)`, `focus_changed(client)` | First policy returning FALSE vetoes focus. |
| `GowlMonitorConfigurator` | `configure(monitor, mode)` | First configurator returning TRUE claims the monitor. |
| `GowlRuleProvider` | `get_rules()`, `match(client)` | All providers are queried; rules are merged. |
| `GowlStartupHandler` | `on_startup(compositor)` | All handlers called (broadcast). |
| `GowlShutdownHandler` | `on_shutdown(compositor)` | All handlers called (broadcast). |
| `GowlIpcHandler` | `handle_command(command, args)` | First handler returning a result claims the command. |
| `GowlTagManager` | `view_tag(monitor, mask)`, `set_tag(client, mask)` | First manager returning TRUE claims the operation. |
| `GowlGapProvider` | `get_gaps(monitor)` | First provider returning non-NULL gaps wins. |
| `GowlBarProvider` | `render_bar(monitor, surface)`, `get_bar_height()` | First provider returning TRUE claims the bar. |
| `GowlScratchpadHandler` | `toggle_scratchpad(name)`, `add_to_scratchpad(client, name)` | First handler returning TRUE claims the operation. |
| `GowlSwallowHandler` | `should_swallow(parent, child)`, `swallow(parent, child)`, `unswallow(parent)` | First handler returning TRUE claims the swallow. |
| `GowlStickyHandler` | `is_sticky(client)`, `set_sticky(client, sticky)` | First handler returning TRUE claims the operation. |
| `GowlCursorProvider` | `get_cursor_image(mode)`, `set_cursor(image)` | First provider returning non-NULL claims the cursor. |

### Dispatch Semantics

There are two dispatch patterns:

1. **Consumable** -- The event is dispatched to handlers in priority order. The first handler that returns `TRUE` (or a non-NULL result) consumes the event and no further handlers are called. Used by: `GowlKeybindHandler`, `GowlMouseHandler`, `GowlClientPlacer`, `GowlIpcHandler`, etc.

2. **Broadcast** -- Every handler is called regardless of return value. Used by: `GowlStartupHandler`, `GowlShutdownHandler`, `GowlClientDecorator`.

## Configuration System

### Dual Config Architecture

Gowl supports two configuration mechanisms that work together:

1. **YAML Config** (`gowl.yaml`) -- Declarative configuration for all standard settings, keybinds, rules, and module options. Parsed at startup.

2. **C Config** (`config.c`) -- Compiled to a `.so` and loaded via `g_module_open()`. The exported `gowl_config_init()` function runs after YAML loading, allowing it to override or supplement YAML values programmatically.

### Config Precedence

From lowest to highest priority:

1. Built-in defaults (hardcoded in `GowlConfig` property defaults)
2. YAML configuration file
3. C configuration module (`gowl_config_init()`)
4. CLI arguments

### YAML Search Paths

The config loader searches for `gowl.yaml` in this order (first match wins):

1. `./data/gowl.yaml` (project directory, for development)
2. `~/.config/gowl/gowl.yaml` (user config, recommended)
3. `/etc/gowl/gowl.yaml` (system-wide config)
4. `/usr/local/gowl/gowl.yaml` (installation default)

### C Config Search Paths

The config compiler searches for `config.c` in:

1. `~/.config/gowl/config.c` (user config)
2. `/etc/gowl/config.c` (system-wide config)

The compiled `.so` is cached in `$XDG_CACHE_HOME/gowl/config.so`. It is recompiled only if the source is newer than the cached `.so`.

### Config Properties

All configuration values are exposed as GObject properties on `GowlConfig`, making them accessible via `g_object_get()` / `g_object_set()` and through GObject introspection bindings. The `changed` signal is emitted whenever a property changes, and `reloaded` after a full reload.

## Build System

The build system uses GNU Make with two main files:

- `config.mk` -- All configurable variables (version, paths, flags, dependency lists, build options).
- `rules.mk` -- Pattern rules for compiling objects, linking libraries, generating protocols, and installation.
- `Makefile` -- Top-level orchestration: source lists, targets, test runner.

### Build Outputs

| Target | Output |
|--------|--------|
| `make` | `build/release/gowl` (executable), `build/release/libgowl.so.0.1.0` (shared lib), `build/release/libgowl.a` (static lib), modules in `build/release/modules/` |
| `make DEBUG=1` | Same structure under `build/debug/` with `-g -O0` |
| `make gir` | `Gowl-0.1.gir` and `Gowl-0.1.typelib` for GObject introspection |
| `make test` | Test binaries in `build/release/test-*` |

### Key Build Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DEBUG` | 0 | Enable debug build |
| `ASAN` | 0 | Enable AddressSanitizer (requires DEBUG=1) |
| `BUILD_XWAYLAND` | 1 | Enable XWayland support |
| `BUILD_GIR` | 0 | Generate GObject introspection data |
| `BUILD_TESTS` | 1 | Build test suite |
| `BUILD_MODULES` | 1 | Build bundled modules |
| `PREFIX` | /usr/local | Installation prefix |
