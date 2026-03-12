# Gowl Configuration Guide

## Overview

Gowl supports two configuration methods that work together:

1. **YAML configuration** -- Declarative config file for all standard settings.
2. **C configuration** -- Compiled config file for full programmatic control.

Both can be used simultaneously. The C config runs after the YAML config, so C values override YAML values.

### Precedence (lowest to highest)

1. Built-in defaults
2. YAML configuration file
3. C configuration module
4. Command-line arguments (`--debug`, etc.)

## YAML Configuration

### Search Paths

Gowl searches for `config.yaml` in this order (first match wins):

1. `./data/config.yaml` -- Project directory (for development)
2. `~/.config/gowl/config.yaml` -- User config (recommended)
3. `/etc/gowl/config.yaml` -- System-wide config
4. `/usr/local/gowl/config.yaml` -- Installation default

Override the search with `--config`:

```bash
gowl --config /path/to/my-config.yaml
```

### Generating a Default Config

```bash
gowl --generate-yaml-config > ~/.config/gowl/config.yaml
```

This writes the built-in default configuration to stdout.

### Config Sections

#### compositor

General compositor settings.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `log-level` | string | `"warning"` | Log verbosity: `"debug"`, `"info"`, `"message"`, `"warning"`, `"error"` |
| `repeat-rate` | int | `25` | Keyboard repeat rate (keys per second) |
| `repeat-delay` | int | `600` | Keyboard repeat delay (milliseconds before repeat starts) |
| `terminal` | string | `"gst"` | Terminal emulator command |
| `menu` | string | `"bemenu-run"` | Application launcher command |
| `sloppyfocus` | bool | `true` | Enable sloppy (focus-follows-mouse) focus |

#### appearance

Visual appearance settings.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `border-width` | int | `2` | Window border width in pixels |
| `border-color-focus` | string | `"#005577"` | Hex color for focused window border |
| `border-color-unfocus` | string | `"#444444"` | Hex color for unfocused window border |
| `border-color-urgent` | string | `"#ff0000"` | Hex color for urgent window border |

#### layout

Layout engine settings.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `default` | string | `"tile"` | Default layout: `"tile"`, `"float"`, `"monocle"` |
| `mfact` | float | `0.55` | Master area factor (0.05 - 0.95) |
| `nmaster` | int | `1` | Number of windows in the master area |

#### tags

Tag (workspace) settings.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `count` | int | `9` | Number of tags (1 - 31) |

### Keybinds

Keybinds are defined in the `keybinds` section as a mapping of key strings to action objects.

#### Key String Format

```
Modifier+Modifier+Key
```

Modifier names (case-insensitive):
- `Super` or `Logo` -- Super/Windows key
- `Shift` -- Shift key
- `Ctrl` or `Control` -- Control key
- `Alt` or `Mod1` -- Alt key
- `Mod2` -- Num Lock (typically)
- `Mod3` -- Mod3
- `Mod5` -- Mod5

The last token in the `+`-separated string is the key name, resolved via `xkb_keysym_from_name()`. Common examples: `Return`, `space`, `Tab`, `a`-`z`, `0`-`9`, `F1`-`F12`, `comma`, `period`, `bracketleft`, `bracketright`.

#### Action Object Format

```yaml
"Super+Return": { action: spawn, arg: "gst" }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `action` | string | yes | The action to perform (see action list below) |
| `arg` | string | no | Argument for the action |

#### Actions

| Action | Argument | Description |
|--------|----------|-------------|
| `spawn` | command string | Spawn an external process |
| `kill_client` | -- | Close the focused client |
| `toggle_float` | -- | Toggle floating state on focused client |
| `toggle_fullscreen` | -- | Toggle fullscreen on focused client |
| `focus_stack` | `"+1"` or `"-1"` | Move focus forward or backward in the stack |
| `focus_monitor` | `"+1"` or `"-1"` | Move focus to next or previous monitor |
| `tag_view` | `"0"` - `"9"` | View a tag (0 = all tags) |
| `tag_set` | `"0"` - `"9"` | Assign tag to focused client (0 = all tags) |
| `tag_toggle_view` | `"1"` - `"9"` | Toggle visibility of a tag |
| `tag_toggle` | `"1"` - `"9"` | Toggle a tag on the focused client |
| `move_to_monitor` | `"+1"` or `"-1"` | Move focused client to next/prev monitor |
| `set_mfact` | `"+0.05"` or `"-0.05"` | Adjust the master area factor |
| `inc_nmaster` | `"+1"` or `"-1"` | Increment/decrement the master count |
| `set_layout` | `"tile"`, `"float"`, `"monocle"` | Set the layout |
| `cycle_layout` | -- | Cycle through available layouts |
| `zoom` | -- | Promote focused client to master |
| `quit` | -- | Quit the compositor |
| `reload_config` | -- | Reload configuration |
| `ipc_command` | IPC command string | Execute an IPC command |
| `custom` | varies | Custom action handled by a module |

#### Example

```yaml
keybinds:
  "Super+Return": { action: spawn, arg: "gst" }
  "Super+p": { action: spawn, arg: "bemenu-run" }
  "Super+Shift+c": { action: kill_client }
  "Super+j": { action: focus_stack, arg: "+1" }
  "Super+k": { action: focus_stack, arg: "-1" }
  "Super+h": { action: set_mfact, arg: "-0.05" }
  "Super+l": { action: set_mfact, arg: "+0.05" }
  "Super+1": { action: tag_view, arg: "1" }
  "Super+Shift+1": { action: tag_set, arg: "1" }
  "Super+Shift+q": { action: quit }
```

### Window Rules

Rules match clients by `app_id` and/or `title` and apply settings.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `app_id` | string | `""` | Wayland app_id to match (empty = match any) |
| `title` | string | `""` | Window title pattern (empty = match any) |
| `tags` | int | `0` | Tag bitmask to assign (0 = no override) |
| `floating` | bool | `false` | Whether the client should float |
| `monitor` | int | `-1` | Monitor index (-1 = default) |

#### Example

```yaml
rules:
  - app_id: "firefox"
    tags: 2
    floating: false
    monitor: -1

  - app_id: "pavucontrol"
    floating: true

  - app_id: "mpv"
    floating: true
    tags: 0
    monitor: -1
```

### Autostart

Commands to run once at compositor startup:

```yaml
autostart:
  - "waybar"
  - "mako"
  - "swayidle -w"
```

### Monitor Configuration

Per-output settings keyed by output name:

```yaml
monitors:
  eDP-1:
    width: 1920
    height: 1080
    refresh: 60.0
    x: 0
    y: 0
    scale: 1.0
  HDMI-A-1:
    width: 2560
    height: 1440
    refresh: 144.0
    x: 1920
    y: 0
    scale: 1.0
```

### Module Configuration

Per-module settings passed to the module's `configure()` method:

```yaml
modules:
  vanitygaps:
    enabled: true
    inner-gap: 5
    outer-gap: 10
  pertag:
    enabled: true
  autostart:
    enabled: true
```

## C Configuration

### Overview

The C config system allows full programmatic control of the compositor using the gowl C API. The config source is a standard C file that gets compiled to a shared object (`.so`) at startup by `GowlConfigCompiler`.

### Search Paths

The compositor searches for `config.c` in:

1. `~/.config/gowl/config.c` (user config)
2. `/etc/gowl/config.c` (system-wide config)

Override with `--c-config`:

```bash
gowl --c-config /path/to/config.c
```

Disable C config entirely:

```bash
gowl --no-c-config
```

### Generating a Default C Config

```bash
gowl --generate-c-config > ~/.config/gowl/config.c
```

### How It Works

1. The compositor finds `config.c` in the search paths.
2. `GowlConfigCompiler` reads the file and scans for an optional `#define CRISPY_PARAMS` to extract extra compiler flags.
3. The file is compiled via the crispy library with SHA256 content-hash caching.
4. The compiled `.so` is cached in `$XDG_CACHE_HOME/gowl/` (filename derived from content hash).
5. The `.so` is opened via `g_module_open()` and the `gowl_config_init` symbol is resolved and called.
6. After the compositor is fully started, the optional `gowl_config_ready` symbol is looked up and called (if present).
7. If compilation fails, the compositor logs a warning and continues with YAML/default config.

### CRISPY_PARAMS

Optional define to pass extra compiler flags. Shell expansion is supported:

```c
#define CRISPY_PARAMS ""
```

This is useful for specifying additional include paths or libraries:

```c
#define CRISPY_PARAMS "-I/opt/custom/include -lm"
```

Shell expansion example (runs at compile time):

```c
#define CRISPY_PARAMS "$(pkg-config --cflags json-glib-1.0)"
```

If omitted, only the default pkg-config flags are used.

### gowl_config_init Entry Point

The `.so` must export a single function:

```c
G_MODULE_EXPORT gboolean
gowl_config_init(void)
{
    /* configuration code here */
    return TRUE;
}
```

This function has access to two extern symbols provided by the compositor:

```c
extern GowlCompositor *gowl_compositor;
extern GowlConfig     *gowl_config;
```

#### Setting Properties

Use `g_object_set()` on `gowl_config`:

```c
g_object_set(gowl_config,
    "border-width", 3,
    "mfact", 0.60,
    "terminal", "kitty",
    NULL);
```

#### Adding Keybinds

Use `gowl_config_add_keybind()`:

```c
#include <xkbcommon/xkbcommon.h>

gowl_config_add_keybind(gowl_config,
    GOWL_KEY_MOD_LOGO, XKB_KEY_Return,
    GOWL_ACTION_SPAWN, "gst");

gowl_config_add_keybind(gowl_config,
    GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT, XKB_KEY_c,
    GOWL_ACTION_KILL_CLIENT, NULL);
```

#### Adding Rules

Use `gowl_config_add_rule()`:

```c
gowl_config_add_rule(gowl_config,
    "firefox", NULL,    /* app_id, title */
    GOWL_TAGMASK(1),   /* tag 2 (0-indexed) */
    FALSE,              /* floating */
    -1);                /* monitor */
```

### Return Value

Return `TRUE` on success. Return `FALSE` to signal failure; the compositor will log a warning and fall back to the YAML/default configuration.

### gowl_config_ready Entry Point (Optional)

The `.so` may optionally export a second function:

```c
G_MODULE_EXPORT void
gowl_config_ready(void)
{
    /* called after compositor is fully started */
}
```

This function is called **once** after the compositor is fully started and the Wayland display is ready to accept clients. It has access to the same extern symbols (`gowl_compositor`, `gowl_config`) as `gowl_config_init`.

Use `gowl_config_ready` to spawn Wayland clients that need a running compositor — status bars, notification daemons, wallpaper setters, etc. These cannot be spawned from `gowl_config_init` because the Wayland display does not exist yet at that point.

If the symbol is absent, the compositor silently skips the call.

#### Example: Spawning a Status Bar

```c
static void
spawn_gowlbar(void)
{
    g_autofree gchar *path = NULL;
    gchar            *argv[] = { NULL, NULL };
    GError           *error  = NULL;

    path = g_find_program_in_path("gowlbar");
    if (path == NULL)
        return;

    argv[0] = path;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT,
                       NULL, NULL, NULL, &error)) {
        g_printerr("failed to spawn gowlbar: %s\n", error->message);
        g_clear_error(&error);
    }
}

G_MODULE_EXPORT void
gowl_config_ready(void)
{
    spawn_gowlbar();
}
```

## Using Both YAML and C Config

When both configs are present:

1. Built-in defaults are applied.
2. YAML config is loaded (overrides defaults).
3. C config runs (overrides YAML).

A common pattern is to use YAML for declarative settings and C config only for things that require logic (conditional keybinds, computed values, etc.):

```yaml
# config.yaml -- base settings
appearance:
  border-width: 2
  border-color-focus: "#005577"

layout:
  mfact: 0.55
```

```c
/* config.c -- conditional overrides */
G_MODULE_EXPORT gboolean
gowl_config_init(void)
{
    const gchar *hostname;

    hostname = g_get_host_name();

    /* Different mfact on ultrawide monitor setup */
    if (g_str_equal(hostname, "workstation")) {
        g_object_set(gowl_config, "mfact", 0.35, NULL);
    }

    return TRUE;
}
```

## CLI Options

| Option | Description |
|--------|-------------|
| `--version`, `-v` | Show version |
| `--debug`, `-d` | Enable debug logging |
| `--config PATH` | Override YAML config path |
| `--c-config PATH` | Override C config path |
| `--no-c-config` | Skip C config compilation |
| `--startup CMD`, `-s CMD` | Run a command after startup |
| `--generate-yaml-config` | Print default YAML config to stdout |
| `--generate-c-config` | Print default C config template to stdout |
