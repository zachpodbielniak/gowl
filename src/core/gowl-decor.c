/*
 * gowl-decor.c — Window decoration for nested Wayland sessions
 *
 * Uses libdecor to add a title bar when gowl runs inside another
 * compositor.  The decorated wl_surface is passed to wlroots via
 * wlr_wl_output_create_from_surface(), which renders content to
 * it without creating its own xdg_toplevel.  libdecor manages
 * the xdg shell integration and draws decorations on subsurfaces.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef GOWL_HAVE_LIBDECOR

#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_output.h>
#include <libdecor.h>

#include "gowl-core-private.h"
#include "gowl-decor.h"
#include "gowl-compositor.h"

struct _GowlDecor {
	GowlCompositor         *compositor;
	struct wlr_backend     *wl_backend;        /* Wayland sub-backend */
	struct wl_compositor   *parent_compositor;
	struct libdecor        *ctx;
	struct libdecor_frame  *frame;
	struct wl_surface      *surface;
	struct wlr_output      *output;
	int                     width;
	int                     height;
	gboolean                configured;
};

/* ===== Registry listener (bind parent wl_compositor) ===== */

static void
registry_global(void *data, struct wl_registry *registry,
                uint32_t name, const char *interface, uint32_t version)
{
	GowlDecor *decor = (GowlDecor *)data;
	guint v;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		v = MIN(version, 4);
		decor->parent_compositor =
			(struct wl_compositor *)wl_registry_bind(
				registry, name, &wl_compositor_interface, v);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

/* ===== libdecor context interface ===== */

static void
on_libdecor_error(struct libdecor *context, enum libdecor_error error,
                  const char *message)
{
	(void)context;
	(void)error;
	g_warning("gowl-decor: libdecor error: %s", message);
}

static struct libdecor_interface libdecor_iface = {
	on_libdecor_error,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* ===== Frame callbacks ===== */

/*
 * frame_configure_cb:
 *
 * Called when the parent compositor configures the window (resize,
 * state change).  Commits the decoration state and updates the
 * wlr_output mode so gowl re-arranges clients at the new size.
 */
static void
frame_configure_cb(struct libdecor_frame *frame,
                   struct libdecor_configuration *configuration,
                   void *user_data)
{
	GowlDecor *decor;
	struct libdecor_state *state;
	int width;
	int height;

	decor = (GowlDecor *)user_data;
	width = 0;
	height = 0;

	/* Extract content size; returns false if compositor didn't specify */
	if (!libdecor_configuration_get_content_size(configuration,
	    frame, &width, &height)) {
		/* Use stored size, or a reasonable default for initial configure */
		width = decor->width > 0 ? decor->width : 1280;
		height = decor->height > 0 ? decor->height : 720;
	}

	g_debug("gowl-decor: configure %dx%d (output=%p, prev=%dx%d)",
	        width, height, (void *)decor->output,
	        decor->width, decor->height);

	/* Commit decoration state */
	state = libdecor_state_new(width, height);
	libdecor_frame_commit(frame, state, configuration);
	libdecor_state_free(state);

	/* Propagate resize to the wlr_output */
	if (width > 0 && height > 0) {
		struct wlr_output_state ostate;

		decor->width = width;
		decor->height = height;

		if (decor->output != NULL) {
			wlr_output_state_init(&ostate);
			wlr_output_state_set_custom_mode(&ostate,
				width, height, 0);
			wlr_output_state_set_enabled(&ostate, TRUE);
			wlr_output_commit_state(decor->output, &ostate);
			wlr_output_state_finish(&ostate);
			g_debug("gowl-decor: output mode set to %dx%d",
			        width, height);

			/* The output-layout change listener does not fire for
			 * this surface-backed nested output, so propagate the
			 * new size explicitly: update the monitor geometry and
			 * re-arrange layer-shell surfaces (the gowlbar status
			 * bar) + wallpaper/bar to the new width.  Without this
			 * the bar keeps its old size after a host resize. */
			gowl_compositor_notify_output_resized(decor->compositor,
			                                      decor->output);
		}
	}

	decor->configured = TRUE;
}

/*
 * frame_close_cb:
 *
 * Called when the user clicks the close button in the title bar.
 */
static void
frame_close_cb(struct libdecor_frame *frame, void *user_data)
{
	GowlDecor *decor;

	(void)frame;
	decor = (GowlDecor *)user_data;
	gowl_compositor_quit(decor->compositor);
}

/*
 * frame_commit_cb:
 *
 * Called by libdecor when it needs the application to commit the
 * wl_surface so decoration subsurfaces stay in sync.
 */
static void
frame_commit_cb(struct libdecor_frame *frame, void *user_data)
{
	GowlDecor *decor;

	(void)frame;
	decor = (GowlDecor *)user_data;
	if (decor->surface != NULL)
		wl_surface_commit(decor->surface);
}

/*
 * frame_dismiss_popup_cb:
 *
 * No-op — gowl has no decoration popups.
 */
static void
frame_dismiss_popup_cb(struct libdecor_frame *frame,
                       const char *seat_name, void *user_data)
{
	(void)frame;
	(void)seat_name;
	(void)user_data;
}

static struct libdecor_frame_interface frame_iface = {
	frame_configure_cb,
	frame_close_cb,
	frame_commit_cb,
	frame_dismiss_popup_cb,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* ===== Public API ===== */

GowlDecor *
gowl_decor_new(GowlCompositor *compositor, struct wlr_backend *wl_backend)
{
	GowlDecor *decor;
	struct wl_display *parent;
	struct wl_registry *registry;

	g_return_val_if_fail(compositor != NULL, NULL);
	g_return_val_if_fail(wl_backend != NULL, NULL);

	parent = wlr_wl_backend_get_remote_display(wl_backend);
	if (parent == NULL) {
		g_warning("gowl-decor: no parent Wayland display");
		return NULL;
	}

	decor = g_new0(GowlDecor, 1);
	decor->compositor = compositor;
	decor->wl_backend = wl_backend;

	/* Bind to the parent compositor's wl_compositor global.
	 * Use wl_display_dispatch_pending + flush instead of roundtrip
	 * because wlroots may have already dispatched the registry
	 * events onto the default queue during backend creation. */
	registry = wl_display_get_registry(parent);
	wl_registry_add_listener(registry, &registry_listener, decor);
	wl_display_flush(parent);

	/* Try a non-blocking dispatch first; if globals are already
	 * queued we pick them up without blocking. */
	wl_display_dispatch_pending(parent);

	/* If that wasn't enough, do one blocking dispatch */
	if (decor->parent_compositor == NULL) {
		wl_display_dispatch(parent);
	}
	wl_registry_destroy(registry);

	if (decor->parent_compositor == NULL) {
		g_warning("gowl-decor: failed to bind parent wl_compositor");
		g_free(decor);
		return NULL;
	}

	g_debug("gowl-decor: bound parent wl_compositor");

	/* Create libdecor context.
	 *
	 * Force the cairo decoration plugin.  The GTK plugin calls GTK3
	 * APIs from within frame_configure_cb (which runs on the
	 * compositor thread), but the main thread also uses GTK3 for the
	 * pgtk display backend.  GTK3 is not thread-safe, so concurrent
	 * access causes CSS data-structure corruption (SIGSEGV in
	 * _gtk_css_style_property_get_id).  The cairo plugin avoids GTK
	 * entirely.  On SSD compositors (sway, Hyprland), the parent
	 * draws decorations regardless of plugin choice.
	 *
	 * Also temporarily restore the parent compositor's display name
	 * because the GTK plugin's gtk_init_check() (and possibly other
	 * plugins) connects to $WAYLAND_DISPLAY, which gowl has already
	 * overwritten with its own server socket. */
	{
		const gchar *parent_wd;
		const gchar *our_socket;
		const gchar *old_plugin_dir;
		gchar *tmpdir;
		gchar *link_path;

		parent_wd  = decor->compositor->parent_wl_display;
		our_socket = decor->compositor->socket_name;

		/* Create a temp dir containing only the cairo plugin */
		old_plugin_dir = g_getenv("LIBDECOR_PLUGIN_DIR");
		tmpdir = g_dir_make_tmp("gowl-decor-XXXXXX", NULL);
		if (tmpdir != NULL) {
			static const gchar *cairo_paths[] = {
				"/usr/lib64/libdecor/plugins-1/libdecor-cairo.so",
				"/usr/lib/libdecor/plugins-1/libdecor-cairo.so",
				"/usr/lib/x86_64-linux-gnu/libdecor/plugins-1/libdecor-cairo.so",
				NULL
			};
			const gchar **p;

			link_path = g_build_filename(tmpdir,
				"libdecor-cairo.so", NULL);
			for (p = cairo_paths; *p != NULL; p++) {
				if (g_file_test(*p, G_FILE_TEST_EXISTS)) {
					symlink(*p, link_path);
					g_setenv("LIBDECOR_PLUGIN_DIR",
					         tmpdir, TRUE);
					g_debug("gowl-decor: forcing cairo "
					        "plugin from %s", *p);
					break;
				}
			}
			g_free(link_path);
		}

		if (parent_wd != NULL)
			g_setenv("WAYLAND_DISPLAY", parent_wd, TRUE);

		decor->ctx = libdecor_new(parent, &libdecor_iface);

		if (our_socket != NULL)
			g_setenv("WAYLAND_DISPLAY", our_socket, TRUE);

		/* Restore or unset LIBDECOR_PLUGIN_DIR */
		if (old_plugin_dir != NULL)
			g_setenv("LIBDECOR_PLUGIN_DIR",
			         old_plugin_dir, TRUE);
		else
			g_unsetenv("LIBDECOR_PLUGIN_DIR");

		/* Clean up temp dir (plugin is already loaded) */
		if (tmpdir != NULL) {
			link_path = g_build_filename(tmpdir,
				"libdecor-cairo.so", NULL);
			unlink(link_path);
			g_free(link_path);
			rmdir(tmpdir);
			g_free(tmpdir);
		}
	}

	if (decor->ctx == NULL) {
		g_warning("gowl-decor: failed to create libdecor context");
		wl_compositor_destroy(decor->parent_compositor);
		g_free(decor);
		return NULL;
	}

	g_message("gowl-decor: nested session detected, libdecor initialized");
	return decor;
}

gboolean
gowl_decor_setup(GowlDecor *decor)
{
	struct wl_display *parent;

	g_return_val_if_fail(decor != NULL, FALSE);
	g_return_val_if_fail(decor->compositor != NULL, FALSE);

	/* Create the content surface on the parent compositor */
	decor->surface =
		wl_compositor_create_surface(decor->parent_compositor);
	if (decor->surface == NULL) {
		g_warning("gowl-decor: failed to create wl_surface");
		return FALSE;
	}

	/* Decorate the surface (creates xdg_surface + xdg_toplevel) */
	decor->frame = libdecor_decorate(decor->ctx, decor->surface,
	                                  &frame_iface, decor);
	if (decor->frame == NULL) {
		g_warning("gowl-decor: failed to decorate surface");
		wl_surface_destroy(decor->surface);
		decor->surface = NULL;
		return FALSE;
	}

	/* Set window metadata */
	libdecor_frame_set_title(decor->frame, "CMacs");
	libdecor_frame_set_app_id(decor->frame, "cmacs");
	libdecor_frame_map(decor->frame);

	/* Flush the parent display so the map request is sent.
	 * Do NOT roundtrip here — it can deadlock if wlroots owns
	 * the parent display queue.  The initial configure event
	 * will arrive through the normal event loop. */
	parent = wlr_wl_backend_get_remote_display(decor->wl_backend);
	wl_display_flush(parent);

	g_debug("gowl-decor: frame mapped, awaiting initial configure");

	/* Create a wlr_output that renders to our decorated surface.
	 * Unlike wlr_wl_output_create(), this does NOT create its own
	 * xdg_toplevel — it reuses our libdecor-managed surface. */
	decor->output = wlr_wl_output_create_from_surface(
		decor->wl_backend, decor->surface);
	if (decor->output == NULL) {
		g_warning("gowl-decor: failed to create output from surface");
		libdecor_frame_unref(decor->frame);
		decor->frame = NULL;
		wl_surface_destroy(decor->surface);
		decor->surface = NULL;
		return FALSE;
	}

	g_debug("gowl-decor: wlr_output created from surface: %s",
	        decor->output->name);

	g_message("gowl-decor: decorated output created (%dx%d)",
	          decor->width, decor->height);
	return TRUE;
}

void
gowl_decor_destroy(GowlDecor *decor)
{
	if (decor == NULL)
		return;

	if (decor->frame != NULL) {
		libdecor_frame_unref(decor->frame);
		decor->frame = NULL;
	}

	if (decor->ctx != NULL) {
		libdecor_unref(decor->ctx);
		decor->ctx = NULL;
	}

	/* The surface is owned by the wlr_output — destroying the
	 * output will destroy the surface. */
	decor->surface = NULL;
	decor->output = NULL;

	g_free(decor);
}

gboolean
gowl_decor_owns_output(GowlDecor *decor, struct wlr_output *output)
{
	if (decor == NULL || output == NULL)
		return FALSE;
	return decor->output == output;
}

#endif /* GOWL_HAVE_LIBDECOR */
