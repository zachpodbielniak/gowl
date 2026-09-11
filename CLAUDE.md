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
- `test-overlay-layout` -- The panel the dropdown and the scratchpad share
  (`gowl_overlay_panel_box()`) and the scratchpad's column split: columns
  fill the panel exactly and share its top edge and height, which is what
  lets several windows slide up as one panel
- `test-overlay-adopt` -- `gowl_compositor_adopt_overlay()` /
  `release_overlay()`: what can be adopted, a hidden overlay being on no
  tag, a floating window coming back where it floated, focus-stack steps
  staying in their overlay group, `show_client` never viewing "no tags",
  and the session file leaving overlays out
- `test-scratchpad-module` -- The scratchpad module against the real .so:
  every command's reply, focus elsewhere rolling it away, members that
  unmap or are destroyed, strict settings, and switching it off giving
  every window back
- `test-compositor-teardown` -- Starts a real compositor (headless
  backend, pixman renderer, a private runtime dir, systemd off) and
  finalizes it in a subprocess: wlroots aborts if any listener is still
  on a signal when its object is destroyed
- `test-cube` -- Desktop cube planner: step count and itinerary, no
  wrap-around, duration growth and cap, slot window, and the envelope's
  flatness at both ends (the property that makes a rotation cut-free)
- `test-cube-render` -- The cube's GL path against a real GLES2 renderer:
  which way is up, that the flat frame fills the viewport corner to
  corner, and that a quarter turn actually changes the picture. Skips
  itself when there is no DRM render node
- `test-effects` -- How scene-effect hooks are shared out: priority
  order, consumable hooks stopping at the first claimant, broadcast hooks
  reaching every provider, and neither kind affecting the other. Both
  failure modes are silent, so this is the file to read before changing
  `src/core/gowl-effects.c`
- `test-fx-render` -- The shared effect layer (`src/fx`) against a real
  GLES2 renderer: quad and ortho orientation agreeing with each other,
  the blur actually blurring, corner rounding actually rounding. Skips
  itself without a DRM render node
- `test-expo` -- The overview's grid: cells keeping the output's aspect,
  the grid fitting, no overlap, the closed zoom landing EXACTLY on the
  anchor tile (the property that makes it cut-free), hit testing and
  keyboard stepping
- `test-blur-shadow` -- The analytic drop shadow: falloff, rounded
  corners, and premultiplied output (straight colour gives every shadow a
  bright halo)
- `test-fx-sheet-guard.sh` -- a capture that hides the client layers also
  hides the effect sheets (a sheet is a sibling of the layer trees and
  holds a picture of the desktop *with* its windows)
- `test-inject-routing.sh` -- injected input goes through the compositor's
  own decision, not straight to the seat
- `test-bar-wifi-scan` -- nmcli output to a network list: one entry per
  NETWORK (nmcli emits one row per BSS, so a dual-band router or a mesh
  repeats an SSID), ranked before truncation, escaped colons, hidden
  networks dropped. Runs against real `nmcli` output via
  `GOWL_TEST_NMCLI_WIFI`
- `test-blur-geom` -- Where the blur backdrop crops the wallpaper, when it
  is rebuilt, and
  the one invariant that matters: the source box must lie inside the
  texture. `wlr_render_pass_add_texture()` asserts it, which aborts the
  compositor mid page-flip -- under `cmacs --gowl` that is the user's
  whole session, reported with an idle pselect backtrace naming nothing
  to do with blur. Carries the crash from the core dump as a regression
  case, plus a sweep over every window position
- `test-inject-keyboard` -- Injected keys and the modifiers they carry:
  the evdev-to-xkb `+8` offset (getting it wrong types a *different
  letter*, silently), Shift/Ctrl/Alt actually modifying, and local and
  remote modifiers composing on one state
- `test-input-recorder` -- Input recording: consent gate, bounded ring,
  motion coalescing, self-stop deadline, secret-suppression policy,
  payload shape, and the config-to-recorder wiring

`make test` also runs every `tests/*.sh` **source guard** before the compiled
tests. These assert invariants no unit test can reach:
- `test-no-libregnum.sh` -- gowl links no rendering engine (see below)
- `test-portal-guard.sh` -- `xdg-desktop-portal-gowl` still serves BOTH
  libei directions. A libei client declares itself sender or receiver at
  connect time; a receiver is a KVM sharing this machine (InputCapture),
  a **sender** is a KVM driving it (RemoteDesktop). Rejecting senders is
  why client mode used to do nothing, and it fails with no error on
  either side: the D-Bus handshake succeeds, `ConnectToEIS` hands over a
  working fd, and the connection dies the instant libei states its
  direction. A source guard rather than a unit test because this machine
  has libeis (server) but not libei (client), so there is nothing to
  build a real sender with
- `test-cube-guard.sh` -- the `cube` module still claims ONLY its own
  reveal (it sorts ahead of every other effect module, so a `client_event`
  that started returning TRUE broadly would silently switch window
  animations off), still leaves `get_geometry` / `surface_at` /
  `alpha_changed` to the animation module, and still takes an output with
  an opaque sheet rather than by disabling a shared, cross-monitor scene
  layer
- `test-close-guard.sh` -- `wlr_xdg_toplevel_send_close` /
  `wlr_xwayland_surface_close` have exactly one call site each (inside
  `gowl_client_close()`), `gowl_compositor_focus_client()` still consults
  `gowl_focus_decide()`, and no new `wlr_seat_keyboard_notify_enter` call
  sites have appeared
- `test-overlay-guard.sh` -- `on_client_unmap()` still hands an adopted
  overlay back (a toplevel that maps again would otherwise come back
  invisible, on no tag, with nothing to show it), and the focus_stack
  action still steps through `gowl_compositor_stack_neighbour()` (else
  Super+j on a shown scratchpad lands on a tile and rolls it away)
- `test-teardown-guard.sh` -- every listener `gowl_compositor_start()`
  adds is taken back off by `remove_compositor_listeners()`, which
  finalize calls before it destroys anything, and the capture provider's
  listener on the toplevel image-capture source comes off in its finalize
- `test-record-guard.sh` -- the input recorder's taps are still wired to
  all six input hooks, the injection helpers and `motionnotify` are still
  *un*wired (they are reached by both the real and the synthetic path, so
  a tap there would record gowl's own injected input), `on_kb_key` still
  carries the Super+Shift+Escape force-stop, and the on-screen indicator
  is still raised from the recorder's own state change

> **Both directions of libei are load-bearing, and they are asymmetric.**
> `tools/xdg-desktop-portal-gowl` is one libeis context serving two
> opposite roles: a *receiver* client is a software KVM sharing this
> machine's input (InputCapture, the compositor pushes at it), a *sender*
> client is a KVM driving this machine (RemoteDesktop, it pushes at us and
> we inject). Both arrive through the same `ConnectToEIS`, and the only
> thing distinguishing them is one flag — so a backend written for capture
> alone looks complete and fails silently in the other direction. Zones
> must reach the EIS device regardless of which portal is in use: a
> RemoteDesktop-only client never calls `GetZones`, and a device with no
> region cannot be positioned absolutely. See `docs/input-capture.org`.

> **Injected input must take the same path as real input.** The injectors
> call `compositor_handle_key()` / `compositor_handle_button()`, never
> `wlr_seat_*_notify_*`. Straight to the seat means the focused client and
> nothing else: no keybinds, no module keybinds, no embedder intercept, no
> bar hit test. That was the bug behind "deskflow can type but no gowl
> keybind works, and the bar is unclickable from the remote mouse but fine
> from the real one". Synthetic input skips exactly three things — the
> recorder tap (never record our own injections), the InputCapture
> diversion (would echo input back to its sender) and key repeat (the
> sender repeats already). `tests/test-inject-routing.sh` enforces it.

> **A captured image that outlives the frame needs a buffer you own.**
> `gowl_fx_capture_to_buffer()` returns a slot of the *output's* swapchain
> — fine to present and drop in one frame, wrong to keep. Keeping one takes
> a buffer out of the output's rotation and ties the content to a pool the
> compositor keeps drawing the live desktop into; if the slot is handed back
> out, what you hold silently becomes a photograph of the desktop with its
> windows. That was the blur backdrop showing another tag's app through
> translucent windows. Allocate your own `wlr_swapchain` like `GowlFxSheet`
> does. `tests/test-fx-buffer-ownership.sh` enforces it.

> **A `GowlFxSheet` is NOT in a layer, so hiding the layers does not hide it.**
> A sheet's tree is a direct child of `scene->tree`, a sibling of the layer
> trees, so it can sit above or below whole layers. It is also an opaque,
> monitor-sized picture of the desktop *with its windows in it*, parked by
> the cube/expo/switcher. So any capture that hides the client layers to see
> past the windows must also call `gowl_fx_vis_hide_sheets(vis)` — blur's
> wallpaper backdrop, rebuilt during a tag switch while the cube's sheet was
> up, otherwise captured the tag being *left* and cached it as current (a
> translucent terminal on tag 2 showing the chat window from tag 4).
> `tests/test-fx-sheet-guard.sh` enforces it. Related ordering rule:
> `gowl_compositor_arrange()` applies tag visibility to the whole monitor
> *before* announcing `REVEAL`, because an effect handling REVEAL may
> capture, and a one-pass loop would let it see a half-switched monitor.

> **A source box outside its texture aborts the whole session.**
> `wlr_render_pass_add_texture()` asserts `src.x >= 0 && src.y >= 0 &&
> src.x + src.width <= texture->width && src.y + src.height <=
> texture->height`. That assert is not a debug aid: it aborts the process,
> on the compositor thread, during a page flip. So never derive a source
> box from `GowlClient.geom` -- that is the *layout's* idea of the window
> and is routinely off-output (a scrolling layout leaves it unclipped on
> purpose; a floating window dragged half off screen is never clipped at
> all). Use `GowlClient.frame`, the frame as drawn, which
> `gowl_compositor_apply_frame_geometry()` records. Intersect with the
> monitor, then clamp to the texture -- clamping the width alone misses
> the window scrolled off the *left* edge, which fails `src.x >= 0`
> instead. See *A source box outside its texture aborts the session* in
> `docs/architecture.org`.

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

> **A hidden overlay is on no tag (`tags == 0`), and a view of no tags is an
> empty screen.** Hidden overlays -- the dropdown's shell, the scratchpad's
> windows -- keep `tags = 0` so nothing counts them as occupying a tag;
> `gowl_compositor_present_overlay()`, `adopt_overlay()` and `setmon()` keep
> it that way. So anything that switches a monitor to *a client's* tags must
> skip 0: `gowl_compositor_show_client()` did not, and jumping to a hidden
> overlay would have set its monitor to viewing nothing, taking every window
> off the screen. `tests/test-overlay-adopt.c` holds it. The same reason
> keeps overlays out of the session file.

> **Teardown takes the compositor's own listeners off first.** wlroots
> asserts that a signal has no listeners left when its object is destroyed
> -- XWayland, the keyboard group, the backend, every global
> `wl_display_destroy()` takes with it -- and an assert is an abort.
> `remove_compositor_listeners()` runs before finalize destroys anything
> (dwl's `cleanuplisteners()`), so a listener added to the compositor in
> `gowl_compositor_start()` has to be added there too, and a sub-object
> that listens on a global takes its listener off in its own finalize, as
> the capture provider does; `tests/test-teardown-guard.sh` checks both.
> Effects are closed by then: after
> `gowl_effects_finish()` nothing is dispatched to a provider. Standalone
> gowl and cmacs `--gowl` exit with the compositor alive, so only the
> tests and cmacs's `gowl-stop` finalize one --
> `tests/test-compositor-teardown.c` does it for real.

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
  fx/                       # Shared visual-effect layer (GL, capture, sheet) used by the effect modules
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

> **`src/fx/` is the only place with GL, and it OWNS none of it.** The
> five visual-effect modules (`cube`, `expo`, `switcher`, `magnifier`,
> `blur`) contain no GL at all: they call `gowl_fx_*`, which borrows the
> EGL context wlroots' GLES2 renderer already owns and restores whatever
> was current. Three invariants hold across every caller -- Y is negated
> in the projections (a wlroots buffer's first row is the top, GL's is
> the bottom), there is no depth buffer (colour-only framebuffer; draw
> back to front and cull on the CPU), and a `GowlFxVis` capture is a lie
> that must be taken back on every exit path. See *Visual effect layer*
> in `docs/architecture.org`.

> **Do not add a rendering-engine dependency.** The raw-frame sink
> (`src/core/gowl-frame-sink.{c,h}`, see *Raw Frame Sink* in
> `docs/architecture.org`) lets external producers (e.g. cmacs screensavers)
> hand gowl finished ARGB8888 pixels — gowl must stay free of `libregnum` /
> `graylib` / `raylib`, and must not gain a renderer of its own.
> `tests/test-no-libregnum.sh` (run by `make test`) enforces this and will
> fail the build if those symbols/headers leak in. Borrowing the context
> wlroots' GLES2 renderer already owns is a different thing and is what
> `src/fx/` does; that boundary file itself stays free of GL includes.

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
