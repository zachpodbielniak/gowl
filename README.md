# gowl

<p align="center">
  <img src="data/logo.png" alt="gowl logo" width="200">
</p>

A GObject-based Wayland compositor built on wlroots 0.19. Gowl takes inspiration from dwl and dwm, reimagining their tiling window management philosophy with a full GObject architecture. Rather than a direct port, gowl provides a modular, introspectable compositor with a plugin architecture based on GModule and GObject interfaces.

## Features

- **GObject type system** -- Every compositor object is a proper GObject with properties, signals, and introspection support.
- **Dual configuration** -- YAML config for declarative settings, C config for full programmatic control. Both can be used simultaneously.
- **Module/plugin system** -- Extend the compositor via shared object plugins that subclass `GowlModule` and implement any combination of 18 GObject interfaces.
- **18 hook interfaces** -- Layout, keybind, mouse, client decorator, focus policy, IPC, tag management, gap provider, bar provider, scratchpad, swallow, sticky, cursor, startup/shutdown, and more.
- **Priority-based dispatch** -- Module events are dispatched in priority order with both consumable and broadcast semantics.
- **wlroots 0.19** -- Built on the latest stable wlroots for full Wayland protocol support.
- **XWayland support** -- Optional X11 client compatibility (compile-time toggle).
- **GObject Introspection** -- Optional GIR/typelib generation for language bindings.
- **dwm-style tiling** -- Master-stack tile, monocle, and float layouts with tag-based workspaces.
- **Session lock** -- ext-session-lock-v1 support.
- **Layer shell** -- wlr-layer-shell for panels, bars, and overlays.
- **Built-in config compiler** -- Auto-compiles C config files at startup.

## Quick Build

### Dependencies (Fedora)

```bash
sudo dnf install -y \
    gcc make pkg-config \
    glib2-devel gobject-introspection-devel json-glib-devel \
    wayland-devel wayland-protocols-devel wlroots-devel \
    libxkbcommon-devel libinput-devel libyaml-devel \
    libxcb-devel xcb-util-wm-devel
```

Or: `make install-deps`

### Build

```bash
make
```

Debug build: `make DEBUG=1`

Run tests: `make test`

Install: `make install`

## Usage

```bash
# Run the compositor
gowl

# Run with debug logging
gowl --debug

# Run with a custom config
gowl --config ~/.config/gowl/config.yaml

# Run with a startup command
gowl -s waybar

# Generate a default YAML config
gowl --generate-yaml-config > ~/.config/gowl/config.yaml

# Generate a default C config
gowl --generate-c-config > ~/.config/gowl/config.c
```

## Configuration

Gowl supports two configuration methods:

### YAML Config

Declarative configuration in `~/.config/gowl/config.yaml`. Covers appearance, layout, keybinds, rules, autostart, monitor settings, and module configuration.

Generate a default config:

```bash
mkdir -p ~/.config/gowl
gowl --generate-yaml-config > ~/.config/gowl/config.yaml
```

### C Config

Full programmatic configuration in `~/.config/gowl/config.c`. Auto-compiled to a shared object at startup. Has access to the full gowl C API via `g_object_set()`, `gowl_config_add_keybind()`, and `gowl_config_add_rule()`.

Generate a default config:

```bash
gowl --generate-c-config > ~/.config/gowl/config.c
```

C config runs after YAML config, so it can override any YAML values.

See [docs/configuration.md](docs/configuration.md) for the full configuration reference.

## Module System

Modules are `.so` plugins that subclass `GowlModule` and implement one or more of 18 GObject interfaces to hook into compositor events. Each module exports a `gowl_module_register()` function that returns its GType.

Bundled modules include: autostart, vanitygaps, pertag, scratchpad, swallow, movestack, fibonacci, centeredmaster, and IPC.

Install modules to `~/.local/lib/gowl/modules/` or `$(PREFIX)/lib/gowl/modules/`.

See [docs/modules.md](docs/modules.md) for the module development guide.

## Documentation

- [docs/architecture.md](docs/architecture.md) -- Type hierarchy, module system, interface dispatch, config system.
- [docs/building.md](docs/building.md) -- Dependencies, build commands, build options.
- [docs/configuration.md](docs/configuration.md) -- YAML and C configuration reference.
- [docs/modules.md](docs/modules.md) -- Module development guide with examples.

## License

Copyright (C) 2026 Zach Podbielniak

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See [LICENSE](LICENSE) for the full license text.
