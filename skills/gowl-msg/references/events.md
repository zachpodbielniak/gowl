# Wire-protocol events

Subscribe with `events.subscribe {"events": [PATTERNS]}` (glob patterns; absent = all) or `gowl-msg -s -j PATTERN...`. Each arrives as:

```json
{"jsonrpc":"2.0","method":"event","params":{"event":NAME,"seq":N,"time":MS,"data":{...},"text":"EVENT ..."}}
```

| Event | When | `data` |
|---|---|---|
| `window.added` | a window mapped and was placed | `{id, window}` |
| `window.removed` | a window unmapped | `{id}` |
| `window.focus` | keyboard focus moved | `{id, window}`; `id` null when nothing has focus |
| `window.title` | the focused window's title changed (also on focus) | `{id, title}` |
| `window.changed` | a window's geometry or state changed | `{id, changed[], window}` |
| `window.minimized` / `window.restored` | minimize / restore | `{id}` |
| `tags.changed` | an output views other tags -- from ANY source | `{monitor, active, previous, occupied, urgent}` |
| `layout.changed` | an output's layout changed | `{monitor, layout, symbol, previous}` |
| `monitor.changed` | geometry, window area, scale, transform, enabled, mfact or nmaster | `{name, monitor}` |
| `monitor.added` / `monitor.removed` | hotplug | `{name, monitor}` / `{name}` |
| `monitor.power` | DPMS on/off | `{monitor, on}` |
| `keymode.changed` | key mode entered or left | `{mode}` |
| `keyboard.layout` | keyboard layout switched | `{index, name}` |
| `profile.changed` | another output profile matches | `{profile}` (null = none) |
| `session.lock` | locked / unlocked | `{locked}` |
| `pointer.confine` | confinement changed | `{active, id?, reason}` |
| `crt.changed` | CRT filter switched | `{on}` |
| `backdrop.changed` | translucency backdrop changed | `{style}` |
| `command` | a module command ran, from any source | `{line}` |
| `macro`, `broadcast`, `inputremap`, `screenshot`, ... | a module's own event | `{args, words[]}` |

## Semantics worth knowing

- `window.changed.changed` lists any of `geometry`, `tags`, `monitor`, `floating`, `fullscreen`, `minimized`, `sticky`, `below`, `urgent`, `visible`. One action can produce several events: floating one window re-tiles its neighbours, each announced, and a window's `floating` change can arrive before its `geometry` change.
- A window's first appearance is `window.added`, never `window.changed`.
- `tags.changed` / `layout.changed` / `monitor.changed` come from a diff after every arrange, so a tag view from a key, Elisp, MCP or the socket is announced the same way. The text dialect's `EVENT tags` is not -- do not rely on it from a program.
- Events a request causes arrive before that request's response on the same connection.
- `seq` gaps mean missed events (reconnect): resync with `gowl.state`.

## Shell patterns

```bash
# React to every new window of one app
gowl-msg -s -j window.added |
while read -r ev; do
	[[ $(yq -p json '.data.window.app_id' <<<"$ev") == org.example.tool ]] || continue
	gowl-msg -c window.center id="$(yq -p json '.data.id' <<<"$ev")" width=800 height=500
done
```

```bash
# Current tag for a status line, updated live
gowl-msg -s -j tags.changed | yq -p json --unwrapScalar '.data.active'
```
