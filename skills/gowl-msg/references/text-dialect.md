# The text dialect

One command line in, one line back. Queries answer JSON on one line; everything else answers `OK ...` or `ERROR ...`. For a person at a shell, bar scripts, and module commands that have no wire-protocol method. From a program, prefer the wire protocol, or reach any of these through `gowl.command {"line": "..."}`, which returns `{ok, reply, json?}`.

```bash
gowl-msg clients | yq -p json '.[].app_id'
gowl-msg view 4                 # tag 3 (bitmask)
gowl-msg -s                     # text events: "EVENT focus 12", "EVENT mode resize", ...
```

## Core commands

| Command | Reply |
|---|---|
| `clients`, `focused`, `monitors`, `profile`, `tags [OUTPUT]`, `layouts`, `keybinds` | JSON |
| `layout [NAME]`, `mode [NAME]`, `keyboard-layout [next\|prev\|N]` | read, or set then read |
| `focus ID`, `close ID`, `view TAGMASK [OUTPUT]` | OK / ERROR |
| `dispatch KEY`, `action NAME [ARG]` | run a binding / an action |
| `power on\|off\|toggle`, `hdr [on\|off\|toggle] [OUTPUT]`, `hdr-state [OUTPUT]`, `crt [on\|off\|toggle]` | OK on/off |
| `pointer-confine [on\|off\|toggle\|state]` | JSON, or OK on/off |
| `tray [list\|activate\|menu\|secondary KEY]` | JSON / OK |
| `backdrop [STYLE\|next\|prev]` | OK STYLE |
| `tag-transition [NAME\|next\|prev\|random\|none\|list]` | OK NAME, or the loaded transitions as JSON |
| `hints [-show\|-hide]`, `hints-select LABEL`, `hints-list` | window hints |
| `lock`, `unlock`, `locked` | |
| `reload`, `ping`, `version`, `help`, `subscribe` | |
| `menu [ROUTE]`, `menu-path`, `menu-open ROUTE`, `menu-close`, `menu-refresh`, `menu-list [ROUTE]` | the Super+space card |
| `screenshot-ocr`, `screenshot-color`, `screenshot-area`, ... | screenshot module |

## Module commands (when the module is loaded)

- `expo`, `scratchpad-toggle`, `scratchpad-add`, `scratchpad-remove`, `clipboard-list`, `notify-dnd`, ...
- `inputremap-devices|list|add RULE|remove NAME|clear|reload|enable|disable|log|identify|status`
- `broadcast-on|off|toggle|panic|status|list|add-team YAML|add-rule YAML|assign TEAM SLOT [ID]|...`
- `gabbrev-status|list|on|off|toggle|reload|load PATH|add YAML|remove NAME|clear|expand ID|insert TEXT|search|render ID|test TEXT|resolve ID JSON|cancel|builtin`
- `macro-run NAME ARGS`, `macro-stop`, `macro-list`, `macro-status`, `macro-record ...`, `macro-voice ...`

An unknown command answers `ERROR unknown command 'X'`. `gowl-msg help` prints the core list.

## Text events

`EVENT client-added ID APP_ID`, `client-removed ID`, `focus ID`, `title TEXT`, `tags OUTPUT ACTIVE OCCUPIED 0 ACTIVE` (keybind paths only), `layout OUTPUT SYMBOL`, `mode NAME`, `keymap INDEX NAME`, `power OUTPUT on|off`, `profile NAME`, `monitor-added|removed OUTPUT`, `lock locked|unlocked`, `pointer-confine ...`, `crt on|off`, `backdrop STYLE`, `command LINE`, and module events. The wire protocol's events carry all of these structured, plus `window.changed`, `tags.changed` from every source, `layout.changed` and `monitor.changed`.
