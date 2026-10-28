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

/**
 * SECTION:bar-plugins-devservices
 * @title: The devservices widget
 * @short_description: start, stop and watch a project's dev services
 *
 * A project's development stack -- a database in a container, a
 * backend, a frontend -- declared once in
 * `~/.config/gowl/devservices/<project>.yaml' and then brought up,
 * watched and brought down from the bar.  By default the widget follows
 * the focused window: the project is the one whose root holds the
 * working directory of the shell that window is showing.
 *
 * Everything that waits runs on a worker.  An action is not one long
 * job but a state machine advanced one STEP per worker run -- one step
 * starts a wave of services, later steps check on them -- because the
 * bar's worker pool is four threads shared by every widget, and an
 * `Up' that held one for the minute a container takes to pull would
 * starve the clock.  While something is in flight the widget asks to
 * be polled every second.
 *
 * Processes are started in their own session (so their own process
 * group), logged to `$XDG_STATE_HOME/gowl/devservices/', and recorded
 * with their /proc start time in a state file, so a restarted gowl can
 * still see and stop what it started -- and never signals a pid that
 * has since been reused by something else.  See
 * docs/bar-widgets/devservices.org.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>	/* WIFEXITED and friends: POSIX puts them here; glibc also has them in stdlib.h */

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"

#include "bar-internal.h"
#include "bar-devservices-model.h"

/* The widget's glyph: a server rack (Font Awesome f233). */
#define DS_ICON "\xef\x88\xb3"

/* A destructive click stays armed this long. */
#define DS_ARM_US (5 * G_USEC_PER_SEC)

/* ----------------------------------------------------------------
 * State
 * ---------------------------------------------------------------- */

/* One service's live state.  Written by the worker under the lock;
   the panel reads it under the same lock. */
typedef struct {
	BarDevservicesState state;
	gint                unhealthy;
	gboolean            healthy;
	/* Someone else is listening on the service's port. */
	gint                foreign_pid;
	gchar              *foreign_comm;
	gchar              *last_line;
	gchar              *error;
	/* The process this widget started, or NULL. */
	BarDevservicesRun  *run;
	/* A `podman compose up/down' in flight, not persisted. */
	BarDevservicesRun  *compose;
	gboolean            compose_up;
	gint                exit_status;
	gboolean            have_exit;
	/* Started by the action in flight and still inside its `wait':
	   slow to answer is starting, not stuck, until that runs out. */
	gboolean            waiting;
	/* When this widget started it (monotonic us), 0 if adopted. */
	gint64              spawned_at;
} DsSvc;

typedef struct {
	BarDevservicesRun *run;
	gchar             *result;
} DsTask;

typedef struct {
	BarDevservicesStack *stack;
	GPtrArray           *svcs;   /* DsSvc*, parallel to stack->services */
	GPtrArray           *tasks;  /* DsTask*, parallel to stack->tasks */
	gboolean             runs_changed;
} DsProject;

typedef enum {
	DS_OP_UP,
	DS_OP_DOWN,
	DS_OP_RESTART,
	DS_OP_TASK
} DsOpKind;

/* An action in flight: a list of waves walked one step at a time. */
typedef struct {
	DsOpKind   kind;
	gchar     *project;
	gchar     *task;
	gboolean   stopping;    /* in the down half (DOWN, RESTART) */
	GPtrArray *waves;       /* GArray of guint, in walking order */
	guint      wave;
	gboolean   wave_started;
	gboolean   killed;
	gint64     deadline;    /* monotonic us */
} DsOp;

typedef struct {
	DsOpKind kind;
	gchar   *project;
	gchar   *task;
} DsRequest;

typedef struct {
	/* Guards every field the panel or the bar label reads.  Held
	   only for copying and swapping, never across a subprocess. */
	GMutex     lock;
	/* Serialises worker steps: the async poll and work queued from a
	   click may run on two pool threads at once.  Taken only on
	   workers, never on the compositor thread. */
	GMutex     work;

	GPtrArray *projects;      /* DsProject* */
	GPtrArray *load_errors;   /* gchar* */
	gchar     *dir_sig;
	gchar     *config_dir;    /* as last loaded, for messages */
	gboolean   force_reload;

	/* The focused window's pid, written by the poll on the
	   compositor thread and read by the worker. */
	gint       focus_pid;
	gchar     *auto_key;      /* the project the focus is in */
	gchar     *pinned;        /* a project picked in the panel */

	DsOp      *op;
	DsRequest *pending;
	gchar     *progress;
	gchar     *last_error;

	GPtrArray *toasts;        /* "urgency\tsummary\tbody" */

	gboolean   scan_requested;
	gboolean   scanning;
	gboolean   scanned;
	GPtrArray *candidates;    /* BarDevservicesCandidate* */
	GPtrArray *declared;      /* gchar* keys written this session */
	gchar     *declare_key;
	gchar     *takeover;      /* service id to take the port for */
	gchar     *stop_task;     /* task id to stop */

	gchar     *armed;
	gint64     armed_at;

	gboolean   podman_missing;
	gboolean   curl_missing;
	gboolean   needs_podman;
	gboolean   needs_curl;
} DevData;

static void
ds_svc_free(gpointer data)
{
	DsSvc *svc = data;

	if (svc == NULL)
		return;
	g_free(svc->foreign_comm);
	g_free(svc->last_line);
	g_free(svc->error);
	bar_devservices_run_free(svc->run);
	bar_devservices_run_free(svc->compose);
	g_free(svc);
}

static void
ds_task_free(gpointer data)
{
	DsTask *task = data;

	if (task == NULL)
		return;
	bar_devservices_run_free(task->run);
	g_free(task->result);
	g_free(task);
}

static void
ds_project_free(gpointer data)
{
	DsProject *proj = data;

	if (proj == NULL)
		return;
	bar_devservices_stack_free(proj->stack);
	g_ptr_array_unref(proj->svcs);
	g_ptr_array_unref(proj->tasks);
	g_free(proj);
}

static void
ds_op_free(DsOp *op)
{
	if (op == NULL)
		return;
	g_free(op->project);
	g_free(op->task);
	if (op->waves != NULL)
		g_ptr_array_unref(op->waves);
	g_free(op);
}

static void
ds_request_free(DsRequest *req)
{
	if (req == NULL)
		return;
	g_free(req->project);
	g_free(req->task);
	g_free(req);
}

static gpointer
ds_create(GowlBarPlugin *plugin)
{
	DevData *dd;

	(void)plugin;
	dd = g_new0(DevData, 1);
	g_mutex_init(&dd->lock);
	g_mutex_init(&dd->work);
	dd->projects = g_ptr_array_new_with_free_func(ds_project_free);
	dd->load_errors = g_ptr_array_new_with_free_func(g_free);
	dd->toasts = g_ptr_array_new_with_free_func(g_free);
	dd->candidates = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_candidate_free);
	dd->declared = g_ptr_array_new_with_free_func(g_free);
	dd->force_reload = TRUE;
	return dd;
}

/*
 * Services are deliberately NOT stopped here.  They are the user's dev
 * servers, a gowl restart is not a request to kill them, and the state
 * file is what lets the next gowl pick them up again.
 */
static void
ds_destroy(GowlBarPlugin *plugin, gpointer data)
{
	DevData *dd = data;

	(void)plugin;
	if (dd == NULL)
		return;
	g_ptr_array_unref(dd->projects);
	g_ptr_array_unref(dd->load_errors);
	g_ptr_array_unref(dd->toasts);
	g_ptr_array_unref(dd->candidates);
	g_ptr_array_unref(dd->declared);
	g_free(dd->dir_sig);
	g_free(dd->config_dir);
	g_free(dd->auto_key);
	g_free(dd->pinned);
	ds_op_free(dd->op);
	ds_request_free(dd->pending);
	g_free(dd->progress);
	g_free(dd->last_error);
	g_free(dd->declare_key);
	g_free(dd->takeover);
	g_free(dd->stop_task);
	g_free(dd->armed);
	g_mutex_clear(&dd->lock);
	g_mutex_clear(&dd->work);
	g_free(dd);
}

/* Replace a string slot; the caller holds the lock. */
static void
ds_set(gchar **slot, const gchar *value)
{
	if (g_strcmp0(*slot, value) == 0)
		return;
	g_free(*slot);
	*slot = g_strdup(value);
}

/* ----------------------------------------------------------------
 * Settings
 * ---------------------------------------------------------------- */

/* A setting, or @fallback when unset or empty, copied: a worker must
   not hold the borrowed pointer across a subprocess. */
static gchar *
ds_setting(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, key);

	return g_strdup((v != NULL && v[0] != '\0') ? v : fallback);
}

static gchar *
ds_config_dir(GowlBarPlugin *plugin)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, "config-dir");

	if (v != NULL && v[0] != '\0')
		return bar_expand_tilde(v);
	return g_build_filename(g_get_user_config_dir(), "gowl", "devservices",
	                        NULL);
}

static gchar *
ds_state_dir(GowlBarPlugin *plugin)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, "state-dir");

	if (v != NULL && v[0] != '\0')
		return bar_expand_tilde(v);
	return g_build_filename(g_get_user_state_dir(), "gowl", "devservices",
	                        NULL);
}

static gint
ds_setting_int(GowlBarPlugin *plugin, const gchar *key, gint fallback,
               gint lo, gint hi)
{
	gint v = gowl_bar_plugin_get_setting_int(plugin, key, fallback);

	return CLAMP(v, lo, hi);
}

/* `final' (the default), `all' or `off'. */
static gboolean
ds_notify_wanted(GowlBarPlugin *plugin, gboolean final)
{
	const gchar *mode = gowl_bar_plugin_get_setting(plugin, "notify");

	if (g_strcmp0(mode, "off") == 0)
		return FALSE;
	if (g_strcmp0(mode, "all") == 0)
		return TRUE;
	return final;
}

/*
 * Queue a toast for the compositor thread.  The worker never raises
 * one itself: the toast stack belongs to the bar's own thread, and the
 * poll that runs there next drains this list.
 */
static void
ds_toast(GowlBarPlugin *plugin, DevData *dd, GowlBarToastUrgency urgency,
         gboolean final, const gchar *summary, const gchar *body)
{
	g_autofree gchar *s = NULL;
	g_autofree gchar *b = NULL;

	if (!ds_notify_wanted(plugin, final))
		return;
	s = g_utf8_make_valid(summary, -1);
	b = g_utf8_make_valid((body != NULL) ? body : "", -1);
	g_strdelimit(s, "\t", ' ');
	g_strdelimit(b, "\t", ' ');
	g_mutex_lock(&dd->lock);
	if (dd->toasts->len < 16)
		g_ptr_array_add(dd->toasts, g_strdup_printf("%d\t%s\t%s",
		                (gint)urgency, s, b));
	g_mutex_unlock(&dd->lock);
}

static gchar *
ds_log_path(const gchar *state_dir, const gchar *key, const gchar *id)
{
	g_autofree gchar *name = g_strdup_printf("%s-%s.log", key, id);

	return g_build_filename(state_dir, name, NULL);
}

static gchar *
ds_task_log_path(const gchar *state_dir, const gchar *key, const gchar *id)
{
	g_autofree gchar *name = g_strdup_printf("%s-task-%s.log", key, id);

	return g_build_filename(state_dir, name, NULL);
}

static gchar *
ds_state_path(const gchar *state_dir, const gchar *key)
{
	g_autofree gchar *name = g_strdup_printf("%s.state", key);

	return g_build_filename(state_dir, name, NULL);
}

/* ----------------------------------------------------------------
 * Which project
 * ---------------------------------------------------------------- */

static DsProject *
ds_find_project(DevData *dd, const gchar *key)
{
	guint i;

	if (key == NULL)
		return NULL;
	for (i = 0; i < dd->projects->len; i++) {
		DsProject *proj = g_ptr_array_index(dd->projects, i);

		if (strcmp(proj->stack->key, key) == 0 ||
		    strcmp(proj->stack->name, key) == 0)
			return proj;
	}
	return NULL;
}

/*
 * ds_current:
 *
 * The project the bar and the panel are about: a panel pin first, then
 * the `fixed' project, then (in `auto') the one the focus is in.  In
 * `all' mode with nothing pinned there is none -- the bar aggregates.
 * Called with the lock held, or from the worker.
 */
static DsProject *
ds_current(GowlBarPlugin *plugin, DevData *dd)
{
	const gchar *mode = gowl_bar_plugin_get_setting(plugin, "mode");
	DsProject *proj;

	proj = ds_find_project(dd, dd->pinned);
	if (proj != NULL)
		return proj;
	if (g_strcmp0(mode, "fixed") == 0)
		return ds_find_project(dd,
			gowl_bar_plugin_get_setting(plugin, "project"));
	if (g_strcmp0(mode, "all") == 0)
		return NULL;
	return ds_find_project(dd, dd->auto_key);
}

static gboolean
ds_mode_is(GowlBarPlugin *plugin, const gchar *mode)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, "mode");

	if (v == NULL || v[0] == '\0')
		v = "auto";
	return strcmp(v, mode) == 0;
}

/* ----------------------------------------------------------------
 * The bar label
 * ---------------------------------------------------------------- */

/* Gather the states of @proj (or of every project) into @states. */
static void
ds_collect_states(DevData *dd, DsProject *proj, GArray *states)
{
	guint i, j;

	for (i = 0; i < dd->projects->len; i++) {
		DsProject *p = g_ptr_array_index(dd->projects, i);

		if (proj != NULL && p != proj)
			continue;
		for (j = 0; j < p->svcs->len; j++) {
			DsSvc *svc = g_ptr_array_index(p->svcs, j);

			g_array_append_val(states, svc->state);
		}
	}
}

static void
ds_apply_color(GowlBarPlugin *plugin, BarDevservicesAgg agg)
{
	switch (agg) {
	case BAR_DEVSERVICES_AGG_ERROR:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-error"),
			GOWL_BAR_COLOR_RED);
		break;
	case BAR_DEVSERVICES_AGG_WORKING:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-working"),
			GOWL_BAR_COLOR_YELLOW);
		break;
	case BAR_DEVSERVICES_AGG_OK:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-ok"),
			GOWL_BAR_COLOR_GREEN);
		break;
	case BAR_DEVSERVICES_AGG_OFF:
	default:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-off"),
			GOWL_BAR_COLOR_SUBTEXT);
		break;
	}
}

/*
 * ds_apply_label:
 *
 * Label, colour and tooltip from the cached state.  Only setters, so
 * it is safe from the worker as well as the poll; both call it, the
 * worker so a finished step shows without waiting for the next tick.
 */
static void
ds_apply_label(GowlBarPlugin *plugin, DevData *dd)
{
	g_autoptr(GArray) states = NULL;
	GString *tip;
	DsProject *proj;
	BarDevservicesAgg agg;
	guint up = 0, i;
	gboolean busy;
	g_autofree gchar *label = NULL;
	const gchar *icon;

	icon = gowl_bar_plugin_get_setting(plugin, "icon");
	gowl_bar_plugin_set_icon(plugin, (icon != NULL && icon[0] != '\0')
	                                 ? icon : DS_ICON);

	states = g_array_new(FALSE, FALSE, sizeof(BarDevservicesState));
	tip = g_string_new(NULL);

	g_mutex_lock(&dd->lock);
	proj = ds_current(plugin, dd);
	busy = (dd->op != NULL || dd->pending != NULL);

	if (dd->projects->len == 0) {
		g_mutex_unlock(&dd->lock);
		gowl_bar_plugin_set_label(plugin, NULL);
		ds_apply_color(plugin, BAR_DEVSERVICES_AGG_OFF);
		g_string_append(tip, "Dev services: no stacks declared yet");
		gowl_bar_plugin_set_tooltip(plugin, tip->str);
		g_string_free(tip, TRUE);
		return;
	}

	if (proj == NULL && !ds_mode_is(plugin, "all")) {
		g_mutex_unlock(&dd->lock);
		gowl_bar_plugin_set_label(plugin, NULL);
		ds_apply_color(plugin, busy ? BAR_DEVSERVICES_AGG_WORKING
		                            : BAR_DEVSERVICES_AGG_OFF);
		g_string_append(tip, "Dev services: the focused window is not "
		                "in a declared project");
		gowl_bar_plugin_set_tooltip(plugin, tip->str);
		g_string_free(tip, TRUE);
		return;
	}

	ds_collect_states(dd, proj, states);
	agg = bar_devservices_aggregate((const BarDevservicesState *)
	                                states->data, states->len, busy, &up);
	label = g_strdup_printf("%u/%u", up, states->len);

	if (proj != NULL) {
		g_string_append_printf(tip, "%s", proj->stack->name);
		for (i = 0; i < proj->svcs->len; i++) {
			DsSvc *svc = g_ptr_array_index(proj->svcs, i);
			BarDevservicesService *def;

			def = g_ptr_array_index(proj->stack->services, i);
			g_string_append_printf(tip, "\n%s: %s", def->label,
			                       bar_devservices_state_name(svc->state));
		}
	} else {
		g_string_append(tip, "Dev services");
		for (i = 0; i < dd->projects->len; i++) {
			DsProject *p = g_ptr_array_index(dd->projects, i);
			g_autoptr(GArray) ps = g_array_new(FALSE, FALSE,
				sizeof(BarDevservicesState));
			guint pup = 0;

			ds_collect_states(dd, p, ps);
			bar_devservices_aggregate((const BarDevservicesState *)
			                          ps->data, ps->len, FALSE, &pup);
			g_string_append_printf(tip, "\n%s: %u/%u up",
			                       p->stack->name, pup, ps->len);
		}
	}
	if (dd->podman_missing && dd->needs_podman)
		g_string_append(tip, "\npodman is not installed (Fedora: "
		                "podman, podman-compose)");
	if (dd->curl_missing && dd->needs_curl)
		g_string_append(tip, "\ncurl is not installed (Fedora: curl)");
	g_mutex_unlock(&dd->lock);

	gowl_bar_plugin_set_label(plugin, label);
	ds_apply_color(plugin, agg);
	gowl_bar_plugin_set_tooltip(plugin, tip->str);
	g_string_free(tip, TRUE);
}

/* ----------------------------------------------------------------
 * Loading stacks (worker)
 * ---------------------------------------------------------------- */

/*
 * ds_adopt:
 * @proj: a freshly built project
 * @state_dir: where its state file lives
 *
 * Attach the runs a previous gowl (or this one, before a reload)
 * recorded.  The next refresh checks each against /proc and drops the
 * ones that are gone or whose pid now belongs to someone else.
 */
static void
ds_adopt(DsProject *proj, const gchar *state_dir)
{
	g_autofree gchar *path = ds_state_path(state_dir, proj->stack->key);
	g_autoptr(GPtrArray) runs = bar_devservices_state_load(path);
	guint i;

	for (i = 0; i < runs->len; i++) {
		BarDevservicesRun *run = g_ptr_array_index(runs, i);
		gint idx;

		if (g_str_has_prefix(run->id, "task:")) {
			guint t;

			for (t = 0; t < proj->stack->tasks->len; t++) {
				BarDevservicesTask *def;
				DsTask *task;

				def = g_ptr_array_index(proj->stack->tasks, t);
				task = g_ptr_array_index(proj->tasks, t);
				if (strcmp(def->id, run->id + 5) == 0 &&
				    task->run == NULL) {
					task->run = g_ptr_array_steal_index(runs, i);
					i--;
					break;
				}
			}
			continue;
		}
		idx = bar_devservices_stack_find_service(proj->stack, run->id);
		if (idx >= 0) {
			DsSvc *svc = g_ptr_array_index(proj->svcs, idx);

			if (svc->run == NULL) {
				svc->run = g_ptr_array_steal_index(runs, i);
				i--;
			}
		}
	}
}

static DsProject *
ds_project_new(BarDevservicesStack *stack)
{
	DsProject *proj;
	guint i;

	proj = g_new0(DsProject, 1);
	proj->stack = stack;
	proj->svcs = g_ptr_array_new_with_free_func(ds_svc_free);
	proj->tasks = g_ptr_array_new_with_free_func(ds_task_free);
	for (i = 0; i < stack->services->len; i++)
		g_ptr_array_add(proj->svcs, g_new0(DsSvc, 1));
	for (i = 0; i < stack->tasks->len; i++)
		g_ptr_array_add(proj->tasks, g_new0(DsTask, 1));
	return proj;
}

/*
 * ds_reload:
 *
 * Re-read the stack directory when it changed.  Live state moves from
 * the old project to the new one by key and service id, so editing a
 * label while a server runs neither loses it nor restarts it.
 */
static void
ds_reload(GowlBarPlugin *plugin, DevData *dd, const gchar *state_dir)
{
	g_autofree gchar *dir = ds_config_dir(plugin);
	g_autofree gchar *sig = bar_devservices_dir_signature(dir);
	g_autoptr(GPtrArray) errors = NULL;
	g_autoptr(GPtrArray) stacks = NULL;
	GPtrArray *projects;
	guint i, j;
	gboolean needs_podman = FALSE, needs_curl = FALSE;

	if (!dd->force_reload && g_strcmp0(sig, dd->dir_sig) == 0 &&
	    g_strcmp0(dir, dd->config_dir) == 0)
		return;

	errors = g_ptr_array_new_with_free_func(g_free);
	stacks = bar_devservices_load_dir(dir, errors);
	projects = g_ptr_array_new_with_free_func(ds_project_free);

	while (stacks->len > 0) {
		BarDevservicesStack *stack = g_ptr_array_steal_index(stacks, 0);
		DsProject *proj = ds_project_new(stack);
		DsProject *old = ds_find_project(dd, stack->key);

		for (j = 0; j < stack->services->len; j++) {
			BarDevservicesService *def;

			def = g_ptr_array_index(stack->services, j);
			if (def->kind == BAR_DEVSERVICES_KIND_PODMAN)
				needs_podman = TRUE;
			if (def->health != NULL &&
			    g_str_has_prefix(def->health, "http"))
				needs_curl = TRUE;
		}

		if (old == NULL) {
			ds_adopt(proj, state_dir);
		} else {
			/* Same project: carry each service's live state
			   across by id.  Done on the worker, which is the
			   only writer, so reading `old' needs no lock. */
			for (j = 0; j < stack->services->len; j++) {
				BarDevservicesService *def;
				gint k;

				def = g_ptr_array_index(stack->services, j);
				k = bar_devservices_stack_find_service(old->stack,
				                                       def->id);
				if (k >= 0) {
					DsSvc *tmp = g_ptr_array_index(proj->svcs, j);

					proj->svcs->pdata[j] = old->svcs->pdata[k];
					old->svcs->pdata[k] = tmp;
				}
			}
			for (j = 0; j < stack->tasks->len; j++) {
				BarDevservicesTask *def;
				BarDevservicesTask *od;

				def = g_ptr_array_index(stack->tasks, j);
				od = bar_devservices_stack_find_task(old->stack,
				                                     def->id);
				if (od != NULL) {
					guint k;

					for (k = 0; k < old->stack->tasks->len; k++) {
						if (g_ptr_array_index(old->stack->tasks,
						                      k) == od) {
							DsTask *tmp;

							tmp = g_ptr_array_index(proj->tasks, j);
							proj->tasks->pdata[j] =
								old->tasks->pdata[k];
							old->tasks->pdata[k] = tmp;
						}
					}
				}
			}
		}
		g_ptr_array_add(projects, proj);
	}

	g_mutex_lock(&dd->lock);
	g_ptr_array_unref(dd->projects);
	dd->projects = projects;
	g_ptr_array_set_size(dd->load_errors, 0);
	for (i = 0; i < errors->len; i++)
		g_ptr_array_add(dd->load_errors,
		                g_utf8_make_valid(g_ptr_array_index(errors, i),
		                                  -1));
	g_free(dd->dir_sig);
	dd->dir_sig = g_steal_pointer(&sig);
	g_free(dd->config_dir);
	dd->config_dir = g_strdup(dir);
	dd->force_reload = FALSE;
	dd->needs_podman = needs_podman;
	dd->needs_curl = needs_curl;
	g_mutex_unlock(&dd->lock);
}

/* ----------------------------------------------------------------
 * Refreshing status (worker)
 * ---------------------------------------------------------------- */

/* Settings a worker step needs, copied once per step. */
typedef struct {
	gchar *state_dir;
	gchar *podman;
	gchar *compose;
	gchar *curl;
	gchar *open_cmd;
	gint   stuck_after;
	gint   stop_grace;
	gboolean have_podman;
	gboolean have_curl;
} DsCtx;

static void
ds_ctx_clear(DsCtx *ctx)
{
	g_free(ctx->state_dir);
	g_free(ctx->podman);
	g_free(ctx->compose);
	g_free(ctx->curl);
	g_free(ctx->open_cmd);
}

/* The first word of a command setting, for a PATH check. */
static gboolean
ds_have(const gchar *command)
{
	g_auto(GStrv) argv = NULL;

	if (command == NULL || !g_shell_parse_argv(command, NULL, &argv, NULL))
		return FALSE;
	if (g_path_is_absolute(argv[0]))
		return g_file_test(argv[0], G_FILE_TEST_IS_EXECUTABLE);
	return bar_have_command(argv[0]);
}

static gboolean
ds_url_healthy(const DsCtx *ctx, const gchar *url)
{
	g_autofree gchar *out = NULL;
	const gchar *argv[10];

	/* -f fails on an HTTP error status; -o /dev/null with -w gives
	   bar_run_argv some output on success, which it needs to tell
	   success from failure. */
	argv[0] = ctx->curl;
	argv[1] = "-fsS";
	argv[2] = "--max-time";
	argv[3] = "2";
	argv[4] = "-o";
	argv[5] = "/dev/null";
	argv[6] = "-w";
	argv[7] = "%{http_code}";
	argv[8] = url;
	argv[9] = NULL;
	out = bar_run_argv(argv);
	return out != NULL;
}

/*
 * ds_healthy:
 *
 * The service's health check.  A URL needs curl; without it the check
 * falls back to the port, so a missing package degrades the answer
 * rather than painting every service stuck.
 */
static gboolean
ds_healthy(const DsCtx *ctx, const BarDevservicesService *def,
           const DsSvc *svc, GHashTable *listening)
{
	if (def->health != NULL && g_str_has_prefix(def->health, "http") &&
	    ctx->have_curl)
		return ds_url_healthy(ctx, def->health);
	if (def->health != NULL && strcmp(def->health, "port") == 0)
		return bar_devservices_port_connects(def->port, 1000);
	if (def->port > 0)
		return g_hash_table_contains(listening,
		                             GUINT_TO_POINTER((guint)def->port)) ||
		       bar_devservices_port_connects(def->port, 500);
	/* Nothing to ask, so "running" is all there is -- but not the
	   instant it was forked: a command that dies on a bad flag must
	   fail the Up, not pass it in the millisecond before it exits. */
	return svc->spawned_at == 0 ||
	       g_get_monotonic_time() - svc->spawned_at >= G_USEC_PER_SEC;
}

static gchar *
ds_proc_comm(gint pid)
{
	g_autofree gchar *path = NULL;
	gchar *comm = NULL;

	path = g_strdup_printf("/proc/%d/comm", pid);
	if (!g_file_get_contents(path, &comm, NULL, NULL))
		return NULL;
	g_strstrip(comm);
	return comm;
}

/*
 * ds_refresh_project:
 *
 * One poll of every service in @proj: reap what exited, check
 * liveness against the recorded start time, run the health checks,
 * notice a stranger on our port, and step the status machine.  The
 * blocking parts run without the lock; each service's results are
 * swapped in under it.
 */
static void
ds_refresh_project(GowlBarPlugin *plugin, DevData *dd, DsProject *proj,
                   const DsCtx *ctx, const gchar *ps_json,
                   GHashTable *listening)
{
	guint i;

	for (i = 0; i < proj->svcs->len; i++) {
		DsSvc *svc = g_ptr_array_index(proj->svcs, i);
		BarDevservicesService *def;
		g_autofree gchar *log = NULL;
		g_autofree gchar *line = NULL;
		g_autofree gchar *error = NULL;
		g_autofree gchar *foreign_comm = NULL;
		BarDevservicesState prev, next;
		gboolean running = FALSE, healthy = FALSE, drop_run = FALSE;
		gboolean compose_done = FALSE, compose_ok = TRUE;
		gint foreign = 0, status = 0;

		def = g_ptr_array_index(proj->stack->services, i);
		log = ds_log_path(ctx->state_dir, proj->stack->key, def->id);
		bar_devservices_log_rotate(log, BAR_DEVSERVICES_LOG_MAX);
		line = bar_devservices_log_tail(log);

		/* A compose up/down child: reap it when it finishes. */
		if (svc->compose != NULL) {
			if (bar_devservices_reap(svc->compose->pid, &status)) {
				compose_done = TRUE;
				compose_ok = WIFEXITED(status) &&
				             WEXITSTATUS(status) == 0;
			} else if (bar_devservices_run_liveness("/proc",
			           svc->compose) == BAR_DEVSERVICES_GONE) {
				compose_done = TRUE;
			}
		}

		if (def->kind == BAR_DEVSERVICES_KIND_PODMAN) {
			running = bar_devservices_podman_running(ps_json, def);
		} else if (svc->run != NULL) {
			BarDevservicesLiveness live;

			if (bar_devservices_reap(svc->run->pid, &status)) {
				svc->have_exit = TRUE;
				svc->exit_status = status;
			}
			live = bar_devservices_run_liveness("/proc", svc->run);
			running = (live == BAR_DEVSERVICES_ALIVE ||
			           live == BAR_DEVSERVICES_ORPHANS);
			drop_run = !running;
		}
		if (running)
			healthy = ds_healthy(ctx, def, svc, listening);

		/* Somebody else on our port, while we are not running:
		   the reason Up would fail, and what Take over fixes. */
		if (!running && def->port > 0) {
			gpointer inode = g_hash_table_lookup(listening,
				GUINT_TO_POINTER((guint)def->port));

			if (inode != NULL) {
				foreign = bar_devservices_socket_owner("/proc",
					*(guint64 *)inode, getuid());
				if (foreign == 0)
					foreign = -1;
				else
					foreign_comm = ds_proc_comm(foreign);
			}
		}

		g_mutex_lock(&dd->lock);
		prev = svc->state;
		if (drop_run && svc->run != NULL) {
			/* It ended without being asked to: that is a
			   failure worth keeping on screen, not "off". */
			if (prev != BAR_DEVSERVICES_STOPPING &&
			    prev != BAR_DEVSERVICES_OFF) {
				if (svc->have_exit && WIFEXITED(svc->exit_status))
					error = g_strdup_printf("exited with "
						"status %d",
						WEXITSTATUS(svc->exit_status));
				else if (svc->have_exit &&
				         WIFSIGNALED(svc->exit_status))
					error = g_strdup_printf("killed by "
						"signal %d",
						WTERMSIG(svc->exit_status));
				else
					error = g_strdup("exited");
				prev = BAR_DEVSERVICES_FAILED;
				svc->state = BAR_DEVSERVICES_FAILED;
				ds_set(&svc->error, error);
			}
			g_clear_pointer(&svc->run, bar_devservices_run_free);
			proj->runs_changed = TRUE;
		}
		if (compose_done) {
			if (!compose_ok && svc->compose_up) {
				g_autofree gchar *msg = NULL;

				msg = g_strdup_printf("podman compose up failed%s%s",
				                      (line != NULL) ? ": " : "",
				                      (line != NULL) ? line : "");
				ds_set(&svc->error, msg);
				prev = BAR_DEVSERVICES_FAILED;
			}
			g_clear_pointer(&svc->compose, bar_devservices_run_free);
		}
		next = bar_devservices_status_step(&svc->unhealthy, prev,
		                                   running, healthy,
		                                   ctx->stuck_after);
		/* A podman service with its compose child still working is
		   starting, whatever ps says yet. */
		if (svc->compose != NULL && svc->compose_up &&
		    next == BAR_DEVSERVICES_OFF)
			next = BAR_DEVSERVICES_STARTING;
		if (svc->compose != NULL && !svc->compose_up)
			next = BAR_DEVSERVICES_STOPPING;
		if (svc->waiting && next == BAR_DEVSERVICES_STUCK)
			next = BAR_DEVSERVICES_STARTING;
		svc->state = next;
		svc->healthy = healthy;
		svc->foreign_pid = foreign;
		ds_set(&svc->foreign_comm, foreign_comm);
		ds_set(&svc->last_line, line);
		if (next == BAR_DEVSERVICES_UP)
			ds_set(&svc->error, NULL);
		g_mutex_unlock(&dd->lock);

		if (prev != next) {
			g_autofree gchar *summary = NULL;

			if (next == BAR_DEVSERVICES_STUCK) {
				summary = g_strdup_printf("%s: %s is stuck",
				                          proj->stack->name, def->label);
				ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL,
				         dd->op == NULL, summary,
				         "Running, but its health check keeps "
				         "failing.");
			} else if (next == BAR_DEVSERVICES_FAILED &&
			           dd->op == NULL) {
				summary = g_strdup_printf("%s: %s stopped",
				                          proj->stack->name, def->label);
				ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE,
				         summary, error);
			} else if (next == BAR_DEVSERVICES_UP) {
				summary = g_strdup_printf("%s: %s is up",
				                          proj->stack->name, def->label);
				ds_toast(plugin, dd, GOWL_BAR_TOAST_LOW, FALSE,
				         summary, NULL);
			}
		}
	}

	/* Tasks: reap the ones that finished and say how. */
	for (i = 0; i < proj->tasks->len; i++) {
		DsTask *task = g_ptr_array_index(proj->tasks, i);
		BarDevservicesTask *def = g_ptr_array_index(proj->stack->tasks, i);
		gint status = 0;
		gboolean reaped;
		g_autofree gchar *result = NULL;
		g_autofree gchar *summary = NULL;

		if (task->run == NULL)
			continue;
		reaped = bar_devservices_reap(task->run->pid, &status);
		if (!reaped && bar_devservices_run_liveness("/proc", task->run)
		    != BAR_DEVSERVICES_GONE)
			continue;
		if (reaped && WIFEXITED(status))
			result = (WEXITSTATUS(status) == 0)
				? g_strdup("finished")
				: g_strdup_printf("failed (status %d)",
				                  WEXITSTATUS(status));
		else
			result = g_strdup("ended");
		g_mutex_lock(&dd->lock);
		g_clear_pointer(&task->run, bar_devservices_run_free);
		ds_set(&task->result, result);
		proj->runs_changed = TRUE;
		g_mutex_unlock(&dd->lock);
		summary = g_strdup_printf("%s: %s %s", proj->stack->name,
		                          def->label, result);
		ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE, summary, NULL);
	}
}

/* Write the state file if a run was added or dropped. */
static void
ds_save_state(DsProject *proj, const gchar *state_dir)
{
	g_autoptr(GPtrArray) runs = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	if (!proj->runs_changed)
		return;
	proj->runs_changed = FALSE;
	/* Borrowed pointers: the array frees nothing. */
	runs = g_ptr_array_new();
	for (i = 0; i < proj->svcs->len; i++) {
		DsSvc *svc = g_ptr_array_index(proj->svcs, i);

		if (svc->run != NULL)
			g_ptr_array_add(runs, svc->run);
	}
	for (i = 0; i < proj->tasks->len; i++) {
		DsTask *task = g_ptr_array_index(proj->tasks, i);

		if (task->run != NULL)
			g_ptr_array_add(runs, task->run);
	}
	path = ds_state_path(state_dir, proj->stack->key);
	if (!bar_devservices_state_save(path, runs, &error))
		g_warning("gowl-bar: devservices: %s", error->message);
}

/* ----------------------------------------------------------------
 * Actions (worker): a state machine one step at a time
 * ---------------------------------------------------------------- */

static void
ds_op_finish(GowlBarPlugin *plugin, DevData *dd, DsProject *proj,
             const gchar *error)
{
	DsOp *op = dd->op;
	g_autofree gchar *summary = NULL;
	const gchar *verb;

	switch (op->kind) {
	case DS_OP_UP:      verb = "up"; break;
	case DS_OP_DOWN:    verb = "down"; break;
	case DS_OP_RESTART: verb = "restart"; break;
	case DS_OP_TASK:    verb = "task"; break;
	default:            verb = "action"; break;
	}

	if (error != NULL) {
		summary = g_strdup_printf("%s: %s failed",
		                          (proj != NULL) ? proj->stack->name
		                                         : op->project, verb);
		ds_toast(plugin, dd, GOWL_BAR_TOAST_CRITICAL, TRUE, summary, error);
	} else if (op->kind == DS_OP_UP || op->kind == DS_OP_RESTART) {
		summary = g_strdup_printf("%s is up", proj->stack->name);
		ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE, summary, NULL);
	} else if (op->kind == DS_OP_DOWN) {
		summary = g_strdup_printf("%s is down", proj->stack->name);
		ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE, summary, NULL);
	}

	g_mutex_lock(&dd->lock);
	if (proj != NULL) {
		guint i;

		for (i = 0; i < proj->svcs->len; i++)
			((DsSvc *)g_ptr_array_index(proj->svcs, i))->waiting = FALSE;
	}
	dd->op = NULL;
	ds_set(&dd->progress, NULL);
	ds_set(&dd->last_error, error);
	g_mutex_unlock(&dd->lock);
	ds_op_free(op);
}

/* Start one service of an up wave. */
static gboolean
ds_start_service(DevData *dd, DsProject *proj, guint idx, const DsCtx *ctx,
                 gchar **error)
{
	BarDevservicesService *def = g_ptr_array_index(proj->stack->services, idx);
	DsSvc *svc = g_ptr_array_index(proj->svcs, idx);
	g_autofree gchar *log = ds_log_path(ctx->state_dir, proj->stack->key,
	                                    def->id);
	g_autoptr(GError) local = NULL;
	BarDevservicesRun *run;

	if (svc->state == BAR_DEVSERVICES_UP ||
	    svc->state == BAR_DEVSERVICES_STARTING ||
	    svc->state == BAR_DEVSERVICES_STUCK)
		return TRUE;

	if (def->kind == BAR_DEVSERVICES_KIND_PODMAN) {
		g_auto(GStrv) argv = NULL;

		if (!ctx->have_podman) {
			*error = g_strdup_printf("%s needs podman, which is not "
				"installed (Fedora: podman, podman-compose)",
				def->label);
			return FALSE;
		}
		argv = bar_devservices_compose_argv(ctx->compose, def, TRUE);
		if (argv == NULL) {
			*error = g_strdup("the compose-command setting does not "
			                  "parse");
			return FALSE;
		}
		run = bar_devservices_spawn((const gchar * const *)argv,
		                            proj->stack->root, NULL, log,
		                            "compose", "/proc", &local);
		if (run == NULL) {
			*error = g_strdup_printf("%s: %s", def->label,
			                         local->message);
			return FALSE;
		}
		g_mutex_lock(&dd->lock);
		bar_devservices_run_free(svc->compose);
		svc->compose = run;
		svc->compose_up = TRUE;
		svc->waiting = TRUE;
		svc->state = BAR_DEVSERVICES_STARTING;
		svc->unhealthy = 0;
		ds_set(&svc->error, NULL);
		g_mutex_unlock(&dd->lock);
		return TRUE;
	}

	run = bar_devservices_spawn((const gchar * const *)def->argv, def->dir,
	                            (const gchar * const *)def->env, log,
	                            def->id, "/proc", &local);
	if (run == NULL) {
		*error = g_strdup_printf("%s: %s", def->label, local->message);
		g_mutex_lock(&dd->lock);
		svc->state = BAR_DEVSERVICES_FAILED;
		ds_set(&svc->error, local->message);
		g_mutex_unlock(&dd->lock);
		return FALSE;
	}
	g_mutex_lock(&dd->lock);
	bar_devservices_run_free(svc->run);
	svc->run = run;
	svc->have_exit = FALSE;
	svc->waiting = TRUE;
	svc->spawned_at = g_get_monotonic_time();
	svc->state = BAR_DEVSERVICES_STARTING;
	svc->unhealthy = 0;
	ds_set(&svc->error, NULL);
	proj->runs_changed = TRUE;
	g_mutex_unlock(&dd->lock);
	return TRUE;
}

/* Begin stopping one service of a down wave. */
static gboolean
ds_stop_service(DevData *dd, DsProject *proj, guint idx, const DsCtx *ctx,
                gchar **error)
{
	BarDevservicesService *def = g_ptr_array_index(proj->stack->services, idx);
	DsSvc *svc = g_ptr_array_index(proj->svcs, idx);

	if (def->kind == BAR_DEVSERVICES_KIND_PODMAN) {
		g_autofree gchar *log = NULL;
		g_auto(GStrv) argv = NULL;
		g_autoptr(GError) local = NULL;
		BarDevservicesRun *run;

		if (svc->state == BAR_DEVSERVICES_OFF ||
		    svc->state == BAR_DEVSERVICES_FAILED)
			return TRUE;
		if (!ctx->have_podman) {
			*error = g_strdup("podman is not installed (Fedora: "
			                  "podman, podman-compose)");
			return FALSE;
		}
		argv = bar_devservices_compose_argv(ctx->compose, def, FALSE);
		if (argv == NULL) {
			*error = g_strdup("the compose-command setting does not "
			                  "parse");
			return FALSE;
		}
		log = ds_log_path(ctx->state_dir, proj->stack->key, def->id);
		run = bar_devservices_spawn((const gchar * const *)argv,
		                            proj->stack->root, NULL, log,
		                            "compose", "/proc", &local);
		if (run == NULL) {
			*error = g_strdup_printf("%s: %s", def->label,
			                         local->message);
			return FALSE;
		}
		g_mutex_lock(&dd->lock);
		bar_devservices_run_free(svc->compose);
		svc->compose = run;
		svc->compose_up = FALSE;
		svc->state = BAR_DEVSERVICES_STOPPING;
		g_mutex_unlock(&dd->lock);
		return TRUE;
	}

	if (svc->run == NULL) {
		g_mutex_lock(&dd->lock);
		if (svc->state == BAR_DEVSERVICES_FAILED)
			svc->state = BAR_DEVSERVICES_OFF;
		g_mutex_unlock(&dd->lock);
		return TRUE;
	}
	/* The run's start time is checked before the signal: a pid the
	   state file remembers may belong to somebody else by now. */
	if (bar_devservices_run_signal("/proc", svc->run, SIGTERM)
	    == BAR_DEVSERVICES_MISMATCH) {
		g_mutex_lock(&dd->lock);
		g_clear_pointer(&svc->run, bar_devservices_run_free);
		svc->state = BAR_DEVSERVICES_OFF;
		proj->runs_changed = TRUE;
		g_mutex_unlock(&dd->lock);
		return TRUE;
	}
	g_mutex_lock(&dd->lock);
	svc->state = BAR_DEVSERVICES_STOPPING;
	g_mutex_unlock(&dd->lock);
	return TRUE;
}

/* Is every service of the current wave where the op wants it? */
static gboolean
ds_wave_settled(DsProject *proj, GArray *wave, gboolean stopping,
                gchar **failed)
{
	guint i;
	gboolean all = TRUE;

	for (i = 0; i < wave->len; i++) {
		guint idx = g_array_index(wave, guint, i);
		DsSvc *svc = g_ptr_array_index(proj->svcs, idx);
		BarDevservicesService *def;

		def = g_ptr_array_index(proj->stack->services, idx);
		if (stopping) {
			if (svc->run != NULL || svc->compose != NULL)
				all = FALSE;
			continue;
		}
		if (svc->state == BAR_DEVSERVICES_FAILED) {
			*failed = g_strdup_printf("%s %s", def->label,
				(svc->error != NULL) ? svc->error : "failed");
			return FALSE;
		}
		if (svc->state != BAR_DEVSERVICES_UP)
			all = FALSE;
	}
	return all;
}

static void
ds_open_url(GowlBarPlugin *plugin, const gchar *open_cmd, const gchar *url)
{
	g_autofree gchar *quoted = g_shell_quote(url);
	g_autofree gchar *line = g_strdup_printf("%s %s", open_cmd, quoted);

	gowl_bar_plugin_spawn(plugin, line);
}

static gint
ds_wave_wait(DsProject *proj, GArray *wave)
{
	guint i;
	gint wait = 1;

	for (i = 0; i < wave->len; i++) {
		BarDevservicesService *def;

		def = g_ptr_array_index(proj->stack->services,
		                        g_array_index(wave, guint, i));
		wait = MAX(wait, def->wait);
	}
	return wait;
}

/* The down half's waves: the up order, reversed. */
static GPtrArray *
ds_reverse(GPtrArray *waves)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_array_unref);
	guint i;

	for (i = waves->len; i > 0; i--)
		g_ptr_array_add(out, g_array_ref(g_ptr_array_index(waves, i - 1)));
	return out;
}

/*
 * ds_op_step:
 *
 * Advance the action by one step.  Starting a wave and checking on it
 * are separate steps on purpose: the services get a poll's worth of
 * time between, and no step blocks for longer than a health check.
 */
static void
ds_op_step(GowlBarPlugin *plugin, DevData *dd, const DsCtx *ctx)
{
	DsOp *op = dd->op;
	DsProject *proj;
	GArray *wave;
	gint64 now = g_get_monotonic_time();
	g_autofree gchar *error = NULL;
	g_autofree gchar *progress = NULL;
	guint i;

	proj = ds_find_project(dd, op->project);
	if (proj == NULL) {
		ds_op_finish(plugin, dd, NULL, "the stack is no longer declared");
		return;
	}

	if (op->wave >= op->waves->len) {
		if (op->kind == DS_OP_RESTART && op->stopping) {
			/* The down half is done: walk the same waves up. */
			GPtrArray *up = ds_reverse(op->waves);

			g_ptr_array_unref(op->waves);
			op->waves = up;
			op->stopping = FALSE;
			op->wave = 0;
			op->wave_started = FALSE;
			ds_save_state(proj, ctx->state_dir);
			return;
		}
		if (op->kind == DS_OP_TASK) {
			BarDevservicesTask *def;
			DsTask *task = NULL;
			g_autofree gchar *log = NULL;
			g_autoptr(GError) local = NULL;
			g_autofree gchar *rid = NULL;
			BarDevservicesRun *run;

			def = bar_devservices_stack_find_task(proj->stack, op->task);
			for (i = 0; def != NULL && i < proj->tasks->len; i++)
				if (g_ptr_array_index(proj->stack->tasks, i) == def)
					task = g_ptr_array_index(proj->tasks, i);
			if (def == NULL || task == NULL) {
				ds_op_finish(plugin, dd, proj, "the task is gone");
				return;
			}
			log = ds_task_log_path(ctx->state_dir, proj->stack->key,
			                       def->id);
			rid = g_strconcat("task:", def->id, NULL);
			run = bar_devservices_spawn((const gchar * const *)def->argv,
			                            def->dir, NULL, log, rid, "/proc",
			                            &local);
			if (run == NULL) {
				ds_op_finish(plugin, dd, proj, local->message);
				return;
			}
			g_mutex_lock(&dd->lock);
			bar_devservices_run_free(task->run);
			task->run = run;
			ds_set(&task->result, "running");
			proj->runs_changed = TRUE;
			g_mutex_unlock(&dd->lock);
			ds_save_state(proj, ctx->state_dir);
		}
		ds_save_state(proj, ctx->state_dir);
		ds_op_finish(plugin, dd, proj, NULL);
		return;
	}

	wave = g_ptr_array_index(op->waves, op->wave);

	if (!op->wave_started) {
		for (i = 0; i < wave->len; i++) {
			guint idx = g_array_index(wave, guint, i);
			gboolean ok;

			ok = op->stopping
				? ds_stop_service(dd, proj, idx, ctx, &error)
				: ds_start_service(dd, proj, idx, ctx, &error);
			if (!ok) {
				ds_save_state(proj, ctx->state_dir);
				ds_op_finish(plugin, dd, proj, error);
				return;
			}
		}
		op->wave_started = TRUE;
		op->killed = FALSE;
		op->deadline = now + (gint64)(op->stopping
			? ctx->stop_grace : ds_wave_wait(proj, wave))
			* G_USEC_PER_SEC;
		/* A container's stop has its own ten-second timeout. */
		if (op->stopping)
			for (i = 0; i < wave->len; i++) {
				BarDevservicesService *def = g_ptr_array_index(
					proj->stack->services,
					g_array_index(wave, guint, i));

				if (def->kind == BAR_DEVSERVICES_KIND_PODMAN)
					op->deadline = MAX(op->deadline,
						now + 120 * G_USEC_PER_SEC);
			}
		ds_save_state(proj, ctx->state_dir);
		progress = g_strdup_printf("%s wave %u of %u",
		                           op->stopping ? "Stopping" : "Starting",
		                           op->wave + 1, op->waves->len);
		g_mutex_lock(&dd->lock);
		ds_set(&dd->progress, progress);
		g_mutex_unlock(&dd->lock);
		return;
	}

	if (ds_wave_settled(proj, wave, op->stopping, &error)) {
		if (!op->stopping)
			for (i = 0; i < wave->len; i++) {
				BarDevservicesService *def = g_ptr_array_index(
					proj->stack->services,
					g_array_index(wave, guint, i));

				if (def->open && def->url != NULL)
					ds_open_url(plugin, ctx->open_cmd, def->url);
			}
		op->wave++;
		op->wave_started = FALSE;
		return;
	}
	if (error != NULL) {
		ds_op_finish(plugin, dd, proj, error);
		return;
	}
	if (now < op->deadline)
		return;

	if (op->stopping && !op->killed) {
		/* The grace period is over: SIGKILL whatever of the wave's
		   process groups is left, still checked by start time. */
		for (i = 0; i < wave->len; i++) {
			DsSvc *svc = g_ptr_array_index(proj->svcs,
			                               g_array_index(wave, guint, i));

			if (svc->run != NULL)
				bar_devservices_run_signal("/proc", svc->run, SIGKILL);
		}
		op->killed = TRUE;
		op->deadline = now + 3 * G_USEC_PER_SEC;
		return;
	}

	/* Out of time. */
	{
		GString *late = g_string_new(NULL);

		for (i = 0; i < wave->len; i++) {
			guint idx = g_array_index(wave, guint, i);
			DsSvc *svc = g_ptr_array_index(proj->svcs, idx);
			BarDevservicesService *def;

			def = g_ptr_array_index(proj->stack->services, idx);
			if (op->stopping ? (svc->run == NULL && svc->compose == NULL)
			                 : svc->state == BAR_DEVSERVICES_UP)
				continue;
			g_string_append_printf(late, "%s%s", late->len ? ", " : "",
			                       def->label);
			if (!op->stopping) {
				g_autofree gchar *why = g_strdup_printf(
					"not healthy after %d s", def->wait);

				g_mutex_lock(&dd->lock);
				svc->state = BAR_DEVSERVICES_FAILED;
				ds_set(&svc->error, why);
				g_mutex_unlock(&dd->lock);
			}
		}
		g_string_prepend(late, op->stopping ? "would not stop: "
		                                    : "not healthy in time: ");
		ds_op_finish(plugin, dd, proj, late->str);
		g_string_free(late, TRUE);
	}
}

/* Turn a queued request into an op, or refuse it with a toast. */
static void
ds_begin(GowlBarPlugin *plugin, DevData *dd, DsRequest *req)
{
	DsProject *proj = ds_find_project(dd, req->project);
	g_autoptr(GError) error = NULL;
	GPtrArray *waves = NULL;
	DsOp *op;
	guint i;

	if (proj == NULL)
		return;

	if (req->kind == DS_OP_TASK) {
		BarDevservicesTask *task;

		task = bar_devservices_stack_find_task(proj->stack, req->task);
		if (task == NULL)
			return;
		waves = bar_devservices_plan(proj->stack,
		                             (const gchar * const *)task->needs,
		                             &error);
		/* A task with no needs plans to nothing; an empty `only'
		   must not mean "everything". */
		if (waves != NULL && task->needs[0] == NULL)
			g_ptr_array_set_size(waves, 0);
	} else {
		waves = bar_devservices_plan(proj->stack, NULL, &error);
	}
	if (waves == NULL) {
		g_autofree gchar *summary = g_strdup_printf("%s: cannot start",
		                                            proj->stack->name);

		ds_toast(plugin, dd, GOWL_BAR_TOAST_CRITICAL, TRUE, summary,
		         error->message);
		g_mutex_lock(&dd->lock);
		ds_set(&dd->last_error, error->message);
		g_mutex_unlock(&dd->lock);
		return;
	}

	op = g_new0(DsOp, 1);
	op->kind = req->kind;
	op->project = g_strdup(proj->stack->key);
	op->task = g_strdup(req->task);
	if (req->kind == DS_OP_DOWN || req->kind == DS_OP_RESTART) {
		op->waves = ds_reverse(waves);
		g_ptr_array_unref(waves);
		op->stopping = TRUE;
	} else {
		op->waves = waves;
		/* Up starts from a clean slate: a failure from last time
		   is not this attempt's failure. */
		g_mutex_lock(&dd->lock);
		for (i = 0; i < proj->svcs->len; i++) {
			DsSvc *svc = g_ptr_array_index(proj->svcs, i);

			if (svc->state == BAR_DEVSERVICES_FAILED) {
				svc->state = BAR_DEVSERVICES_OFF;
				ds_set(&svc->error, NULL);
			}
		}
		g_mutex_unlock(&dd->lock);
	}
	g_mutex_lock(&dd->lock);
	dd->op = op;
	ds_set(&dd->last_error, NULL);
	g_mutex_unlock(&dd->lock);
}

/* ----------------------------------------------------------------
 * The worker step
 * ---------------------------------------------------------------- */

static void
ds_ctx_init(GowlBarPlugin *plugin, DsCtx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state_dir = ds_state_dir(plugin);
	ctx->podman = ds_setting(plugin, "podman", "podman");
	ctx->compose = ds_setting(plugin, "compose-command", "podman compose");
	ctx->curl = ds_setting(plugin, "curl", "curl");
	ctx->open_cmd = ds_setting(plugin, "open-command", "xdg-open");
	ctx->stuck_after = ds_setting_int(plugin, "stuck-after", 6, 1, 1000);
	ctx->stop_grace = ds_setting_int(plugin, "stop-grace", 5, 1, 600);
	ctx->have_podman = ds_have(ctx->podman);
	ctx->have_curl = ds_have(ctx->curl);
}

/* Which project the focused window is in (worker). */
static void
ds_update_focus(GowlBarPlugin *plugin, DevData *dd)
{
	gint pid = g_atomic_int_get(&dd->focus_pid);
	g_autofree gchar *cwd = NULL;
	GPtrArray *stacks;
	gint idx;
	guint i;
	const gchar *key = NULL;

	/* Only `auto' follows the focus; the walk reads every process's
	   stat, which is not worth doing for a mode that ignores it. */
	if (!ds_mode_is(plugin, "auto"))
		return;

	/*
	 * Our own process -- cmacs --gowl's frames -- has a cwd that
	 * means nothing; cmacs pushes the current buffer's directory as
	 * `focus-cwd' instead.  So does a session with no compositor.
	 */
	if (pid <= 0 || pid == (gint)getpid())
		cwd = ds_setting(plugin, "focus-cwd", NULL);
	else
		cwd = bar_devservices_focus_cwd("/proc", pid);
	if (cwd != NULL && cwd[0] == '~') {
		gchar *expanded = bar_expand_tilde(cwd);

		g_free(cwd);
		cwd = expanded;
	}

	/* The matcher takes stacks; the projects are borrowed into a
	   plain array of them.  The worker is the only one that replaces
	   the projects, so they cannot change under it here. */
	stacks = g_ptr_array_new();
	for (i = 0; i < dd->projects->len; i++)
		g_ptr_array_add(stacks,
		                ((DsProject *)g_ptr_array_index(dd->projects,
		                                                i))->stack);
	idx = bar_devservices_match_root(stacks, cwd);
	if (idx >= 0)
		key = ((BarDevservicesStack *)g_ptr_array_index(stacks,
		                                                idx))->key;
	g_mutex_lock(&dd->lock);
	ds_set(&dd->auto_key, key);
	g_mutex_unlock(&dd->lock);
	g_ptr_array_unref(stacks);
}

/* Stop whoever holds a service's port: one pid, ours, re-checked. */
static void
ds_do_takeover(GowlBarPlugin *plugin, DevData *dd, const gchar *svc_id)
{
	DsProject *proj;
	BarDevservicesService *def;
	g_autoptr(GHashTable) listening = NULL;
	gpointer inode;
	gint idx, pid, port = 0;
	g_autofree gchar *comm = NULL;
	g_autofree gchar *summary = NULL;

	/* The pin is the compositor thread's to change. */
	g_mutex_lock(&dd->lock);
	proj = ds_current(plugin, dd);
	idx = (proj != NULL)
		? bar_devservices_stack_find_service(proj->stack, svc_id) : -1;
	if (idx >= 0) {
		def = g_ptr_array_index(proj->stack->services, idx);
		port = def->port;
	}
	g_mutex_unlock(&dd->lock);
	if (port <= 0)
		return;
	listening = bar_devservices_listening("/proc");
	inode = g_hash_table_lookup(listening, GUINT_TO_POINTER((guint)port));
	if (inode == NULL)
		return;
	pid = bar_devservices_socket_owner("/proc", *(guint64 *)inode, getuid());
	/* One pid, never gowl itself, never another user's (the owner
	   search only looks at ours).  Not its group: the listener may
	   share one with the terminal it was started from. */
	if (pid <= 1 || pid == (gint)getpid()) {
		summary = g_strdup_printf("Port %d is not held by one of your "
		                          "processes", port);
		ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE, summary, NULL);
		return;
	}
	comm = ds_proc_comm(pid);
	if (kill((pid_t)pid, SIGTERM) != 0) {
		summary = g_strdup_printf("Could not stop pid %d", pid);
		ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE, summary,
		         g_strerror(errno));
		return;
	}
	summary = g_strdup_printf("Stopped %s (pid %d) on port %d",
	                          (comm != NULL) ? comm : "process", pid,
	                          port);
	ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE, summary, NULL);
}

/* Stop a running task's process group, checked by start time. */
static void
ds_do_stop_task(GowlBarPlugin *plugin, DevData *dd, const gchar *task_id)
{
	guint i, p;

	(void)plugin;
	for (p = 0; p < dd->projects->len; p++) {
		DsProject *proj = g_ptr_array_index(dd->projects, p);

		for (i = 0; i < proj->tasks->len; i++) {
			BarDevservicesTask *def;
			DsTask *task = g_ptr_array_index(proj->tasks, i);

			def = g_ptr_array_index(proj->stack->tasks, i);
			if (task->run != NULL && strcmp(def->id, task_id) == 0)
				bar_devservices_run_signal("/proc", task->run,
				                           SIGTERM);
		}
	}
}

static void
ds_do_scan(GowlBarPlugin *plugin, DevData *dd)
{
	g_autofree gchar *roots = NULL;
	GPtrArray *found;
	GPtrArray *stacks;
	guint i;

	roots = ds_setting(plugin, "search-roots",
	                   "~/source/projects:~/Projects");
	/* The scan only needs the roots; borrow the stacks. */
	stacks = g_ptr_array_new();
	for (i = 0; i < dd->projects->len; i++)
		g_ptr_array_add(stacks,
		                ((DsProject *)g_ptr_array_index(dd->projects,
		                                                i))->stack);
	found = bar_devservices_scan(roots, stacks, 24);
	g_ptr_array_unref(stacks);

	g_mutex_lock(&dd->lock);
	g_ptr_array_unref(dd->candidates);
	dd->candidates = found;
	dd->scanning = FALSE;
	dd->scanned = TRUE;
	g_mutex_unlock(&dd->lock);
}

static void
ds_do_declare(GowlBarPlugin *plugin, DevData *dd, const gchar *key)
{
	BarDevservicesCandidate *cand = NULL;
	g_autofree gchar *dir = ds_config_dir(plugin);
	g_autofree gchar *path = NULL;
	g_autofree gchar *summary = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	/* Candidates are only replaced on this (the worker's) side. */
	for (i = 0; i < dd->candidates->len; i++) {
		BarDevservicesCandidate *c = g_ptr_array_index(dd->candidates, i);

		if (strcmp(c->key, key) == 0)
			cand = c;
	}
	if (cand == NULL)
		return;
	if (!bar_devservices_write_draft(dir, cand, &path, &error)) {
		ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE,
		         "Could not declare the stack", error->message);
		return;
	}
	summary = g_strdup_printf("Declared %s", cand->key);
	ds_toast(plugin, dd, GOWL_BAR_TOAST_NORMAL, TRUE, summary,
	         "Edit the draft before pressing Up: nothing in it has run.");
	g_mutex_lock(&dd->lock);
	g_ptr_array_add(dd->declared, g_strdup(cand->key));
	dd->force_reload = TRUE;
	g_mutex_unlock(&dd->lock);
}

/*
 * ds_step:
 *
 * Everything the widget does that waits, in one place, serialised by
 * the work mutex: reload, take a request, refresh status, advance the
 * action, save state.  Run by the async poll and queued from clicks.
 */
static void
ds_step(GowlBarPlugin *plugin, DevData *dd)
{
	DsCtx ctx;
	DsRequest *req;
	g_autofree gchar *ps_json = NULL;
	g_autofree gchar *takeover = NULL;
	g_autofree gchar *declare = NULL;
	g_autofree gchar *stop_task = NULL;
	g_autoptr(GHashTable) listening = NULL;
	gboolean scan, any_podman = FALSE;
	guint i;

	g_mutex_lock(&dd->work);
	ds_ctx_init(plugin, &ctx);

	ds_reload(plugin, dd, ctx.state_dir);
	ds_update_focus(plugin, dd);

	g_mutex_lock(&dd->lock);
	req = (dd->op == NULL) ? g_steal_pointer(&dd->pending) : NULL;
	takeover = g_steal_pointer(&dd->takeover);
	declare = g_steal_pointer(&dd->declare_key);
	stop_task = g_steal_pointer(&dd->stop_task);
	scan = dd->scan_requested;
	dd->scan_requested = FALSE;
	dd->podman_missing = !ctx.have_podman;
	dd->curl_missing = !ctx.have_curl;
	g_mutex_unlock(&dd->lock);

	if (req != NULL) {
		ds_begin(plugin, dd, req);
		ds_request_free(req);
	}
	if (takeover != NULL)
		ds_do_takeover(plugin, dd, takeover);
	if (stop_task != NULL)
		ds_do_stop_task(plugin, dd, stop_task);
	if (declare != NULL)
		ds_do_declare(plugin, dd, declare);
	if (scan)
		ds_do_scan(plugin, dd);
	if (declare != NULL)
		ds_reload(plugin, dd, ctx.state_dir);

	/* One `podman ps' and one read of the socket tables per step,
	   shared by every service of every project. */
	for (i = 0; i < dd->projects->len; i++) {
		DsProject *proj = g_ptr_array_index(dd->projects, i);
		guint j;

		for (j = 0; j < proj->stack->services->len; j++)
			if (((BarDevservicesService *)g_ptr_array_index(
			     proj->stack->services, j))->kind ==
			    BAR_DEVSERVICES_KIND_PODMAN)
				any_podman = TRUE;
	}
	if (any_podman && ctx.have_podman) {
		const gchar *argv[] = { ctx.podman, "ps", "--format", "json",
		                        NULL };

		ps_json = bar_run_argv(argv);
	}
	listening = bar_devservices_listening("/proc");

	for (i = 0; i < dd->projects->len; i++) {
		DsProject *proj = g_ptr_array_index(dd->projects, i);

		ds_refresh_project(plugin, dd, proj, &ctx, ps_json, listening);
		ds_save_state(proj, ctx.state_dir);
	}

	if (dd->op != NULL)
		ds_op_step(plugin, dd, &ctx);

	ds_ctx_clear(&ctx);
	g_mutex_unlock(&dd->work);

	ds_apply_label(plugin, dd);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
ds_work(GowlBarPlugin *plugin, gpointer user_data)
{
	ds_step(plugin, (DevData *)user_data);
}

/* ----------------------------------------------------------------
 * Vtable: poll side
 * ---------------------------------------------------------------- */

/* Poll fast while anything moves; slowly otherwise. */
static gint
ds_interval(GowlBarPlugin *plugin, gpointer data)
{
	DevData *dd = data;
	gboolean busy;
	guint i, j;

	g_mutex_lock(&dd->lock);
	busy = (dd->op != NULL || dd->pending != NULL || dd->scanning);
	for (i = 0; !busy && i < dd->projects->len; i++) {
		DsProject *proj = g_ptr_array_index(dd->projects, i);

		for (j = 0; j < proj->svcs->len; j++) {
			DsSvc *svc = g_ptr_array_index(proj->svcs, j);

			if (svc->state == BAR_DEVSERVICES_STARTING ||
			    svc->state == BAR_DEVSERVICES_STOPPING)
				busy = TRUE;
		}
	}
	g_mutex_unlock(&dd->lock);
	if (busy)
		return 1;
	return ds_setting_int(plugin, "interval", 5, 1, 3600);
}

/*
 * ds_focus_pid:
 *
 * The focused window's pid.  The compositor may only be asked on its
 * own thread, which is where the poll runs; the worker gets the number.
 */
static gint
ds_focus_pid(void)
{
	const BarEnv *env = bar_env();
	GowlClient *focused;

	if (env == NULL || env->compositor == NULL)
		return 0;
	focused = gowl_compositor_get_focused_client(
		GOWL_COMPOSITOR(env->compositor));
	if (focused == NULL)
		return 0;
	return (gint)gowl_client_get_pid(focused);
}

/* Raise the toasts the worker queued, here on the bar's thread. */
static void
ds_flush_toasts(GowlBarPlugin *plugin, DevData *dd)
{
	GPtrArray *toasts;
	guint i;

	g_mutex_lock(&dd->lock);
	toasts = dd->toasts;
	dd->toasts = g_ptr_array_new_with_free_func(g_free);
	g_mutex_unlock(&dd->lock);

	for (i = 0; i < toasts->len; i++) {
		g_auto(GStrv) f = g_strsplit(g_ptr_array_index(toasts, i), "\t", 3);

		if (g_strv_length(f) < 3)
			continue;
		gowl_bar_plugin_notify(plugin,
			(GowlBarToastUrgency)g_ascii_strtoll(f[0], NULL, 10),
			f[1], (f[2][0] != '\0') ? f[2] : NULL);
	}
	g_ptr_array_unref(toasts);
}

static void
ds_poll(GowlBarPlugin *plugin, gpointer data)
{
	DevData *dd = data;

	if (ds_mode_is(plugin, "auto"))
		g_atomic_int_set(&dd->focus_pid, ds_focus_pid());
	ds_flush_toasts(plugin, dd);
	ds_apply_label(plugin, dd);
}

static void
ds_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	ds_step(plugin, (DevData *)data);
}

static void
ds_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	DevData *dd = data;

	(void)settings;
	/* config-dir may have moved; the next step re-reads it. */
	g_mutex_lock(&dd->lock);
	dd->force_reload = TRUE;
	g_mutex_unlock(&dd->lock);
	ds_apply_label(plugin, dd);
}

/* ----------------------------------------------------------------
 * Panel
 * ---------------------------------------------------------------- */

static GowlBarColor
ds_state_role(BarDevservicesState state)
{
	switch (state) {
	case BAR_DEVSERVICES_UP:       return GOWL_BAR_COLOR_GREEN;
	case BAR_DEVSERVICES_STARTING:
	case BAR_DEVSERVICES_STOPPING: return GOWL_BAR_COLOR_YELLOW;
	case BAR_DEVSERVICES_STUCK:
	case BAR_DEVSERVICES_FAILED:   return GOWL_BAR_COLOR_RED;
	case BAR_DEVSERVICES_OFF:
	default:                       return GOWL_BAR_COLOR_SUBTEXT;
	}
}

static gboolean
ds_is_armed(DevData *dd, const gchar *id)
{
	return dd->armed != NULL && strcmp(dd->armed, id) == 0 &&
	       g_get_monotonic_time() - dd->armed_at < DS_ARM_US;
}

static void
ds_panel_project(GowlBarPlugin *plugin, DevData *dd, GowlBarPanel *panel,
                 DsProject *proj)
{
	g_autoptr(GArray) states = NULL;
	GowlBarPanelItem *item;
	BarDevservicesAgg agg;
	g_autofree gchar *sub = NULL;
	guint up = 0, i;
	gboolean busy = (dd->op != NULL || dd->pending != NULL);

	states = g_array_new(FALSE, FALSE, sizeof(BarDevservicesState));
	ds_collect_states(dd, proj, states);
	agg = bar_devservices_aggregate((const BarDevservicesState *)
	                                states->data, states->len, busy, &up);
	if (busy && dd->progress != NULL)
		sub = g_strdup(dd->progress);
	else if (busy)
		sub = g_strdup("Working...");
	else if (agg == BAR_DEVSERVICES_AGG_ERROR)
		sub = g_strdup_printf("%u of %u up, with problems", up,
		                      states->len);
	else
		sub = g_strdup_printf("%u of %u up", up, states->len);
	item = gowl_bar_panel_add_hero(panel, DS_ICON, proj->stack->name, sub);
	gowl_bar_panel_item_set_busy(item, busy);

	if (dd->last_error != NULL) {
		item = gowl_bar_panel_add_label(panel, dd->last_error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}

	if (proj->svcs->len > 0)
		gowl_bar_panel_add_section(panel, "Services");
	for (i = 0; i < proj->svcs->len; i++) {
		DsSvc *svc = g_ptr_array_index(proj->svcs, i);
		BarDevservicesService *def;
		g_autofree gchar *row_id = NULL;
		g_autofree gchar *value = NULL;
		const gchar *subtitle;

		def = g_ptr_array_index(proj->stack->services, i);
		row_id = g_strconcat("svc:", def->id, NULL);
		subtitle = (svc->error != NULL) ? svc->error
			: (svc->last_line != NULL) ? svc->last_line
			: (def->kind == BAR_DEVSERVICES_KIND_PODMAN)
				? "podman compose" : "process";
		item = gowl_bar_panel_add_row(panel, row_id,
			(def->kind == BAR_DEVSERVICES_KIND_PODMAN)
				? "\xef\x8e\x95" : "\xef\x84\xa0",
			def->label, subtitle);
		if (def->port > 0)
			value = g_strdup_printf("%s :%d",
				bar_devservices_state_name(svc->state), def->port);
		else
			value = g_strdup(bar_devservices_state_name(svc->state));
		gowl_bar_panel_item_set_value(item, value);
		gowl_bar_panel_item_set_value_color(item,
		                                    ds_state_role(svc->state));

		if (svc->foreign_pid != 0 && svc->state != BAR_DEVSERVICES_UP) {
			g_autofree gchar *take_id = NULL;
			g_autofree gchar *title = NULL;
			g_autofree gchar *who = NULL;

			take_id = g_strconcat("takeover:", def->id, NULL);
			if (svc->foreign_pid < 0) {
				who = g_strdup_printf("Port %d is held by another "
				                      "user's process", def->port);
				item = gowl_bar_panel_add_label(panel, who);
				gowl_bar_panel_item_set_color(item,
				                              GOWL_BAR_COLOR_YELLOW);
				continue;
			}
			who = g_strdup_printf("held by %s (pid %d)",
			                      (svc->foreign_comm != NULL)
			                      ? svc->foreign_comm : "a process",
			                      svc->foreign_pid);
			title = ds_is_armed(dd, take_id)
				? g_strdup_printf("Click again to stop pid %d",
				                  svc->foreign_pid)
				: g_strdup_printf("Take over port %d", def->port);
			item = gowl_bar_panel_add_row(panel, take_id, "\xef\x81\xb1",
			                              title, who);
			gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
		}
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, "Up", FALSE);
	gowl_bar_panel_add_button(item, "Down", FALSE);
	gowl_bar_panel_add_button(item, "Restart", FALSE);
	gowl_bar_panel_add_button(item, "Logs", FALSE);
	gowl_bar_panel_add_button(item, "Open", FALSE);
	gowl_bar_panel_item_set_disabled(item, busy);

	if (proj->tasks->len > 0) {
		gowl_bar_panel_add_section(panel, "Tasks");
		for (i = 0; i < proj->tasks->len; i++) {
			DsTask *task = g_ptr_array_index(proj->tasks, i);
			BarDevservicesTask *def;
			g_autofree gchar *row_id = NULL;
			g_autofree gchar *needs = NULL;

			def = g_ptr_array_index(proj->stack->tasks, i);
			row_id = g_strconcat("task:", def->id, NULL);
			if (def->needs[0] != NULL) {
				g_autofree gchar *list = g_strjoinv(", ", def->needs);

				needs = g_strconcat("needs ", list, NULL);
			}
			item = gowl_bar_panel_add_row(panel, row_id, "\xef\x81\x8b",
				def->label, (task->result != NULL &&
				             task->run == NULL)
				            ? task->result : needs);
			gowl_bar_panel_item_set_value(item,
				(task->run != NULL) ? "running - click to stop"
				                    : "run");
			gowl_bar_panel_item_set_value_color(item,
				(task->run != NULL) ? GOWL_BAR_COLOR_YELLOW
				                    : GOWL_BAR_COLOR_SUBTEXT);
		}
	}
	(void)plugin;
}

static GowlBarPanel *
ds_panel(GowlBarPlugin *plugin, gpointer data)
{
	DevData *dd = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	DsProject *proj;
	g_autoptr(GPtrArray) stacks = NULL;
	g_autoptr(GPtrArray) clashes = NULL;
	guint i;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 480);

	/* Held across the build: every string below is the worker's to
	   replace. */
	g_mutex_lock(&dd->lock);
	proj = ds_current(plugin, dd);

	if (dd->projects->len == 0) {
		g_autofree gchar *where = NULL;

		gowl_bar_panel_add_hero(panel, DS_ICON, "Dev services",
		                        "No stacks declared");
		where = g_strdup_printf("Declare a project's services in %s/"
			"<project>.yaml (name, root, services, tasks) -- see "
			"docs/bar-widgets/devservices.org -- or scan your "
			"source directories below for projects to start from.",
			(dd->config_dir != NULL) ? dd->config_dir
			                         : "~/.config/gowl/devservices");
		gowl_bar_panel_add_label(panel, where);
	} else if (proj != NULL) {
		ds_panel_project(plugin, dd, panel, proj);
	} else {
		gowl_bar_panel_add_hero(panel, DS_ICON, "Dev services",
			ds_mode_is(plugin, "all")
				? "Every project"
				: "The focused window is not in a project");
	}

	/* The project list: what to pin, and the way back. */
	if (dd->projects->len > 0 &&
	    (!ds_mode_is(plugin, "fixed") || dd->pinned != NULL)) {
		gowl_bar_panel_add_section(panel, "Projects");
		if (dd->pinned != NULL) {
			item = gowl_bar_panel_add_row(panel, "unpin", "\xef\x82\x8d",
				"Unpin",
				ds_mode_is(plugin, "all")
					? "Back to every project"
					: "Follow the focused window again");
			gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_SUBTEXT);
		}
		for (i = 0; i < dd->projects->len; i++) {
			DsProject *p = g_ptr_array_index(dd->projects, i);
			g_autoptr(GArray) ps = g_array_new(FALSE, FALSE,
				sizeof(BarDevservicesState));
			g_autofree gchar *row_id = NULL;
			g_autofree gchar *value = NULL;
			BarDevservicesAgg agg;
			guint up = 0;
			static const GowlBarColor roles[] = {
				GOWL_BAR_COLOR_SUBTEXT, GOWL_BAR_COLOR_GREEN,
				GOWL_BAR_COLOR_YELLOW, GOWL_BAR_COLOR_RED
			};

			ds_collect_states(dd, p, ps);
			agg = bar_devservices_aggregate((const BarDevservicesState *)
				ps->data, ps->len, FALSE, &up);
			row_id = g_strconcat("project:", p->stack->key, NULL);
			value = g_strdup_printf("%u/%u", up, ps->len);
			item = gowl_bar_panel_add_row(panel, row_id, NULL,
			                              p->stack->name, p->stack->root);
			gowl_bar_panel_item_set_value(item, value);
			gowl_bar_panel_item_set_value_color(item, roles[agg]);
			gowl_bar_panel_item_set_active(item, p == proj);
		}
	}

	/* Two projects wanting one port: only one of them can be up. */
	stacks = g_ptr_array_new();
	for (i = 0; i < dd->projects->len; i++)
		g_ptr_array_add(stacks,
		                ((DsProject *)g_ptr_array_index(dd->projects,
		                                                i))->stack);
	clashes = bar_devservices_port_clashes(stacks);
	if (clashes->len > 0) {
		gowl_bar_panel_add_section(panel, "Port clashes");
		for (i = 0; i < clashes->len && i < 8; i++) {
			item = gowl_bar_panel_add_label(panel,
			                                g_ptr_array_index(clashes, i));
			gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
		}
		gowl_bar_panel_add_label(panel,
			"Move port: change `port' in one stack and in that app's "
			"own configuration.  The widget never rewrites your code.");
	}

	if (dd->load_errors->len > 0) {
		gowl_bar_panel_add_section(panel, "Problems");
		for (i = 0; i < dd->load_errors->len && i < 8; i++) {
			item = gowl_bar_panel_add_label(panel,
			        g_ptr_array_index(dd->load_errors, i));
			gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
		}
	}
	if (dd->podman_missing && dd->needs_podman) {
		item = gowl_bar_panel_add_label(panel,
			"podman is not installed: podman services cannot start. "
			"Fedora packages: podman, podman-compose.");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}
	if (dd->curl_missing && dd->needs_curl) {
		item = gowl_bar_panel_add_label(panel,
			"curl is not installed: URL health checks fall back to "
			"the port.  Fedora package: curl.");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	/* Finding and declaring projects. */
	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool2");
	gowl_bar_panel_add_button(item, dd->scanning ? "Scanning..."
	                                             : "Scan for projects",
	                          FALSE);
	gowl_bar_panel_add_button(item, "Edit stack", FALSE);
	if (dd->scanned && dd->candidates->len == 0)
		gowl_bar_panel_add_label(panel, "No undeclared projects found "
		                         "under the search roots.");
	for (i = 0; i < dd->candidates->len; i++) {
		BarDevservicesCandidate *c = g_ptr_array_index(dd->candidates, i);
		g_autofree gchar *found = NULL;
		g_autofree gchar *row_id = NULL;
		gboolean done = FALSE;
		guint k;

		for (k = 0; k < dd->declared->len; k++)
			if (strcmp(g_ptr_array_index(dd->declared, k), c->key) == 0)
				done = TRUE;
		{
			GString *f = g_string_new(NULL);

			for (k = 0; k < c->found->len; k++)
				g_string_append_printf(f, "%s%s", k ? ", " : "",
				        (const gchar *)g_ptr_array_index(c->found, k));
			found = g_string_free(f, FALSE);
		}
		row_id = g_strconcat(done ? "edit:" : "declare:", c->key, NULL);
		item = gowl_bar_panel_add_row(panel, row_id, "\xef\x81\xbb",
		                              c->key, found);
		gowl_bar_panel_item_set_value(item, done ? "Edit" : "Declare");
		gowl_bar_panel_item_set_value_color(item, done
			? GOWL_BAR_COLOR_GREEN : GOWL_BAR_COLOR_ACCENT);
	}

	g_mutex_unlock(&dd->lock);
	return panel;
}

/* ----------------------------------------------------------------
 * Panel actions (compositor thread)
 * ---------------------------------------------------------------- */

static gchar *
ds_terminal(GowlBarPlugin *plugin)
{
	const gchar *t = gowl_bar_plugin_get_setting(plugin, "terminal");

	if (t == NULL || t[0] == '\0')
		t = g_getenv("TERMINAL");
	return g_strdup((t != NULL && t[0] != '\0') ? t : "gst");
}

/* Open one or more logs in a terminal: `log-command' plus the quoted
   paths, by default `$TERMINAL -e tail -F'. */
static void
ds_open_logs(GowlBarPlugin *plugin, GPtrArray *paths)
{
	g_autofree gchar *base = NULL;
	GString *line;
	guint i;
	const gchar *cmd = gowl_bar_plugin_get_setting(plugin, "log-command");

	if (paths->len == 0)
		return;
	if (cmd != NULL && cmd[0] != '\0') {
		base = g_strdup(cmd);
	} else {
		g_autofree gchar *term = ds_terminal(plugin);

		base = g_strdup_printf("%s -e tail -F", term);
	}
	line = g_string_new(base);
	for (i = 0; i < paths->len; i++) {
		g_autofree gchar *q = g_shell_quote(g_ptr_array_index(paths, i));

		g_string_append_c(line, ' ');
		g_string_append(line, q);
	}
	gowl_bar_plugin_spawn(plugin, line->str);
	g_string_free(line, TRUE);
}

static void
ds_edit(GowlBarPlugin *plugin, const gchar *file)
{
	const gchar *cmd = gowl_bar_plugin_get_setting(plugin, "edit-command");
	g_autofree gchar *base = NULL;
	g_autofree gchar *q = NULL;
	g_autofree gchar *line = NULL;

	if (file == NULL)
		return;
	if (cmd != NULL && cmd[0] != '\0') {
		base = g_strdup(cmd);
	} else if (g_getenv("EDITOR") != NULL && g_getenv("EDITOR")[0] != '\0') {
		g_autofree gchar *term = ds_terminal(plugin);

		base = g_strdup_printf("%s -e %s", term, g_getenv("EDITOR"));
	} else {
		base = g_strdup("xdg-open");
	}
	q = g_shell_quote(file);
	line = g_strdup_printf("%s %s", base, q);
	gowl_bar_plugin_spawn(plugin, line);
}

/* Queue an action for the worker and kick it. */
static void
ds_request(GowlBarPlugin *plugin, DevData *dd, DsOpKind kind,
           const gchar *project, const gchar *task)
{
	gboolean busy;

	g_mutex_lock(&dd->lock);
	busy = (dd->op != NULL || dd->pending != NULL);
	if (!busy) {
		dd->pending = g_new0(DsRequest, 1);
		dd->pending->kind = kind;
		dd->pending->project = g_strdup(project);
		dd->pending->task = g_strdup(task);
	}
	g_mutex_unlock(&dd->lock);

	if (busy) {
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"Dev services are busy",
			"Wait for the current action to finish.");
		return;
	}
	/* The lock is released first: a host that runs work inline
	   would otherwise deadlock on it. */
	gowl_bar_plugin_queue_work(plugin, ds_work, dd, NULL);
}

static void
ds_kick(GowlBarPlugin *plugin, DevData *dd)
{
	gowl_bar_plugin_queue_work(plugin, ds_work, dd, NULL);
}

static void
ds_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	DevData *dd = data;
	DsProject *proj;
	g_autofree gchar *key = NULL;
	g_autofree gchar *file = NULL;
	g_autoptr(GPtrArray) paths = NULL;
	g_autoptr(GPtrArray) urls = NULL;
	g_autofree gchar *state_dir = NULL;
	guint i;

	(void)value;
	(void)button;
	if (item_id == NULL)
		return;

	state_dir = ds_state_dir(plugin);
	paths = g_ptr_array_new_with_free_func(g_free);
	urls = g_ptr_array_new_with_free_func(g_free);

	/* Copy what the action needs out from under the lock first. */
	g_mutex_lock(&dd->lock);
	proj = ds_current(plugin, dd);
	if (proj != NULL) {
		key = g_strdup(proj->stack->key);
		file = g_strdup(proj->stack->file);
		for (i = 0; i < proj->stack->services->len; i++) {
			BarDevservicesService *def;

			def = g_ptr_array_index(proj->stack->services, i);
			if (g_strcmp0(item_id, "tool") == 0 && index == 3)
				g_ptr_array_add(paths, ds_log_path(state_dir, key,
				                                   def->id));
			else if (g_str_has_prefix(item_id, "svc:") &&
			         strcmp(item_id + 4, def->id) == 0)
				g_ptr_array_add(paths, ds_log_path(state_dir, key,
				                                   def->id));
			if (def->url != NULL)
				g_ptr_array_add(urls, g_strdup(def->url));
		}
	}
	g_mutex_unlock(&dd->lock);

	if (g_strcmp0(item_id, "tool") == 0) {
		if (key == NULL)
			return;
		switch (index) {
		case 0:
			ds_request(plugin, dd, DS_OP_UP, key, NULL);
			break;
		case 1:
			ds_request(plugin, dd, DS_OP_DOWN, key, NULL);
			break;
		case 2:
			ds_request(plugin, dd, DS_OP_RESTART, key, NULL);
			break;
		case 3:
			ds_open_logs(plugin, paths);
			break;
		case 4:
			if (urls->len == 0) {
				gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
					"Nothing to open",
					"No service in this stack has a url.");
				break;
			}
			{
				g_autofree gchar *open_cmd = ds_setting(plugin,
					"open-command", "xdg-open");

				for (i = 0; i < urls->len; i++)
					ds_open_url(plugin, open_cmd,
					            g_ptr_array_index(urls, i));
			}
			break;
		default:
			break;
		}
		return;
	}

	if (g_strcmp0(item_id, "tool2") == 0) {
		if (index == 0) {
			g_mutex_lock(&dd->lock);
			dd->scan_requested = TRUE;
			dd->scanning = TRUE;
			g_mutex_unlock(&dd->lock);
			ds_kick(plugin, dd);
		} else if (index == 1) {
			if (file != NULL)
				ds_edit(plugin, file);
			else
				gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
					"No stack to edit",
					"Pick a project first, or declare one.");
		}
		return;
	}

	if (g_str_has_prefix(item_id, "svc:")) {
		ds_open_logs(plugin, paths);
		return;
	}

	if (g_str_has_prefix(item_id, "project:")) {
		g_mutex_lock(&dd->lock);
		ds_set(&dd->pinned, item_id + 8);
		g_mutex_unlock(&dd->lock);
		ds_apply_label(plugin, dd);
		return;
	}
	if (g_strcmp0(item_id, "unpin") == 0) {
		g_mutex_lock(&dd->lock);
		ds_set(&dd->pinned, NULL);
		g_mutex_unlock(&dd->lock);
		ds_apply_label(plugin, dd);
		return;
	}

	if (g_str_has_prefix(item_id, "task:") && key != NULL) {
		gboolean running = FALSE;

		g_mutex_lock(&dd->lock);
		proj = ds_current(plugin, dd);
		for (i = 0; proj != NULL && i < proj->tasks->len; i++) {
			BarDevservicesTask *def = g_ptr_array_index(proj->stack->tasks,
			                                            i);
			DsTask *task = g_ptr_array_index(proj->tasks, i);

			if (strcmp(def->id, item_id + 5) == 0 && task->run != NULL)
				running = TRUE;
		}
		/* A running task: the click stops its group.  On the
		   worker, because checking the start time may scan /proc. */
		if (running)
			ds_set(&dd->stop_task, item_id + 5);
		g_mutex_unlock(&dd->lock);
		if (running) {
			ds_kick(plugin, dd);
			return;
		}
		ds_request(plugin, dd, DS_OP_TASK, key, item_id + 5);
		return;
	}

	if (g_str_has_prefix(item_id, "takeover:")) {
		gboolean fire;

		g_mutex_lock(&dd->lock);
		fire = ds_is_armed(dd, item_id);
		if (fire) {
			ds_set(&dd->armed, NULL);
			ds_set(&dd->takeover, item_id + 9);
		} else {
			ds_set(&dd->armed, item_id);
			dd->armed_at = g_get_monotonic_time();
		}
		g_mutex_unlock(&dd->lock);
		if (fire)
			ds_kick(plugin, dd);
		return;
	}

	if (g_str_has_prefix(item_id, "declare:")) {
		g_mutex_lock(&dd->lock);
		ds_set(&dd->declare_key, item_id + 8);
		g_mutex_unlock(&dd->lock);
		ds_kick(plugin, dd);
		return;
	}

	if (g_str_has_prefix(item_id, "edit:")) {
		g_autofree gchar *dir = ds_config_dir(plugin);
		g_autofree gchar *name = g_strconcat(item_id + 5, ".yaml", NULL);
		g_autofree gchar *path = NULL;

		if (!bar_devservices_valid_id(item_id + 5))
			return;
		path = g_build_filename(dir, name, NULL);
		ds_edit(plugin, path);
		return;
	}
}

static void
ds_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	/* A fresh look when the panel opens, not up to five seconds of
	   stale state. */
	ds_kick(plugin, (DevData *)data);
}

static void
ds_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	DevData *dd = data;

	(void)plugin;
	g_mutex_lock(&dd->lock);
	ds_set(&dd->armed, NULL);
	g_mutex_unlock(&dd->lock);
}

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

static const GowlBarPluginVTable ds_vtable = {
	sizeof(GowlBarPluginVTable),
	ds_create, ds_destroy,
	NULL, NULL, ds_configure,
	ds_interval, ds_poll, ds_poll_async,
	NULL, NULL,
	NULL, NULL,
	ds_panel, ds_action,
	ds_panel_opened, ds_panel_closed,
	NULL,
	NULL,
	NULL, NULL
};

/**
 * bar_register_devservices_plugins:
 * @registry: the plugin registry
 *
 * Registers `devservices' and its alias `devstack'.
 */
void
bar_register_devservices_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "devservices",
		"Dev services",
		"Start, stop and watch a project's development services",
		&ds_vtable);
	gowl_bar_registry_register_alias(registry, "devstack", "devservices");
}
