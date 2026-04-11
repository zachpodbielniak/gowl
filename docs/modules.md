# Gowl Module Development Guide

## Overview

Gowl modules are shared object (`.so`) plugins loaded at runtime via GModule (`g_module_open()`). Each module is a GObject subclass of `GowlModule` that implements one or more gowl interfaces to hook into compositor events.

## Module Architecture

### Base Class: GowlModule

Every module subclasses `GowlModule`, which is declared as a derivable type:

```c
G_DECLARE_DERIVABLE_TYPE(GowlModule, gowl_module, GOWL, MODULE, GObject)
```

The class structure provides virtual methods:

| Virtual Method | Signature | Description |
|---------------|-----------|-------------|
| `activate` | `gboolean (*)(GowlModule *)` | Called when the module is activated. Return TRUE on success. |
| `deactivate` | `void (*)(GowlModule *)` | Called when the module is deactivated. |
| `get_name` | `const gchar *(*)(GowlModule *)` | Return the human-readable module name. |
| `get_description` | `const gchar *(*)(GowlModule *)` | Return a short description. |
| `get_version` | `const gchar *(*)(GowlModule *)` | Return the version string. |
| `configure` | `void (*)(GowlModule *, gpointer)` | Apply configuration data. |

### Properties

`GowlModule` provides these GObject properties:

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `priority` | int | 100 | Dispatch priority (lower = dispatched first) |
| `is-active` | bool | FALSE | Whether the module is currently active (read-only) |

### Entry Point

Every `.so` module must export exactly one function:

```c
G_MODULE_EXPORT GType
gowl_module_register(void)
{
    return MY_TYPE_MODULE;
}
```

This function returns the GType of the module's `GowlModule` subclass. The `GowlModuleManager` calls this to discover the module type, then instantiates it with `g_object_new()`.

## Available Interfaces

A module implements interfaces to receive events. The `GowlModuleManager` checks which interfaces each module implements at registration time and adds it to the corresponding dispatch arrays.

### GowlLayoutProvider

Provide a custom layout algorithm.

```c
struct _GowlLayoutProviderInterface {
    GTypeInterface parent_iface;
    void         (*arrange)    (GowlLayoutProvider *self,
                                gpointer monitor, GList *clients,
                                gpointer area);
    const gchar *(*get_symbol) (GowlLayoutProvider *self);
};
```

- `arrange` -- Arrange clients within the given area on the given monitor.
- `get_symbol` -- Return the layout symbol string (e.g., `"[]="`).

### GowlKeybindHandler

Handle keyboard events.

```c
struct _GowlKeybindHandlerInterface {
    GTypeInterface parent_iface;
    gboolean (*handle_key) (GowlKeybindHandler *self,
                            guint modifiers, guint keysym,
                            gboolean pressed);
};
```

- Return `TRUE` to consume the event, `FALSE` to pass it to the next handler.

### GowlMouseHandler

Handle pointer (mouse) events.

```c
struct _GowlMouseHandlerInterface {
    GTypeInterface parent_iface;
    gboolean (*handle_button) (GowlMouseHandler *self,
                               guint button, guint state,
                               guint modifiers);
    gboolean (*handle_motion) (GowlMouseHandler *self,
                               gdouble x, gdouble y);
    gboolean (*handle_axis)   (GowlMouseHandler *self,
                               gdouble value, guint orientation);
};
```

### GowlClientDecorator

Decorate/undecorate client windows.

```c
struct _GowlClientDecoratorInterface {
    GTypeInterface parent_iface;
    void (*decorate)   (GowlClientDecorator *self, gpointer client);
    void (*undecorate) (GowlClientDecorator *self, gpointer client);
};
```

### GowlClientPlacer

Control initial placement of new clients.

```c
struct _GowlClientPlacerInterface {
    GTypeInterface parent_iface;
    gboolean (*place) (GowlClientPlacer *self,
                       gpointer client, gpointer monitor);
};
```

### GowlFocusPolicy

Control focus behavior.

```c
struct _GowlFocusPolicyInterface {
    GTypeInterface parent_iface;
    gboolean (*should_focus)  (GowlFocusPolicy *self, gpointer client);
    void     (*focus_changed) (GowlFocusPolicy *self, gpointer client);
};
```

### GowlMonitorConfigurator

Configure monitor settings on hotplug.

```c
struct _GowlMonitorConfiguratorInterface {
    GTypeInterface parent_iface;
    gboolean (*configure) (GowlMonitorConfigurator *self,
                           gpointer monitor, gpointer mode);
};
```

### GowlRuleProvider

Provide window matching rules.

```c
struct _GowlRuleProviderInterface {
    GTypeInterface parent_iface;
    GPtrArray *(*get_rules) (GowlRuleProvider *self);
    gpointer   (*match)     (GowlRuleProvider *self, gpointer client);
};
```

### GowlStartupHandler

Run code at compositor startup.

```c
struct _GowlStartupHandlerInterface {
    GTypeInterface parent_iface;
    void (*on_startup) (GowlStartupHandler *self, gpointer compositor);
};
```

### GowlShutdownHandler

Run code at compositor shutdown.

```c
struct _GowlShutdownHandlerInterface {
    GTypeInterface parent_iface;
    void (*on_shutdown) (GowlShutdownHandler *self, gpointer compositor);
};
```

### GowlIpcHandler

Handle IPC commands.

```c
struct _GowlIpcHandlerInterface {
    GTypeInterface parent_iface;
    gchar *(*handle_command) (GowlIpcHandler *self,
                              const gchar *command, const gchar *args);
};
```

### GowlTagManager

Manage tag operations.

```c
struct _GowlTagManagerInterface {
    GTypeInterface parent_iface;
    gboolean (*view_tag) (GowlTagManager *self,
                          gpointer monitor, guint32 mask);
    gboolean (*set_tag)  (GowlTagManager *self,
                          gpointer client, guint32 mask);
};
```

### GowlGapProvider

Provide gap sizes for layouts.

```c
struct _GowlGapProviderInterface {
    GTypeInterface parent_iface;
    gpointer (*get_gaps) (GowlGapProvider *self, gpointer monitor);
};
```

### GowlBarProvider

Provide a custom status bar.

```c
struct _GowlBarProviderInterface {
    GTypeInterface parent_iface;
    gboolean (*render_bar)     (GowlBarProvider *self,
                                gpointer monitor, gpointer surface);
    gint     (*get_bar_height) (GowlBarProvider *self);
};
```

### GowlScratchpadHandler

Manage scratchpad windows.

```c
struct _GowlScratchpadHandlerInterface {
    GTypeInterface parent_iface;
    gboolean (*toggle_scratchpad)  (GowlScratchpadHandler *self,
                                    const gchar *name);
    gboolean (*add_to_scratchpad)  (GowlScratchpadHandler *self,
                                    gpointer client, const gchar *name);
};
```

### GowlSwallowHandler

Handle window swallowing (terminal absorbs child window).

```c
struct _GowlSwallowHandlerInterface {
    GTypeInterface parent_iface;
    gboolean (*should_swallow) (GowlSwallowHandler *self,
                                gpointer parent, gpointer child);
    gboolean (*swallow)        (GowlSwallowHandler *self,
                                gpointer parent, gpointer child);
    void     (*unswallow)      (GowlSwallowHandler *self,
                                gpointer parent);
};
```

### GowlStickyHandler

Manage sticky windows (visible on all tags).

```c
struct _GowlStickyHandlerInterface {
    GTypeInterface parent_iface;
    gboolean (*is_sticky)  (GowlStickyHandler *self, gpointer client);
    gboolean (*set_sticky) (GowlStickyHandler *self,
                            gpointer client, gboolean sticky);
};
```

### GowlCursorProvider

Provide custom cursor images.

```c
struct _GowlCursorProviderInterface {
    GTypeInterface parent_iface;
    gpointer (*get_cursor_image) (GowlCursorProvider *self, gint mode);
    void     (*set_cursor)       (GowlCursorProvider *self, gpointer image);
};
```

## Creating a Module: Step by Step

### 1. Define the Module Type

```c
/*
 * my-module.c - Example Gowl Module
 *
 * Compile: gcc $(pkg-config --cflags --libs glib-2.0 gobject-2.0 gmodule-2.0 gowl)
 *          -shared -fPIC -o my-module.so my-module.c
 */

#include <gowl/gowl.h>

/* --- Type declaration --- */

#define MY_TYPE_MODULE (my_module_get_type())

G_DECLARE_FINAL_TYPE(MyModule, my_module, MY, MODULE, GowlModule)

struct _MyModule {
    GowlModule parent_instance;

    /* module-specific state */
    gint example_value;
};

/* Implement GowlStartupHandler interface */
static void
my_module_on_startup(
    GowlStartupHandler *handler,
    gpointer            compositor
){
    MyModule *self;

    self = MY_MODULE(handler);

    g_message("MyModule startup! example_value=%d", self->example_value);
}

static void
my_module_startup_handler_init(GowlStartupHandlerInterface *iface)
{
    iface->on_startup = my_module_on_startup;
}

/* Implement GowlKeybindHandler interface */
static gboolean
my_module_handle_key(
    GowlKeybindHandler *handler,
    guint               modifiers,
    guint               keysym,
    gboolean            pressed
){
    /* Example: intercept a specific keybind */
    if (pressed &&
        (modifiers & GOWL_KEY_MOD_LOGO) &&
        keysym == XKB_KEY_F12) {
        g_message("MyModule: F12 pressed!");
        return TRUE; /* consumed */
    }

    return FALSE; /* pass to next handler */
}

static void
my_module_keybind_handler_init(GowlKeybindHandlerInterface *iface)
{
    iface->handle_key = my_module_handle_key;
}
```

### 2. Register Interfaces and Define the GType

```c
/* Register the type with both interfaces */
G_DEFINE_FINAL_TYPE_WITH_CODE(MyModule, my_module, GOWL_TYPE_MODULE,
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
                          my_module_startup_handler_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER,
                          my_module_keybind_handler_init)
)
```

### 3. Override Virtual Methods

```c
static gboolean
my_module_activate(GowlModule *module)
{
    MyModule *self;

    self = MY_MODULE(module);
    self->example_value = 42;

    g_message("MyModule activated");
    return TRUE;
}

static void
my_module_deactivate(GowlModule *module)
{
    g_message("MyModule deactivated");
}

static const gchar *
my_module_get_name(GowlModule *module)
{
    return "my-module";
}

static const gchar *
my_module_get_description(GowlModule *module)
{
    return "An example gowl module";
}

static const gchar *
my_module_get_version(GowlModule *module)
{
    return "0.1.0";
}

static void
my_module_configure(
    GowlModule *module,
    gpointer    config
){
    /* Receive configuration from YAML modules section */
    g_message("MyModule configured");
}
```

### 4. Implement Class and Instance Init

```c
static void
my_module_class_init(MyModuleClass *klass)
{
    GowlModuleClass *module_class;

    module_class = GOWL_MODULE_CLASS(klass);

    module_class->activate        = my_module_activate;
    module_class->deactivate      = my_module_deactivate;
    module_class->get_name        = my_module_get_name;
    module_class->get_description = my_module_get_description;
    module_class->get_version     = my_module_get_version;
    module_class->configure       = my_module_configure;
}

static void
my_module_init(MyModule *self)
{
    self->example_value = 0;
}
```

### 5. Export the Registration Function

```c
G_MODULE_EXPORT GType
gowl_module_register(void)
{
    return MY_TYPE_MODULE;
}
```

## Building a Module

### Manual Build

```bash
gcc -std=gnu89 -shared -fPIC \
    $(pkg-config --cflags --libs glib-2.0 gobject-2.0 gmodule-2.0) \
    -I/path/to/gowl/src \
    -L/path/to/gowl/build/release -lgowl \
    -o my-module.so my-module.c
```

### Using the Build System

Place your module source in `modules/my-module/`:

```
modules/
  my-module/
    Makefile
    my-module.c
```

The top-level `make modules` target will build all modules under `modules/`. Each module directory should have a `Makefile` that accepts `OUTDIR`, `LIBDIR`, `CFLAGS`, and `LDFLAGS` variables.

A minimal module `Makefile`:

```makefile
NAME := my-module

SRCS := $(wildcard *.c)

all:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OUTDIR)/$(NAME).so $(SRCS) -L$(LIBDIR) -lgowl
```

## Installing a Module

Modules are loaded from:

1. `$(PREFIX)/lib/gowl/modules/` (system-wide, installed by `make install`)
2. `~/.local/lib/gowl/modules/` (user-local)

Copy your `.so` to either location:

```bash
cp my-module.so ~/.local/lib/gowl/modules/
```

## Module Configuration

If your module needs configuration from the YAML config, it receives it through the `configure()` virtual method. The YAML `modules` section is parsed and the corresponding subsection is passed to each module by name:

```yaml
modules:
  my-module:
    enabled: true
    example-value: 42
```

The `configure()` method receives a `gpointer` to the config data structure, which it can cast and process as needed.

## Bundled Modules

Gowl ships with several modules in the `modules/` directory:

| Module | Description |
|--------|-------------|
| `autostart` | Runs autostart commands from config |
| `vanitygaps` | Adds configurable inner/outer gaps to layouts |
| `pertag` | Per-tag layout and mfact settings |
| `scratchpad` | Named scratchpad windows |
| `swallow` | Terminal window swallowing |
| `movestack` | Move clients within the stack |
| `fibonacci` | Fibonacci spiral layout |
| `centeredmaster` | Centered master layout |
| `ipc` | IPC socket command handler |
| `copyhighlight` | Sync primary selection to clipboard on text highlight |
| `alpha` | Per-client window opacity with focus dimming |
| `bar` | Compositor status bar with title and system info |

### Bar Module

The bar module renders a compositor-level status bar on the TOP scene layer. It shows the focused client title (left) and 25 configurable system widgets (right). The background is semi-transparent so the wallpaper shows through.

**Interfaces:** `GowlBarProvider`, `GowlStartupHandler`, `GowlShutdownHandler`

**Configuration keys:**

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `height` | int | 28 | Bar height in pixels |
| `bg-color` | hex | #1e1e2ecc | Background color (RRGGBBAA for transparency) |
| `fg-color` | hex | #cdd6f4 | Text color |
| `font` | string | monospace | Font family |
| `font-size` | float | 13 | Font size |
| `widgets` | string | cpu memory disk battery clock | Space-separated widget list |
| `TYPE-color` | hex | (fg-color) | Per-widget color (e.g. `cpu-color`) |
| `cmd-interval` | int | 10 | Seconds between `cmd` widget re-runs |
| `widget-data-KEY` | string | -- | Push data for external widgets (e.g. `widget-data-todo`) |

**Available widgets (25 total):**

| Widget | Syntax | Data Source | Cache |
|--------|--------|-------------|-------|
| `cpu` | `cpu` | /proc/stat delta | 2s |
| `load` | `load` | /proc/loadavg | 5s |
| `memory` | `memory` or `mem` | /proc/meminfo | 5s |
| `swap` | `swap` | /proc/meminfo | 5s |
| `temp` | `temp` | /sys/class/hwmon/ | 10s |
| `gpu` | `gpu` | sysfs (AMD amdgpu) | 5s |
| `disk` | `disk` or `disk:/path` | statvfs | 30s |
| `io` | `io` or `io:DEVICE` | /proc/diskstats | 2s |
| `battery` | `battery` or `bat` | /sys/class/power_supply | 60s |
| `net` | `net` or `net:IFACE` | /proc/net/dev delta | 2s |
| `volume` | `volume` or `vol` | wpctl (PipeWire) | 2s |
| `media` | `media` | playerctl (MPRIS) | 2s |
| `wifi` | `wifi` | /proc/net/wireless | 10s |
| `ip` | `ip` or `ip:IFACE` | getifaddrs() | 30s |
| `vpn` | `vpn` | /sys/class/net/tun0\|wg0 | 10s |
| `git` | `git` | .git/HEAD + git status | 5s |
| `todo` | `todo` | widget-data push | -- |
| `podman` | `podman` or `pod` | podman ps | 30s |
| `cmd` | `cmd:COMMAND` | subprocess | 10s |
| `weather` | `weather` or `weather:LOC` | wttr.in | 15min |
| `uptime` | `uptime` | /proc/uptime | 60s |
| `host` | `host` or `hostname` | g_get_host_name() | once |
| `user` | `user` | g_get_user_name() | once |
| `keymap` | `keymap` | xkb_state | 2s |
| `clock` | `clock` or `time` | localtime() | 2s |

**CMD widget ANSI color support:**

The `cmd` widget parses ANSI escape sequences in command output and applies foreground colors via PangoAttrList. Colors apply only to ASCII printable characters (0x20-0x7E); Unicode characters (emoji, CJK) render in the widget's base color. Supported: standard 8 colors (30-37), bright 8 (90-97), 256-color (38;5;N), truecolor (38;2;R;G;B), and reset (0).

**YAML example:**

```yaml
modules:
  bar:
    enabled: true
    height: 28
    bg-color: "#1e1e2ecc"
    fg-color: "#cdd6f4"
    font: "Hack Nerd Font Mono"
    font-size: 13
    widgets: "cpu load memory swap temp disk:/ net volume git clock"
    cpu-color: "#a6e3a1"
    load-color: "#fab387"
    net-color: "#89b4fa"
    git-color: "#cba6f7"
```

**Implementation notes:**

- Renders via cairo+pango to a custom `wlr_buffer` (ARGB8888)
- Creates per-monitor `wlr_scene_buffer` nodes on the TOP layer
- Connects to compositor `focus-changed`, `client-added`, `client-removed` signals for reactive updates
- 2-second tick timer drives system data refresh and bar redraw
- The compositor's `arrangelayers` subtracts bar height from usable area so tiled clients don't overlap
- Data readers use time-based caching (2s-15min depending on widget) to minimize syscall overhead
- Delta-based widgets (CPU, net, IO) track previous values for rate calculation
- Subprocess widgets (cmd, volume, media, podman, weather) use `g_spawn_command_line_sync()`
- Git widget reads focused client's CWD via `gowl_client_get_process_info()` → `/proc/PID/cwd`

### Alpha Module

The alpha module controls per-client window opacity. When the focused client changes, the previously focused client fades to `unfocused-alpha` and the newly focused client is set to `focused-alpha`.

**Interfaces:** `GowlStartupHandler`, `GowlShutdownHandler`

**Configuration keys:**

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `focused-alpha` | float | 1.0 | Opacity for the focused window (0.0--1.0) |
| `unfocused-alpha` | float | 0.8 | Opacity for unfocused windows (0.0--1.0) |

**YAML example:**

```yaml
modules:
  alpha:
    enabled: true
    focused-alpha: 1.0
    unfocused-alpha: 0.7
```

**Implementation notes:**

- Uses `gowl_client_set_alpha()` which walks the client's scene tree via `wlr_scene_node_for_each_buffer()` and calls `wlr_scene_buffer_set_opacity()` on each buffer.
- Connects to the compositor's `focus-changed` signal to automatically apply alpha on focus changes.
- On deactivation, resets all clients to fully opaque (alpha 1.0).
- Applying configuration while running immediately re-applies alpha to all existing clients.

## Tips

- Set your module's `priority` property to control dispatch order relative to other modules. Lower values are dispatched first.
- Always check `gowl_module_get_is_active()` before performing module operations in signal handlers.
- Use `g_autoptr()` and `g_steal_pointer()` for memory management.
- Return `TRUE` from `handle_key` / `handle_button` only if you actually consumed the event; returning `TRUE` prevents other handlers from seeing it.
- The module `.so` stays loaded for the lifetime of the compositor. Do not call `g_module_close()` on yourself.
