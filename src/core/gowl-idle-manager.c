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

#include "gowl-core-private.h"

/**
 * GowlIdleManager:
 *
 * Manages idle detection and idle-inhibit behaviour.  Tracks the
 * idle timeout and current state (active vs idle).  When the idle
 * timeout elapses without user input, the "idle" signal is emitted;
 * when input resumes, the "resume" signal is emitted.  Idle inhibit
 * clients can prevent the transition to idle.
 *
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlIdleManager, gowl_idle_manager, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_IDLE,
	SIGNAL_RESUME,
	N_SIGNALS
};

static guint idle_signals[N_SIGNALS] = { 0, };

static void rearm_timers(GowlIdleManager *self);

/* --- GObject lifecycle --- */

static void
gowl_idle_manager_dispose(GObject *object)
{
	gowl_idle_manager_detach(GOWL_IDLE_MANAGER(object));
	G_OBJECT_CLASS(gowl_idle_manager_parent_class)->dispose(object);
}

static void
gowl_idle_manager_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_idle_manager_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_idle_manager_class_init(GowlIdleManagerClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_idle_manager_dispose;
	object_class->finalize = gowl_idle_manager_finalize;

	/**
	 * GowlIdleManager::idle:
	 * @manager: the #GowlIdleManager that emitted the signal
	 *
	 * Emitted when the idle timeout elapses and the session
	 * transitions to the idle state.
	 */
	idle_signals[SIGNAL_IDLE] =
		g_signal_new("idle",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlIdleManager::resume:
	 * @manager: the #GowlIdleManager that emitted the signal
	 *
	 * Emitted when user input is received after the session was idle,
	 * transitioning back to the active state.
	 */
	idle_signals[SIGNAL_RESUME] =
		g_signal_new("resume",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);
}

static void
gowl_idle_manager_init(GowlIdleManager *self)
{
	self->wlr_idle_notifier        = NULL;
	self->wlr_idle_inhibit_manager = NULL;
	self->timeout_secs             = 300;
	self->state                    = 0;
	self->compositor               = NULL;
	self->idle_timer               = NULL;
	self->dpms_timer               = NULL;
	self->dpms_timeout_secs        = 0;
	self->inhibitors               = NULL;
	self->inhibited                = FALSE;
	wl_list_init(&self->new_inhibitor.link);
}

/* --- Public API --- */

/**
 * gowl_idle_manager_new:
 *
 * Creates a new #GowlIdleManager with a default timeout of 300
 * seconds and in the active state.
 *
 * Returns: (transfer full): a newly allocated #GowlIdleManager
 */
GowlIdleManager *
gowl_idle_manager_new(void)
{
	return (GowlIdleManager *)g_object_new(GOWL_TYPE_IDLE_MANAGER, NULL);
}

/**
 * gowl_idle_manager_get_state:
 * @self: a #GowlIdleManager
 *
 * Returns the current idle state.  0 means active, 1 means idle.
 *
 * Returns: the state value
 */
gint
gowl_idle_manager_get_state(GowlIdleManager *self)
{
	g_return_val_if_fail(GOWL_IS_IDLE_MANAGER(self), 0);

	return self->state;
}

/**
 * gowl_idle_manager_get_timeout:
 * @self: a #GowlIdleManager
 *
 * Returns the idle timeout in seconds.
 *
 * Returns: the timeout in seconds
 */
gint
gowl_idle_manager_get_timeout(GowlIdleManager *self)
{
	g_return_val_if_fail(GOWL_IS_IDLE_MANAGER(self), 300);

	return self->timeout_secs;
}

/**
 * gowl_idle_manager_set_timeout:
 * @self: a #GowlIdleManager
 * @timeout_secs: the new idle timeout in seconds
 *
 * Sets the idle timeout.  A value of 0 disables idle detection.
 */
void
gowl_idle_manager_set_timeout(
	GowlIdleManager *self,
	gint              timeout_secs
){
	g_return_if_fail(GOWL_IS_IDLE_MANAGER(self));

	self->timeout_secs = timeout_secs;
	rearm_timers(self);
}

gint
gowl_idle_manager_get_dpms_timeout(GowlIdleManager *self)
{
	g_return_val_if_fail(GOWL_IS_IDLE_MANAGER(self), 0);

	return self->dpms_timeout_secs;
}

void
gowl_idle_manager_set_dpms_timeout(
	GowlIdleManager *self,
	gint             timeout_secs
){
	g_return_if_fail(GOWL_IS_IDLE_MANAGER(self));

	self->dpms_timeout_secs = MAX(timeout_secs, 0);
	rearm_timers(self);
}

gboolean
gowl_idle_manager_is_inhibited(GowlIdleManager *self)
{
	g_return_val_if_fail(GOWL_IS_IDLE_MANAGER(self), FALSE);

	return self->inhibited;
}

/* -----------------------------------------------------------
 * Timers
 *
 * Two wl_event_loop timers, both restarted by every input event and
 * both parked while an inhibitor is up.  The idle timer only announces
 * (the "idle" signal, which a lock module or an embedder acts on); the
 * dpms timer acts, through the compositor's output-power path, which
 * also remembers that it was the timer so that the next input undoes
 * it.
 * ----------------------------------------------------------- */

static int
on_idle_timer(void *data)
{
	GowlIdleManager *self = (GowlIdleManager *)data;

	if (self->state == 0) {
		self->state = 1;
		g_signal_emit(self, idle_signals[SIGNAL_IDLE], 0);
	}
	return 0;
}

static int
on_dpms_timer(void *data)
{
	GowlIdleManager *self = (GowlIdleManager *)data;
	GowlCompositor *comp = (GowlCompositor *)self->compositor;
	GList *l;
	gboolean any = FALSE;

	if (comp == NULL)
		return 0;

	for (l = comp->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;

		if (m->powered_off || m->wlr_output == NULL
		    || !m->wlr_output->enabled)
			continue;
		gowl_compositor_set_monitor_powered(comp, m, FALSE);
		any = TRUE;
	}
	if (any)
		comp->outputs_off_by_idle = TRUE;
	return 0;
}

/*
 * (Re)start whichever timers have a timeout, from now.  Called on
 * every input event, so it must stay cheap: two timer updates.
 */
static void
rearm_timers(GowlIdleManager *self)
{
	if (self->idle_timer != NULL)
		wl_event_source_timer_update(self->idle_timer,
			(!self->inhibited && self->timeout_secs > 0)
			? self->timeout_secs * 1000 : 0);
	if (self->dpms_timer != NULL)
		wl_event_source_timer_update(self->dpms_timer,
			(!self->inhibited && self->dpms_timeout_secs > 0)
			? self->dpms_timeout_secs * 1000 : 0);
}

void
gowl_idle_manager_note_activity(GowlIdleManager *self)
{
	GowlCompositor *comp;

	g_return_if_fail(GOWL_IS_IDLE_MANAGER(self));

	comp = (GowlCompositor *)self->compositor;
	if (self->wlr_idle_notifier != NULL && comp != NULL
	    && comp->wlr_seat != NULL)
		wlr_idle_notifier_v1_notify_activity(
			(struct wlr_idle_notifier_v1 *)self->wlr_idle_notifier,
			comp->wlr_seat);

	if (comp != NULL)
		gowl_compositor_wake_outputs(comp);

	if (self->state != 0) {
		self->state = 0;
		g_signal_emit(self, idle_signals[SIGNAL_RESUME], 0);
	}
	rearm_timers(self);
}

/* -----------------------------------------------------------
 * idle-inhibit-v1
 *
 * An inhibitor counts while its surface is mapped and on a visible
 * tag of its monitor: a paused video on another tag must not keep the
 * screen on.  That makes visibility part of the answer, so the
 * compositor asks for a re-check from arrange() as well as on the
 * inhibitor's own creation and destruction (dwl's checkidleinhibitor).
 * ----------------------------------------------------------- */

typedef struct {
	GowlIdleManager               *manager;
	struct wlr_idle_inhibitor_v1  *inhibitor;
	struct wl_listener             destroy;
} GowlIdleInhibitor;

static gboolean
inhibitor_counts(GowlCompositor *comp, struct wlr_idle_inhibitor_v1 *inh)
{
	struct wlr_surface *surface;
	struct wlr_scene_tree *tree;
	GList *l;

	if (inh->surface == NULL)
		return FALSE;
	surface = wlr_surface_get_root_surface(inh->surface);
	if (surface == NULL || !surface->mapped)
		return FALSE;

	/* A layer surface (a lock screen, an OSD) is visible whenever it
	 * is mapped; a client is visible when its tags are. */
	tree = (struct wlr_scene_tree *)surface->data;
	for (l = comp->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (c->scene != tree)
			continue;
		if (c->isembedded)
			return TRUE;
		return c->mon != NULL && c->mon->wlr_output != NULL
		       && c->mon->wlr_output->enabled
		       && (c->isoverlay ? c->overlay_visible
		           : (c->tags & c->mon->tagset[c->mon->seltags]) != 0);
	}
	return TRUE;
}

void
gowl_idle_manager_check_inhibitors(GowlIdleManager *self)
{
	GowlCompositor *comp;
	GList *l;
	gboolean inhibited = FALSE;

	g_return_if_fail(GOWL_IS_IDLE_MANAGER(self));

	comp = (GowlCompositor *)self->compositor;
	if (comp == NULL)
		return;

	for (l = self->inhibitors; l != NULL && !inhibited; l = l->next) {
		GowlIdleInhibitor *e = (GowlIdleInhibitor *)l->data;

		inhibited = inhibitor_counts(comp, e->inhibitor);
	}

	if (inhibited == self->inhibited)
		return;
	self->inhibited = inhibited;
	g_debug("idle: %s", inhibited ? "inhibited" : "not inhibited");

	if (self->wlr_idle_notifier != NULL)
		wlr_idle_notifier_v1_set_inhibited(
			(struct wlr_idle_notifier_v1 *)self->wlr_idle_notifier,
			inhibited);
	/* Coming out of inhibition starts the clocks from now: the last
	 * keypress may have been two hours ago. */
	rearm_timers(self);
}

static void
on_inhibitor_destroy(struct wl_listener *listener, void *data)
{
	GowlIdleInhibitor *e = wl_container_of(listener, e, destroy);
	GowlIdleManager *self = e->manager;
	(void)data;

	wl_list_remove(&e->destroy.link);
	self->inhibitors = g_list_remove(self->inhibitors, e);
	g_free(e);
	gowl_idle_manager_check_inhibitors(self);
}

static void
on_new_inhibitor(struct wl_listener *listener, void *data)
{
	GowlIdleManager *self = wl_container_of(listener, self, new_inhibitor);
	struct wlr_idle_inhibitor_v1 *inh = data;
	GowlIdleInhibitor *e;

	e = g_new0(GowlIdleInhibitor, 1);
	e->manager = self;
	e->inhibitor = inh;
	e->destroy.notify = on_inhibitor_destroy;
	wl_signal_add(&inh->events.destroy, &e->destroy);
	self->inhibitors = g_list_prepend(self->inhibitors, e);
	gowl_idle_manager_check_inhibitors(self);
}

/**
 * gowl_idle_manager_attach:
 * @self: a #GowlIdleManager
 * @comp: the compositor, whose display, event loop and outputs this
 *   manager drives from now on
 *
 * Creates the idle-inhibit global and both timers.  Called once from
 * gowl_compositor_start(); the compositor owns the manager and its
 * dispose detaches.
 */
void
gowl_idle_manager_attach(
	GowlIdleManager *self,
	GowlCompositor  *comp
){
	struct wlr_idle_inhibit_manager_v1 *mgr;

	g_return_if_fail(GOWL_IS_IDLE_MANAGER(self));
	g_return_if_fail(comp != NULL && comp->wl_display != NULL);

	self->compositor = comp;
	self->wlr_idle_notifier = comp->idle_notifier;

	mgr = wlr_idle_inhibit_v1_create(comp->wl_display);
	self->wlr_idle_inhibit_manager = mgr;
	comp->idle_inhibit_mgr = mgr;
	if (mgr != NULL) {
		self->new_inhibitor.notify = on_new_inhibitor;
		wl_signal_add(&mgr->events.new_inhibitor, &self->new_inhibitor);
	}

	if (comp->event_loop != NULL) {
		self->idle_timer = wl_event_loop_add_timer(comp->event_loop,
		                                           on_idle_timer, self);
		self->dpms_timer = wl_event_loop_add_timer(comp->event_loop,
		                                           on_dpms_timer, self);
	}

	if (comp->config != NULL) {
		self->timeout_secs =
			gowl_config_get_idle_timeout(comp->config);
		self->dpms_timeout_secs =
			gowl_config_get_dpms_timeout(comp->config);
	}
	rearm_timers(self);
}

/**
 * gowl_idle_manager_detach:
 * @self: a #GowlIdleManager
 *
 * Removes the timers and every listener.  wlroots aborts on a listener
 * still attached when its object is destroyed, so this runs before the
 * display goes -- from dispose, and again harmlessly if called twice.
 */
void
gowl_idle_manager_detach(GowlIdleManager *self)
{
	GList *l;

	g_return_if_fail(GOWL_IS_IDLE_MANAGER(self));

	if (self->idle_timer != NULL) {
		wl_event_source_remove(self->idle_timer);
		self->idle_timer = NULL;
	}
	if (self->dpms_timer != NULL) {
		wl_event_source_remove(self->dpms_timer);
		self->dpms_timer = NULL;
	}
	for (l = self->inhibitors; l != NULL; l = l->next) {
		GowlIdleInhibitor *e = (GowlIdleInhibitor *)l->data;

		wl_list_remove(&e->destroy.link);
		g_free(e);
	}
	g_clear_pointer(&self->inhibitors, g_list_free);
	if (!wl_list_empty(&self->new_inhibitor.link)) {
		wl_list_remove(&self->new_inhibitor.link);
		wl_list_init(&self->new_inhibitor.link);
	}
	self->compositor = NULL;
	self->wlr_idle_inhibit_manager = NULL;
}
