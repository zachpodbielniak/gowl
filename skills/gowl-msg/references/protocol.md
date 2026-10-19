# The gowl wire protocol: framing, errors, ordering

Read this before writing a client that talks to the socket directly. The authoritative document is `docs/ipc.org` in the gowl tree (section *The wire protocol*).

## Connection

- Unix stream socket: `$GOWL_SOCKET`, else `$XDG_RUNTIME_DIR/gowl.sock`.
- No handshake. Connect and send. `rpc.discover` returns `{protocol: "gowl-wire", version: 1, server, caller: {pid, uid, connection}, methods[], events[]}`.
- The version changes only when an existing method, parameter or event changes meaning. Additions do not bump it -- check `methods[]` for what you need.
- The server knows the caller's pid (SO_PEERCRED). `mine=true` filters use it.

## Framing

- One JSON value per line, UTF-8, `\n`-terminated (a trailing `\r` is dropped). Up to 1 MiB per line.
- A line whose first non-blank byte is `{` is one request; `[` is a batch. Anything else is a text-dialect command on the same connection.
- Replies never contain a raw newline. Read a line, parse it.

Request:

```json
{"jsonrpc":"2.0","id":7,"method":"window.set_geometry","params":{"id":12,"x":0,"y":0,"width":960,"height":1080}}
```

- `jsonrpc` is optional; if present it must be `"2.0"`.
- `id`: string, integer or null; echoed back. **No `id` = notification**: it runs, and nothing at all is sent back (not even an error).
- `params`: an object, or absent. Arrays are refused (`-32602`): every parameter has a name.
- A member set to `null` counts as absent.

Response, one of:

```json
{"jsonrpc":"2.0","id":7,"result":{...}}
{"jsonrpc":"2.0","id":7,"error":{"code":-32001,"message":"no window 12","data":{"method":"window.set_geometry"}}}
```

## Parameter checking

Every request is checked against the method's declaration before it runs:

- unknown member -> `-32602`, message lists the accepted names (catches `widht`);
- missing required member -> `-32602`;
- wrong type -> `-32602`. Types: `int` (3 or 3.0), `number`, `bool`, `string`, `array`, `object`, `toggle` (true/false/"toggle"; absent = toggle), `window` (an id or "focused"), `monitor` (an output name or "focused").

Optional `id` absent means the focused window; optional `monitor` absent means the focused output. `window.focus` and `window.close` require `id`.

## Errors

| Code | Meaning | What to do |
|---|---|---|
| -32700 | not JSON | fix the serializer; `id` is null |
| -32600 | not a request (no method, bad id, empty batch, line too long) | fix the request |
| -32601 | no such method | `rpc.discover`; a module may not be loaded |
| -32602 | bad parameter | the message names it |
| -32603 | compositor fault | retry later |
| -32000 | failed (spawn could not exec, key undeliverable) | report |
| -32001 | not found (window, output, layout, action) | re-list; the window may have closed |
| -32002 | refused (locked, cmacs-embedded window, overlay, no HDR) | do not retry blindly; the message says why |
| -32003 | unsupported in this build / module not loaded | degrade |

`error.data.method` names the failing method (useful inside a batch).

## Batches

```json
[{"id":"a","method":"tags.view","params":{"tag":2}},{"method":"layout.set","params":{"name":"monocle"}},{"id":"b","method":"window.focus","params":{"id":12}}]
```

- Requests run in order inside **one** event-loop callback: no frame is drawn between them, so a batch that moves several windows shows one picture.
- The reply is one line: an array of responses in request order, notifications left out. All-notifications: no reply. `[]`: `-32600`.
- One failure does not stop the rest. For all-or-nothing placement use `layout.apply`, which checks every entry before moving any.

## Ordering and connections

- Responses come back in request order per connection.
- **A request's own events arrive before its response** (they are pushed while it runs). On a subscribed connection, read lines until the one whose `id` is yours; keep the `"method":"event"` lines you pass.
- Simplest robust design: one connection subscribed to events, another for calls.
- Half-close (`shutdown(SHUT_WR)`) after sending: you still receive every reply, then EOF. A subscribed connection that half-closes keeps receiving events until it hangs up.

## Back-pressure

The compositor never blocks on a client. Unread output is queued; past 16 MiB the connection is dropped. A subscriber must keep reading -- pipe through `--line-buffered` tools, never into a pager you might leave paused for long.

## Events

`events.subscribe {"events": ["window.*", "tags.changed"], "add"?: bool}` -- glob patterns over names; absent or `["*"]` = all. Each event:

```json
{"jsonrpc":"2.0","method":"event","params":{"event":"window.focus","seq":17,"time":1792366904270,"data":{"id":12,"window":{...}},"text":"EVENT focus 12"}}
```

`seq` increases by one per event across the server's life (a gap after reconnecting = missed events: resync with `gowl.state`). `time` is wall-clock ms. `text` is the text-dialect form when there is one. Names and payloads: [events.md](events.md).

## Geometry

`x`, `y`, `width`, `height` are the window **frame** (border included), as `windows.list` reports. Modifiers:

- `relative_to`: `layout` (absolute; default for `window.set_geometry`), `monitor` (from the output's corner), `area` (from the corner of the output less its bars; default for `layout.apply` and `spawn`).
- `units`: `px` (default) or `fraction` of the monitor/area box.
- Absent members keep their current value.
- Placing floats the window and leaves fullscreen.

## cmacs --gowl

Same socket path, same protocol, `GOWL_SOCKET` exported to processes Emacs starts. Methods run on the compositor's dispatch thread under the gowl lock, never in Emacs's Lisp thread. Windows with `embedded: true` live in Emacs buffers: placement and state methods refuse them (`-32002`); queries, focus, close, typing, keys, screenshots and opacity work.
