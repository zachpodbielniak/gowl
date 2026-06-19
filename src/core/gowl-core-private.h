/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * gowl-core-private.h - Internal struct definitions for core types.
 *
 * This header is NOT installed.  It is included only by core .c files
 * that need access to each other's private fields (the GObject-based
 * equivalent of dwl's global state).
 */

#ifndef GOWL_CORE_PRIVATE_H
#define GOWL_CORE_PRIVATE_H

/* gowl public headers (forward decls + GObject type macros) */
#include "gowl-types.h"
#include "gowl-enums.h"
#include "config/gowl-config.h"
#include "ipc/gowl-ipc.h"
#include "module/gowl-module-manager.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "core/gowl-seat.h"
#include "core/gowl-keyboard-group.h"
#include "core/gowl-cursor.h"
#include "core/gowl-idle-manager.h"
#include "core/gowl-bar.h"
#include "core/gowl-layer-surface.h"
#include "boxed/gowl-process-info.h"
#include "interfaces/gowl-prefix-key-policy.h"
#include "protocols/gowl-ext-workspace.h"

/* wayland */
#include <wayland-server-core.h>

/* wlroots - core */
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

/* wlroots - input */
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_xcursor_manager.h>

/* wlroots - shell protocols */
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>

/* wlroots - misc protocols */
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_buffer.h>

/* wlroots - utility */
#include <wlr/util/log.h>

/* wlroots - backend detection (for nested Wayland) */
#include <wlr/backend/wayland.h>

/* xkb */
#include <xkbcommon/xkbcommon.h>

/* -----------------------------------------------------------
 * Macros
 * ----------------------------------------------------------- */

/**
 * LISTEN:
 * @E: pointer to a struct wl_signal
 * @L: pointer to a struct wl_listener (must be embedded in a struct)
 * @H: the callback function (void (*)(struct wl_listener *, void *))
 *
 * Convenience macro that sets the notify callback on @L and adds it
 * to the signal @E in a single expression.  Ported from dwl.
 */
#define LISTEN(E, L, H)  wl_signal_add((E), ((L)->notify = (H), (L)))

/**
 * LISTEN_STATIC:
 * @E: pointer to a struct wl_signal
 * @H: the callback function
 *
 * Heap-allocates a wl_listener and adds it to the signal.  The callback
 * is responsible for calling wl_list_remove(&listener->link) and
 * free(listener) when it no longer needs the listener.
 */
#define LISTEN_STATIC(E, H) \
	do { \
		struct wl_listener *_l = \
			(struct wl_listener *)g_malloc0(sizeof(*_l)); \
		_l->notify = (H); \
		wl_signal_add((E), _l); \
	} while (0)

/* -----------------------------------------------------------
 * Core struct definitions
 * ----------------------------------------------------------- */

#ifdef GOWL_HAVE_LIBDECOR
typedef struct _GowlDecor GowlDecor;
#endif

/**
 * struct _GowlCompositor:
 *
 * Main compositor singleton.  Mirrors dwl's global state as embedded
 * fields in a GObject so the entire lifecycle is ref-counted.
 */
struct _GowlCompositor {
	GObject parent_instance;

	/* wlroots core */
	struct wl_display            *wl_display;
	struct wl_event_loop         *event_loop;
	struct wlr_backend           *backend;
	struct wlr_session           *session;
	struct wlr_renderer          *renderer;
	struct wlr_allocator         *allocator;
	struct wlr_scene             *scene;
	struct wlr_compositor        *wlr_compositor;
	struct wlr_output_layout     *output_layout;

	/* scene graph layers (indexed by GowlSceneLayer) */
	struct wlr_scene_tree        *layers[GOWL_SCENE_LAYER_COUNT];
	struct wlr_scene_tree        *drag_icon;
	struct wlr_scene_rect        *root_bg;
	struct wlr_scene_rect        *locked_bg;

	/* protocol globals */
	struct wlr_xdg_shell                    *xdg_shell;
	struct wlr_layer_shell_v1               *layer_shell;
	struct wlr_idle_notifier_v1             *idle_notifier;
	struct wlr_idle_inhibit_manager_v1      *idle_inhibit_mgr;
	struct wlr_session_lock_manager_v1      *session_lock_mgr;
	struct wlr_xdg_decoration_manager_v1    *xdg_decoration_mgr;
	struct wlr_output_manager_v1            *output_mgr;

	/* gowl sub-objects (compositor-owned) */
	GowlConfig                   *config;       /* borrowed ref */
	GowlModuleManager            *module_mgr;   /* borrowed ref */
	GowlIpc                      *ipc;          /* borrowed ref (may be NULL) */

	/* Runtime-pluggable key policy.  When non-NULL, the compositor
	 * consults it on each key press to decide whether to redirect
	 * keyboard focus to an alternate target (typically the Emacs
	 * client under cmacs `--gowl`).  Always NULL in standalone and
	 * nested gowl, so the intercept path short-circuits. */
	GowlPrefixKeyPolicy          *prefix_key_policy; /* owned, nullable */

	/* Runtime-pluggable workspace manager.  When non-NULL the
	 * compositor becomes authoritative for the workspace-* signals.
	 * Default for standalone and nested: NULL (no workspaces; the
	 * global-tag model reigns).  Cmacs `--gowl' installs a
	 * #GowlFrameWorkspaceManager at startup. */
	GowlWorkspaceProvider        *workspace_provider; /* owned, nullable */

	/* ext-workspace-v1 server.  Registered during
	 * gowl_compositor_start() so external bars (waybar / eww) see
	 * workspace state once a GowlWorkspaceProvider is installed.
	 * Opaque to the compositor — defined in protocols/gowl-ext-workspace.c. */
	gpointer                      ext_workspace_manager;

	/* input sub-objects (owned by compositor, created in start) */
	struct wlr_seat              *wlr_seat;
	struct wlr_cursor            *wlr_cursor;
	struct wlr_xcursor_manager   *xcursor_mgr;
	struct wlr_keyboard_group    *wlr_kb_group;

	/* GObject wrappers for input sub-objects (owned, created in start) */
	GowlSeat                    *seat;
	GowlCursor                  *cursor_obj;
	GowlKeyboardGroup           *kb_group_obj;
	GowlIdleManager             *idle_mgr;
	GowlBar                     *bar;           /* NULL unless bar module active */

	/* keybind state for key-repeat */
	gint                          kb_nsyms;
	const xkb_keysym_t          *kb_keysyms;
	guint32                       kb_mods;
	struct wl_event_source       *key_repeat_source;

	/* client / monitor lists */
	GList                        *monitors;   /* GList of GowlMonitor* */
	GList                        *clients;    /* GList of GowlClient* (tiling) */
	GList                        *fstack;     /* GList of GowlClient* (focus) */
	GowlMonitor                  *selmon;     /* selected monitor */

	/* state */
	gboolean   running;
	gboolean   locked;
	const char *socket_name;

	/* laptop-lid output management (see gowl-lid-policy.h) */
	gboolean            lid_closed;          /* TRUE = lid shut */
	struct wlr_switch  *lid_switch;          /* first LID switch, or NULL */
	struct wl_listener  lid_toggle;          /* wlr_switch->events.toggle */
	struct wl_listener  lid_switch_destroy;  /* lid device destroy */

	/* border colours parsed from config (RGBA floats) */
	float focus_color[4];
	float unfocus_color[4];
	float urgent_color[4];
	float fullscreen_bg_color[4];
	float root_color[4];

	/* embedded wl_listeners (compositor-level events) */
	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_layer_surface;
	struct wl_listener new_xdg_decoration;
	struct wl_listener layout_change;
	struct wl_listener gpu_reset;
	struct wl_listener new_idle_inhibitor;
	struct wl_listener new_session_lock;
	struct wl_listener request_activate;

	/* session lock state */
	struct wlr_session_lock_v1 *cur_lock;
	struct wl_listener lock_new_surface;
	struct wl_listener lock_destroy;
	struct wl_listener lock_unlock;
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;

	/* seat listeners */
	struct wl_listener request_cursor;
	struct wl_listener request_set_sel;
	struct wl_listener request_set_psel;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener request_set_cursor_shape;

	/* cursor listeners */
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	/* keyboard listeners */
	struct wl_listener kb_key;
	struct wl_listener kb_modifiers;

	/* cursor grab state (for interactive move/resize) */
	gint      cursor_mode;    /* GowlCursorMode */
	GowlClient *grabbed_client;
	gdouble   grab_x, grab_y;
	struct wlr_box grab_geobox;
	guint32   resize_edges;

	/* PIDs that should be floated + hidden on map (for embedding) */
	GArray  *prefloat_pids;
	/* New-style prefloat: GArray of GowlPrefloatHintEntry for
	 * dropdown-style pid→client capture with explicit geometry
	 * and a caller-owned on_mapped callback.  Orthogonal to the
	 * legacy prefloat_pids embedder path. */
	GArray  *prefloat_hints;
	/* PIDs whose client should adopt a specific tag bitmask on map
	 * (for "launch into tag N").  GArray of GowlPretagEntry,
	 * consumed on first match.  Orthogonal to prefloat. */
	GArray  *pretag_pids;

	/* Key intercept callback (embedder hook) */
	GowlKeyInterceptFunc key_intercept_func;
	gpointer             key_intercept_data;

	/* Client map callback (embedder hook) */
	GowlClientMapFunc client_map_func;
	gpointer          client_map_data;

#ifdef GOWL_HAVE_LIBDECOR
	/* Nested Wayland decoration (libdecor) */
	GowlDecor           *decor;             /* NULL when not nested */
	struct wlr_backend  *nested_wl_backend; /* Wayland sub-backend */
	struct wlr_output   *default_wl_output; /* default output to destroy */
	gchar               *parent_wl_display; /* parent $WAYLAND_DISPLAY */
#endif
};

/**
 * struct _GowlMonitor:
 *
 * Represents a physical output.  Holds per-output tag state, layout
 * config, and the wlroots output/scene objects.
 */
struct _GowlMonitor {
	GObject parent_instance;

	struct wlr_output       *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect   *fullscreen_bg;

	struct wlr_box m;   /* monitor area, layout-relative */
	struct wlr_box w;   /* window area (minus bar / layer-shell) */

	guint32  tagset[2];
	guint    seltags;    /* index into tagset[] (0 or 1) */
	guint    sellt;      /* selected layout index */
	gint     nmaster;
	gdouble  mfact;
	gchar   *layout_symbol;

	GowlCompositor *compositor;  /* back-reference (unowned) */

	/* per-monitor layer surfaces (GList of GowlLayerSurface*) */
	GList   *layer_surfaces;

	/* embedded wl_listeners */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
};

/**
 * struct _GowlClient:
 *
 * Represents an XDG toplevel client window.  Holds geometry, tags,
 * display state, and wlroots scene/surface objects.
 */
struct _GowlClient {
	GObject parent_instance;

	guint   id;             /* unique monotonic client ID */
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree   *scene;          /* client container node */
	struct wlr_scene_tree   *scene_surface;  /* xdg_surface node */
	struct wlr_scene_rect   *border[4];      /* top, bottom, left, right */

	struct wlr_box geom;    /* layout position including border */
	struct wlr_box prev;    /* saved geometry for fullscreen restore */

	guint32  tags;
	guint    bw;             /* border width in pixels */
	gboolean isfloating;
	gboolean isurgent;
	gboolean isfullscreen;
	gboolean isembedded;     /* externally managed (skip arrange) */
	gfloat   alpha;          /* opacity: 0.0 (transparent) to 1.0 (opaque) */
	guint32  resize;         /* pending configure serial */

	gchar *title;
	gchar *app_id;

	/* Transient rule-override fields populated by handlers of
	 * GowlCompositor::client-pre-map.  on_client_map() reads them
	 * once after the signal returns, before calling setmon(), and
	 * never touches them again.  Zero means "no override".
	 *
	 * pending_rule_tags:     non-zero tag bitmask to assign instead
	 *                        of the selected monitor's current tags
	 * pending_rule_monitor:  target monitor index (-1 = unset)
	 * pending_rule_geom_set: TRUE if a handler set geom manually
	 *                        and resize_client should respect it
	 */
	guint32  pending_rule_tags;
	gint     pending_rule_monitor;
	gboolean pending_rule_geom_set;

	/* XDG decoration (may be NULL if client doesn't negotiate) */
	struct wlr_xdg_toplevel_decoration_v1 *decoration;

	GowlMonitor    *mon;         /* assigned monitor (unowned) */
	GowlCompositor *compositor;  /* back-reference (unowned) */

	/* embedded wl_listeners */
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy_surface;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener maximize;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;

	/* Mirror views: additional scene nodes that duplicate this
	 * client's root surface buffer.  Populated by
	 * gowl_client_add_mirror(); each entry is a strong GowlMirror
	 * ref that stays alive until gowl_client_remove_mirror() or
	 * client destruction.  next_mirror_id is a monotonic
	 * per-client allocator.  Both fields stay zero-initialised in
	 * standalone and nested modes until the cmacs integration
	 * layer calls the mirror API. */
	GList   *mirrors;        /* GList of GowlMirror* (owned) */
	guint64  next_mirror_id;
};

/**
 * struct _GowlSeat:
 *
 * Thin GObject wrapper around the wlr_seat.  The actual seat,
 * keyboard group, and cursor are owned by the compositor; this
 * object exists to preserve the public GObject API.
 */
struct _GowlSeat {
	GObject parent_instance;

	gpointer wlr_seat;           /* struct wlr_seat* (alias) */
	gpointer focused_client;     /* GowlClient* */
	gpointer keyboard_group;     /* GowlKeyboardGroup* */
	gpointer cursor;             /* GowlCursor* */
};

/**
 * struct _GowlKeyboardGroup:
 *
 * Thin GObject wrapper around wlr_keyboard_group.  The actual
 * keyboard group is owned by the compositor.
 */
struct _GowlKeyboardGroup {
	GObject parent_instance;

	gpointer wlr_group;      /* struct wlr_keyboard_group* (alias) */
	gpointer xkb_context;    /* struct xkb_context* (alias) */
	gint     repeat_rate;
	gint     repeat_delay;
};

/**
 * struct _GowlCursor:
 *
 * Thin GObject wrapper around wlr_cursor.  The actual cursor and
 * xcursor manager are owned by the compositor.
 */
struct _GowlCursor {
	GObject parent_instance;

	gpointer wlr_cursor;         /* struct wlr_cursor* (alias) */
	gpointer xcursor_manager;    /* struct wlr_xcursor_manager* (alias) */
	gint     mode;               /* GowlCursorMode */
	gpointer grabbed_client;     /* GowlClient* */
	gdouble  grab_x;
	gdouble  grab_y;
	gint     grab_width;
	gint     grab_height;
};

/**
 * struct _GowlIdleManager:
 *
 * Wraps the wlroots idle notification system.  Tracks idle timeout
 * and current state (active / idle).
 */
struct _GowlIdleManager {
	GObject   parent_instance;

	gpointer  wlr_idle_notifier;          /* struct wlr_idle_notifier_v1* */
	gpointer  wlr_idle_inhibit_manager;   /* struct wlr_idle_inhibit_manager_v1* */
	gint      timeout_secs;
	gint      state;                       /* 0 = ACTIVE, 1 = IDLE */
};

/**
 * struct _GowlBar:
 *
 * Wraps a status bar rendered via a wlr_scene_buffer.  Tracks bar
 * height, visibility, and the associated monitor.
 */
struct _GowlBar {
	GObject   parent_instance;

	gpointer  scene_buffer;   /* struct wlr_scene_buffer* */
	gint      height;
	gboolean  visible;
	gpointer  monitor;        /* GowlMonitor* */
};

/**
 * struct _GowlLayerSurface:
 *
 * Represents a wlr-layer-shell surface (panel, wallpaper, overlay).
 */
struct _GowlLayerSurface {
	GObject parent_instance;

	struct wlr_layer_surface_v1          *wlr_layer_surface;
	struct wlr_scene_layer_surface_v1    *scene_layer_surface;
	struct wlr_scene_tree                *scene;
	gint      layer;
	gboolean  mapped;

	GowlMonitor    *mon;         /* assigned monitor (unowned) */
	GowlCompositor *compositor;  /* back-reference (unowned) */

	/* embedded wl_listeners */
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy_surface;
	struct wl_listener commit;
};

/* -----------------------------------------------------------
 * Internal function declarations (used across core .c files)
 * ----------------------------------------------------------- */

/* compositor internal helpers */
void gowl_compositor_arrange      (GowlCompositor *self, GowlMonitor *m);
void gowl_compositor_focus_client (GowlCompositor *self,
                                   GowlClient     *c,
                                   gboolean        lift);
void gowl_compositor_arrangelayers(GowlCompositor *self, GowlMonitor *m);
void gowl_compositor_motionnotify (GowlCompositor *self,
                                   guint32         time_msec);

/* colour parsing: "#rrggbb" or "#rrggbbaa" -> float[4] */
void gowl_color_parse_to_floats   (const gchar *hex, float out[4]);

#endif /* GOWL_CORE_PRIVATE_H */
