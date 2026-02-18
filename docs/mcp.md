# MCP Server Module

The MCP (Model Context Protocol) module exposes compositor state and controls to AI assistants like Claude Code. It implements the MCP protocol using [mcp-glib](https://gitlab.com/copyleftgames/mcp-glib), enabling AI tools to query windows, manage tags, adjust layouts, and more.

## Quick Start

1. Enable the module in your config (`~/.config/gowl/config.yaml`):

```yaml
modules:
  mcp:
    enabled: true
```

2. Configure Claude Code to use the `gowl-mcp` relay binary. Add to your Claude Code MCP settings:

```json
{
  "mcpServers": {
    "gowl": {
      "command": "/usr/local/bin/gowl-mcp"
    }
  }
}
```

3. Restart gowl. The MCP module starts a Unix socket at `$XDG_RUNTIME_DIR/gowl-mcp.sock`.

## Architecture

```
┌────────────────────────────────────┐
│         Gowl Compositor            │
│                                    │
│  ┌──────────────────────────────┐  │
│  │     MCP Module (mcp.so)      │  │
│  │                              │  │
│  │  MCP Thread (GMainLoop)      │  │
│  │  ├─ McpServer (HTTP+SSE)     │  │
│  │  ├─ McpServer (Socket)       │  │
│  │  └─ Tool handlers ──┐       │  │
│  │                      │       │  │
│  │  Compositor Thread ◄─┘       │  │
│  │  (wl_event_loop dispatch)    │  │
│  └──────────────────────────────┘  │
│           ▲              ▲         │
└───────────┼──────────────┼─────────┘
            │              │
HTTP+SSE clients     Unix socket
                          │
                    ┌──────┴──────┐
                    │  gowl-mcp   │
                    │  stdin↔sock │
                    └─────────────┘
                     ▲           │
                stdin│           │stdout
                     │           ▼
                  AI Client (Claude Code)
```

### Threading Model

The compositor runs on the Wayland event loop. The MCP module spawns a dedicated `GThread` with its own `GMainLoop` for MCP server I/O. Tool calls are dispatched to the compositor thread via `eventfd` + mutex/cond signaling, ensuring all compositor state access is thread-safe.

## Transports

### Unix Socket (stdio relay) -- Default

The module creates a listening Unix socket at `$XDG_RUNTIME_DIR/gowl-mcp.sock`. The `gowl-mcp` binary bridges stdin/stdout to this socket using NDJSON framing, which is what MCP clients like Claude Code expect.

Override the socket path:
- Environment variable: `GOWL_MCP_SOCKET=/path/to/socket gowl-mcp`
- CLI flag: `gowl-mcp --socket /path/to/socket`

### HTTP+SSE

Direct HTTP connections with Server-Sent Events. Enable with:

```yaml
modules:
  mcp:
    enabled: true
    transport-http: true
    http-host: "127.0.0.1"
    http-port: 8716
    # http-auth-token: "secret"
```

When `http-auth-token` is set, clients must include `Authorization: Bearer <token>` in requests.

## Configuration

All settings go under `modules: mcp:` in your YAML config.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | bool | `false` | Enable the MCP module |
| `transport-stdio` | bool | `true` | Enable Unix socket for stdio relay |
| `transport-http` | bool | `false` | Enable HTTP+SSE transport |
| `http-host` | string | `127.0.0.1` | HTTP bind address |
| `http-port` | int | `8716` | HTTP listen port |
| `http-auth-token` | string | (empty) | Bearer token for HTTP auth |
| `instructions` | string | (built-in) | Custom instructions for AI clients |
| `tools` | string | (all) | Newline-delimited allowlist of tool names |

### Tool Allowlist

By default, all tools are enabled. To restrict available tools, set the `tools` key to a newline-delimited list:

```yaml
modules:
  mcp:
    enabled: true
    tools: |
      list_clients
      list_monitors
      describe_desktop
      find_window
      get_focused_client
```

Only listed tools will be registered. Omit the `tools` key entirely to enable all tools.

## Tools Reference

### Query Tools (read-only)

| Tool | Description |
|------|-------------|
| `list_clients` | List all windows with app_id, title, tags, geometry, state, monitor, PID |
| `list_monitors` | List all outputs with name, geometry, active tags, layout, mfact, nmaster |
| `get_tag_state` | Per-monitor tag info: which tags are active and client count per tag |
| `get_config` | Current runtime config (borders, layout defaults, keyboard, terminal) |
| `list_keybinds` | All active keybinds with combo string, action nick, and argument |
| `list_modules` | Loaded modules with name, description, version, priority, active state |
| `get_focused_client` | Details of the currently focused window, or null if none |

### Window Management

| Tool | Parameters | Description |
|------|-----------|-------------|
| `focus_client` | `id`, `app_id`, or `title` | Focus a window by ID or glob pattern |
| `close_client` | `id`, `app_id`, or `title` | Send close request to a window |
| `move_client_to_tag` | client selector + `tags` (bitmask) | Move a client to tag(s) |
| `set_client_state` | client selector + `floating`, `fullscreen`, `urgent` | Set window state |
| `resize_client` | client selector + `width`, `height` | Resize a floating window |
| `move_client` | client selector + `x`, `y` | Move a floating window |
| `swap_clients` | `id_a`, `id_b` | Swap two clients in tiling stack |
| `zoom_client` | client selector | Promote to master area |

Client selector: provide `id` (numeric), `app_id` (glob pattern), or `title` (glob pattern). ID is checked first, then app_id, then title.

### Tag/Workspace Management

| Tool | Parameters | Description |
|------|-----------|-------------|
| `view_tag` | `tags` (bitmask), `monitor` (optional) | Switch to tag(s) on a monitor |
| `toggle_tag_view` | `tag` (0-based index), `monitor` (optional) | Toggle a tag's visibility |
| `set_client_tags` | `id`, `tags` (bitmask) | Assign tag(s) to a client |

Tags use bitmasks: tag 1 = `1`, tag 2 = `2`, tag 3 = `4`, tags 1+2 = `3`, etc.

### Layout

| Tool | Parameters | Description |
|------|-----------|-------------|
| `set_mfact` | `mfact` (0.05-0.95), `monitor` (optional) | Set master area ratio |
| `set_nmaster` | `nmaster` (>= 0), `monitor` (optional) | Set master window count |

### Config & Control

| Tool | Parameters | Description |
|------|-----------|-------------|
| `add_rule` | `app_id`, `title`, `tags`, `floating`, `monitor` | Add a window rule |
| `add_keybind` | `modifiers`, `key`, `action`, `arg` | Bind key combo to action |
| `spawn` | `command` | Launch external process |
| `compositor_quit` | (none) | Shut down the compositor |

### Synthetic Input

| Tool | Parameters | Description |
|------|-----------|-------------|
| `send_key` | `key` (XKB name) | Send key press+release to focused window |
| `send_text` | `text` | Type a string character-by-character |
| `send_mouse` | `button`, `action` (optional) | Send mouse button event (click/press/release) |
| `send_mouse_move` | `x`, `y` | Move cursor to absolute layout coordinates |
| `send_scroll` | `direction`, `amount` (optional) | Send scroll event (up/down/left/right) |

Key names use XKB names: `Return`, `space`, `a`, `Escape`, `Tab`, `BackSpace`, `F1`, etc.
Button names: `left`, `right`, `middle`, or numeric codes.

### Screenshot

| Tool | Parameters | Description |
|------|-----------|-------------|
| `screenshot_client` | `id` | Capture a client window (returns base64 PNG) |
| `screenshot_monitor` | `monitor` (optional) | Capture entire output (returns base64 PNG) |
| `screenshot_region` | `x`, `y`, `width`, `height`, `monitor` (optional) | Capture rectangular region (returns base64 PNG) |

Screenshots are returned as base64-encoded PNG images via the MCP image content type.

### Process Introspection

| Tool | Parameters | Description |
|------|-----------|-------------|
| `get_client_process_info` | `id` | PID, command line, and working directory |
| `signal_client` | `id`, `signal` (number) | Send POSIX signal (e.g. 15=SIGTERM, 9=SIGKILL) |

### Compound Tools

| Tool | Parameters | Description |
|------|-----------|-------------|
| `describe_desktop` | (none) | Full desktop snapshot: monitors, tags, clients, focus |
| `find_window` | `app_id` and/or `title` (glob) | Search for matching windows |

## gowl-mcp Relay Binary

The `gowl-mcp` binary is a small stdio-to-socket relay. It connects to the module's Unix domain socket and bridges NDJSON lines between stdin/stdout and the socket.

```
Usage: gowl-mcp [--socket PATH]

Options:
  --socket PATH   Unix socket path (default: $XDG_RUNTIME_DIR/gowl-mcp.sock)
  -h, --help      Show help
  --license       Show license

Environment:
  GOWL_MCP_SOCKET  Override socket path
```

## Build Requirements

The MCP module is opt-in and requires `MCP=1` on the make command line:

```bash
# Install MCP dependencies
make MCP=1 install-deps

# Build with MCP support
make MCP=1
make MCP=1 DEBUG=1    # debug build
```

The additional dependencies beyond the base compositor are:

```bash
sudo dnf install libsoup3-devel libdex-devel libpng-devel
```

When `MCP=1` is set and the dependencies are found, the build produces:
- `mcp.so` -- the compositor module (in `build/<type>/modules/`)
- `gowl-mcp` -- the stdio relay binary (in `build/<type>/`)

## Files

```
modules/mcp/
  Makefile                      # Module build
  gowl-module-mcp.h             # Module type and state struct
  gowl-module-mcp.c             # Entry point, lifecycle, threading, config
  gowl-mcp-dispatch.h           # Thread dispatch types
  gowl-mcp-dispatch.c           # eventfd-based compositor dispatch
  gowl-mcp-tools.h              # Tool registration declarations
  gowl-mcp-tools.c              # Master registration function
  gowl-mcp-tools-query.c        # 7 read-only query tools
  gowl-mcp-tools-window.c       # 8 window management tools
  gowl-mcp-tools-tag.c          # 3 tag management tools
  gowl-mcp-tools-layout.c       # 2 layout tools
  gowl-mcp-tools-config.c       # 4 config/control tools
  gowl-mcp-tools-input.c        # 5 synthetic input tools
  gowl-mcp-tools-screenshot.c   # 3 screenshot tools
  gowl-mcp-tools-process.c      # 2 process introspection tools
  gowl-mcp-tools-compound.c     # 2 compound/aggregate tools

tools/gowl-mcp/
  Makefile                      # Relay binary build
  gowl-mcp.c                    # stdio-to-socket relay
```
