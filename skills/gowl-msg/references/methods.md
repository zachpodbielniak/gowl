# Wire-protocol methods

`?` marks an optional parameter. `id` is a window (an id or "focused"); `monitor` an output name or "focused". `tags` is a bitmask, `tag` one tag from 1 -- either is accepted wherever tags are taken. "window" / "monitor" in a result is the object shown at the end. `gowl-msg -d METHOD` describes any method of the running build.

## Protocol

| Method | Params | Result |
|---|---|---|
| `rpc.discover` | `method?` | `{protocol, version, server, caller, methods[], events[]}` |
| `rpc.ping` | | `"pong"` |
| `events.subscribe` | `events?` (globs), `add?` | `{subscribed, events[]}` |
| `events.unsubscribe` | | `{subscribed, events[]}` |
| `events.list` | | `[{name, doc}]` |

## Compositor

| Method | Params | Result |
|---|---|---|
| `gowl.version` | | `{version, protocol, protocol_version, wlroots, xwayland}` |
| `gowl.state` | | `{version, focused_monitor, focused_window, locked, keymode, keyboard_layout, monitors[], windows[]}` |
| `gowl.command` | `line` | `{ok, reply, json?}` -- any text command, incl. module commands |
| `gowl.reload` | | `{problems}` |

## Windows

| Method | Params | Result |
|---|---|---|
| `windows.list` | `app_id?` (exact), `title?` (substring), `pid?`, `descendants_of?`, `mine?`, `monitor?`, `tags?`/`tag?`, `visible?`, `floating?`, `embedded?` (default true) | `[window]` |
| `window.get` | `id?` | window |
| `window.focused` | | window or null |
| `window.focus` | `id` | window (switches to its tag/output, restores if minimized) |
| `window.close` | `id` | `{ok}` |
| `window.set_geometry` | `id?`, `x?`, `y?`, `width?`, `height?`, `relative_to?`, `units?` | window (floated) |
| `window.move_by` | `id?`, `dx?`, `dy?`, `dwidth?`, `dheight?` | window (floated) |
| `window.center` | `id?`, `width?`, `height?` | window (floated, centred on the area) |
| `window.set_floating` | `id?`, `value?` | window |
| `window.set_fullscreen` | `id?`, `value?` | window |
| `window.set_sticky` | `id?`, `value?` (pinning also floats) | window |
| `window.set_below` | `id?`, `value?` (floating behind the tiles) | window |
| `window.set_minimized` | `id?`, `value?`, `monitor?`, `tags?`/`tag?` (restore target) | window |
| `window.set_urgent` | `id?`, `value?` | window |
| `window.set_opacity` | `id?`, `opacity` (0-1) | window |
| `window.set_tags` | `id?`, `tags`/`tag` | window |
| `window.send_to_monitor` | `id?`, `monitor`, `tags?`/`tag?` (default: what it shows) | window |
| `window.swap` | `id?`, `with` | `{ok}` |
| `window.promote` | `id?` | window (now master) |
| `window.at` | `x`, `y` | window or null |
| `window.neighbour` | `id?`, `direction` (left/right/up/down) | window or null |
| `window.children` | `id?`, `recursive?` | `[window]` (its dialogs) |
| `window.process` | `id?` | `{pid, ppid, comm, cmdline, cwd}` |
| `window.type_text` | `id?`, `text` | `{sent}` |
| `window.send_key` | `id?`, `key` ("Ctrl+c") | `{ok}` |
| `window.screenshot` | `id?`, `path` (absolute) | `{path, width, height}` |

## Layout

| Method | Params | Result |
|---|---|---|
| `layout.list` | | `[{name, symbol}]` |
| `layout.get` | `monitor?` | `{monitor, name, symbol, mfact, nmaster}` |
| `layout.set` | `monitor?`, `name` | as `layout.get` |
| `layout.set_mfact` | `monitor?`, `value` (0.05-0.95), `relative?` | as `layout.get` |
| `layout.set_nmaster` | `monitor?`, `value`, `relative?` | as `layout.get` |
| `layout.order` | `ids`, `tile?` | `{ok}` -- re-deal these windows' tiling slots in this order |
| `layout.apply` | `windows` (`[{id, x, y, width, height, monitor?}]`), `monitor?`, `relative_to?` (default area), `units?` | `[window]` -- all or nothing |

## Outputs and tags

| Method | Params | Result |
|---|---|---|
| `monitors.list` | | `[monitor]` |
| `monitor.get` | `monitor?` | monitor |
| `monitor.focus` | `monitor?`, `warp?` | monitor |
| `monitor.configure` | `monitor?`, `enabled?`, `scale?`, `transform?` (0-7), `x?`, `y?`, `power?`, `hdr?` | monitor |
| `monitor.screenshot` | `monitor?`, `path` | `{path, width, height}` |
| `tags.get` | `monitor?` | `{monitor, active, previous, occupied, urgent}` |
| `tags.view` | `monitor?`, `tags`/`tag` | as `tags.get` |
| `tags.toggle` | `monitor?`, `tags`/`tag` | as `tags.get` |

## Input, keys, actions

| Method | Params | Result |
|---|---|---|
| `pointer.get` | | `{x, y, window, confined}` |
| `pointer.warp` | `x?`, `y?`, `window?` | as `pointer.get` |
| `keyboard.get_layout` | | `{name, index}` |
| `keyboard.set_layout` | `layout` ("next"/"prev"/index) | `{name, index}` |
| `keymode.get` / `keymode.set` | `mode` | the mode name |
| `keybinds.list` | | `[{key, action, arg, desc, mode}]` |
| `key.dispatch` | `key` ("Super+Return") | bool: something was bound |
| `action.run` | `name` ("tag-view"), `arg?` | bool |

## Tag transitions

| Method | Params | Result |
|---|---|---|
| `transition.get` | | `{current, label, duration, transitions[{name, label}]}` |
| `transition.set` | `name` (a loaded transition, `random`, `none`, `next`, `prev`), `duration?` | as `transition.get` (toasts like Super+:) |

## Programs and session

| Method | Params | Result |
|---|---|---|
| `spawn` | `argv` or `command`, `shell?`, `cwd?`, `env?`, `monitor?`, `tags?`/`tag?`, `floating?`, `x?`, `y?`, `width?`, `height?`, `relative_to?`, `units?` | `{pid}` |
| `session.lock` | | `{ok}` |
| `session.locked` | | bool |
| `outputs.power` | `value?` | bool |

Screen-acting methods (focus, typing, keys, screenshots, tag views, pointer, spawn) return `-32002` while the session is locked.

## Objects

Window:

```json
{"id":12,"app_id":"foot","title":"~","pid":4242,"monitor":"DP-1","tags":4,
 "x":0,"y":30,"width":960,"height":1050,"border":2,"focused":true,"visible":true,
 "floating":false,"fullscreen":false,"urgent":false,"sticky":false,"pinned":false,
 "below":false,"minimized":false,"embedded":false,"overlay":false,"xwayland":false,
 "opacity":1.0,"parent":null,"mine":true}
```

Monitor:

```json
{"name":"DP-1","make":"Dell","model":"U2723QE","serial":"","focused":true,"enabled":true,
 "powered_off":false,"x":0,"y":0,"width":3840,"height":2160,
 "area":{"x":0,"y":30,"width":3840,"height":2130},"scale":1.5,"transform":0,
 "refresh":60.0,"hdr":false,"tags":1,"occupied":5,"urgent":0,
 "layout":"tile","layout_symbol":"[]=","mfact":0.55,"nmaster":1}
```
