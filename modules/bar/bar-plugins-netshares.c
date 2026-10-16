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
 * The `netshares' widget: mount and unmount CIFS, NFS and SSHFS shares.
 *
 * Connections live in a YAML file the widget can also write (add and
 * forget from the panel).  A CIFS or NFS mount is `pkexec mount', so
 * the desktop's polkit agent asks for the password once per mount and
 * nothing is added to /etc/fstab; an SSHFS mount is the user's own FUSE
 * mount and asks nobody.  CIFS passwords live in the keyring through
 * `secret-tool' and reach mount.cifs only through a 0600 credentials
 * file in the runtime directory that exists for the length of one
 * `mount' call.
 *
 * Threads.  Everything that waits -- every subprocess, reading the
 * connection file, resolving a mount point (a hung NFS mount blocks
 * realpath()) -- runs on a worker, through poll_async or
 * gowl_bar_plugin_queue_work().  The compositor-thread callbacks only
 * read and flip state under the lock.  A toast raised by a worker is
 * queued and shown by the next sync poll, because notify is not one of
 * the calls a worker may make; the interval drops to one second while
 * anything is queued or running, so that is never a long wait.
 *
 * The decisions that matter for safety (what reaches argv, which paths
 * are acceptable, what the credentials file holds) are in
 * bar-netshares-model.c, where tests reach them directly.
 */

#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <glib/gstdio.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "bar-internal.h"
#include "bar-netshares-model.h"

/* nf-md-folder_network: a folder with a network cable under it. */
#define NS_ICON          "\xf3\xb0\xa1\xb0"
#define NS_ICON_ADD      "\xef\x81\xa7"
#define NS_ICON_CIFS     "\xf3\xb0\x92\x8d"
#define NS_ICON_NFS      "\xf3\xb0\x92\x8d"
#define NS_ICON_SSHFS    "\xef\x84\xa0"

/* How long a destructive button stays armed after its first click. */
#define NS_ARM_US        (5 * G_USEC_PER_SEC)

/* The connection file is a few hundred bytes; anything this size is
   not one, and reading it on a worker would still be a waste. */
#define NS_MAX_CONFIG    (256 * 1024)
#define NS_MAX_MOUNTINFO (4 * 1024 * 1024)

/* ----------------------------------------------------------------
 * State
 * ---------------------------------------------------------------- */

/* What the widget knows about one connection beyond its definition.
   Keyed by id and kept across reloads, so editing the file does not
   forget that a share is mid-mount. */
typedef struct {
	gboolean  mounted;
	gboolean  busy;          /* a mount or unmount is running */
	gboolean  busy_unmount;  /* the last unmount found files open */
	gchar    *error;         /* the last failure, one sanitised line */
	gchar    *canonical;     /* where the mount really lands */
	gint64    forget_armed;  /* monotonic us of the first click, or 0 */
	gint64    lazy_armed;
	gboolean  auto_tried;    /* automount attempted (or not needed) */
} NsState;

typedef struct {
	GowlBarToastUrgency  urgency;
	gchar               *summary;
	gchar               *body;
} NsToast;

typedef struct {
	/* Guards everything below.  Workers swap whole arrays in under it;
	   the panel builder and the actions read under it. */
	GMutex      lock;

	GPtrArray  *shares;        /* BarNetshare*, replaced whole */
	GHashTable *states;        /* id -> NsState* */
	GPtrArray  *warnings;      /* gchar*, from the last load */
	gchar      *config_error;
	gchar      *config_path;   /* what the last load read */
	gint64      config_mtime;
	goffset     config_size;
	gboolean    config_exists;
	gboolean    reload;        /* force a re-read on the next poll */
	gboolean    loaded;
	guint       jobs;          /* queued or running */

	/* What is installed, refreshed every poll. */
	gboolean    have_pkexec;
	gboolean    have_cifs;
	gboolean    have_nfs;
	gboolean    have_sshfs;
	gboolean    have_fusermount;
	gboolean    have_secret;

	GPtrArray  *toasts;        /* NsToast*, shown by the sync poll */

	/* The password field: which connection it is for, and what has
	   been typed so far.  The host owns the text while the panel is
	   open; it reaches us through panel_text. */
	gchar      *password_for;
	gchar      *password_text;
	guint       password_rev;

	/* The add-connection form. */
	gboolean          form_open;
	BarNetsharesType  form_type;
	gchar            *form_name;
	gchar            *form_server;
	gchar            *form_share;
	gchar            *form_user;
	gchar            *form_mountpoint;
	gchar            *form_password;
	gboolean          form_automount;
	gchar            *form_error;
	gboolean          form_busy;
	guint             form_rev;

	/* Panel item index -> connection id, for the panel's keys. */
	GPtrArray  *row_ids;
} Netshares;

/* The settings a job needs, copied: a setting's string is only
   borrowed for the call that read it, and a job outlives that. */
typedef struct {
	gchar *pkexec;
	gchar *sshfs;
	gchar *fusermount;
	gchar *secret_tool;
	gchar *nfs_version;
	gchar *helper_path;
	gchar *mountinfo;
	gchar *config;
	gchar *home;
	gchar *runtime;
	gint   timeout;
} NsEnv;

typedef enum {
	NS_JOB_MOUNT,
	NS_JOB_UNMOUNT,
	NS_JOB_UNMOUNT_LAZY,
	NS_JOB_STORE_PASSWORD,
	NS_JOB_FORGET,
	NS_JOB_SAVE,
	NS_JOB_REFRESH
} NsJobKind;

typedef struct {
	Netshares   *ns;
	NsJobKind    kind;
	BarNetshare *share;
	gchar       *password;     /* secret; wiped on free */
	gboolean     mount_after;  /* SAVE: mount once written */
} NsJob;

static void ns_poll_async(GowlBarPlugin *plugin, gpointer data);

static void
ns_state_free(NsState *st)
{
	if (st == NULL)
		return;
	g_free(st->error);
	g_free(st->canonical);
	g_free(st);
}

static void
ns_toast_free(NsToast *t)
{
	if (t == NULL)
		return;
	g_free(t->summary);
	g_free(t->body);
	g_free(t);
}

static void
ns_job_free(NsJob *job)
{
	if (job == NULL)
		return;
	bar_netshares_share_free(job->share);
	bar_netshares_secret_free(job->password);
	g_free(job);
}

static gpointer
ns_create(GowlBarPlugin *plugin)
{
	Netshares *ns;

	(void)plugin;
	ns = g_new0(Netshares, 1);
	g_mutex_init(&ns->lock);
	ns->shares = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_netshares_share_free);
	ns->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)ns_state_free);
	ns->warnings = g_ptr_array_new_with_free_func(g_free);
	ns->toasts = g_ptr_array_new_with_free_func(
		(GDestroyNotify)ns_toast_free);
	ns->row_ids = g_ptr_array_new_with_free_func(g_free);
	ns->form_type = BAR_NETSHARES_CIFS;
	return ns;
}

static void
ns_form_clear(Netshares *ns)
{
	g_clear_pointer(&ns->form_name, g_free);
	g_clear_pointer(&ns->form_server, g_free);
	g_clear_pointer(&ns->form_share, g_free);
	g_clear_pointer(&ns->form_user, g_free);
	g_clear_pointer(&ns->form_mountpoint, g_free);
	g_clear_pointer(&ns->form_password, bar_netshares_secret_free);
	g_clear_pointer(&ns->form_error, g_free);
	ns->form_automount = FALSE;
	ns->form_type = BAR_NETSHARES_CIFS;
	ns->form_rev++;
}

static void
ns_destroy(GowlBarPlugin *plugin, gpointer data)
{
	Netshares *ns = data;

	(void)plugin;
	if (ns == NULL)
		return;
	ns_form_clear(ns);
	g_ptr_array_unref(ns->shares);
	g_hash_table_unref(ns->states);
	g_ptr_array_unref(ns->warnings);
	g_ptr_array_unref(ns->toasts);
	g_ptr_array_unref(ns->row_ids);
	g_free(ns->config_error);
	g_free(ns->config_path);
	g_free(ns->password_for);
	bar_netshares_secret_free(ns->password_text);
	g_mutex_clear(&ns->lock);
	g_free(ns);
}

/* The state for @id, made on first use.  Caller holds the lock. */
static NsState *
ns_state(Netshares *ns, const gchar *id)
{
	NsState *st;

	st = g_hash_table_lookup(ns->states, id);
	if (st == NULL) {
		st = g_new0(NsState, 1);
		g_hash_table_insert(ns->states, g_strdup(id), st);
	}
	return st;
}

/* Queue a toast for the next sync poll.  Caller holds the lock. */
static void
ns_queue_toast(Netshares *ns, GowlBarToastUrgency urgency,
               const gchar *summary, const gchar *body)
{
	NsToast *t;

	if (ns->toasts->len >= 16)
		return;
	t = g_new0(NsToast, 1);
	t->urgency = urgency;
	t->summary = g_strdup(summary);
	t->body    = g_strdup(body);
	g_ptr_array_add(ns->toasts, t);
}

/* ----------------------------------------------------------------
 * Settings
 * ---------------------------------------------------------------- */

/* Reads a setting with a default, as an owned copy. */
static gchar *
ns_setting(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v;

	v = gowl_bar_plugin_get_setting(plugin, key);
	return g_strdup((v != NULL && v[0] != '\0') ? v : fallback);
}

/* $HOME as it is NOW: g_get_home_dir() caches its first answer, and the
   widget's tests point HOME at a scratch directory. */
static gchar *
ns_home(void)
{
	const gchar *home = g_getenv("HOME");

	return g_strdup((home != NULL && home[0] == '/') ? home
	                                                 : g_get_home_dir());
}

static gchar *
ns_config_path(GowlBarPlugin *plugin)
{
	const gchar *v;
	const gchar *xdg;
	g_autofree gchar *home = NULL;

	v = gowl_bar_plugin_get_setting(plugin, "config");
	if (v != NULL && v[0] != '\0')
		return bar_expand_tilde(v);
	xdg = g_getenv("XDG_CONFIG_HOME");
	if (xdg != NULL && xdg[0] == '/')
		return g_build_filename(xdg, "gowl", "netshares.yaml", NULL);
	home = ns_home();
	return g_build_filename(home, ".config", "gowl", "netshares.yaml",
	                        NULL);
}

static void
ns_env_load(GowlBarPlugin *plugin, NsEnv *env)
{
	const gchar *rt;
	const gchar *hp;

	env->pkexec      = ns_setting(plugin, "pkexec", "pkexec");
	env->sshfs       = ns_setting(plugin, "sshfs", "sshfs");
	env->fusermount  = ns_setting(plugin, "fusermount", "fusermount3");
	env->secret_tool = ns_setting(plugin, "secret-tool", "secret-tool");
	env->nfs_version = ns_setting(plugin, "nfs-version", "");
	env->mountinfo   = ns_setting(plugin, "mountinfo",
	                              "/proc/self/mountinfo");
	env->config      = ns_config_path(plugin);
	env->home        = ns_home();
	/* NULL means `PATH plus the sbin directories'. */
	hp = gowl_bar_plugin_get_setting(plugin, "helper-path");
	env->helper_path = (hp != NULL) ? g_strdup(hp) : NULL;
	env->timeout     = CLAMP(gowl_bar_plugin_get_setting_int(plugin,
	                                                         "timeout", 30),
	                         1, 600);
	rt = g_getenv("XDG_RUNTIME_DIR");
	env->runtime = g_build_filename(
		(rt != NULL && rt[0] == '/') ? rt : g_get_user_runtime_dir(),
		"gowl-netshares", NULL);
}

static void
ns_env_clear(NsEnv *env)
{
	g_free(env->pkexec);
	g_free(env->sshfs);
	g_free(env->fusermount);
	g_free(env->secret_tool);
	g_free(env->nfs_version);
	g_free(env->helper_path);
	g_free(env->mountinfo);
	g_free(env->config);
	g_free(env->home);
	g_free(env->runtime);
	memset(env, 0, sizeof(*env));
}

/* The FUSE unmount helper: fusermount3, else fusermount (FUSE 2). */
static gchar *
ns_fusermount(const NsEnv *env)
{
	gchar *found;

	found = bar_netshares_find_program(env->fusermount, env->helper_path);
	if (found == NULL && g_strcmp0(env->fusermount, "fusermount3") == 0)
		found = bar_netshares_find_program("fusermount",
		                                   env->helper_path);
	return found;
}

/*
 * ns_missing:
 * @ns: the widget (lock held)
 * @type: a connection type
 * @needs_secret: whether the keyring is needed (CIFS with a user)
 *
 * Returns: (transfer full) (nullable): "program (package), ..." for
 *   what @type needs and this machine lacks, or %NULL
 */
static gchar *
ns_missing(Netshares *ns, BarNetsharesType type, gboolean needs_secret)
{
	GString *out = g_string_new(NULL);

#define NS_NEED(flag, prog) \
	if (!(flag)) \
		g_string_append_printf(out, "%s%s (%s)", \
			out->len > 0 ? ", " : "", prog, \
			bar_netshares_package_for(prog))

	switch (type) {
	case BAR_NETSHARES_CIFS:
		NS_NEED(ns->have_cifs, "mount.cifs");
		NS_NEED(ns->have_pkexec, "pkexec");
		if (needs_secret)
			NS_NEED(ns->have_secret, "secret-tool");
		break;
	case BAR_NETSHARES_NFS:
		NS_NEED(ns->have_nfs, "mount.nfs");
		NS_NEED(ns->have_pkexec, "pkexec");
		break;
	case BAR_NETSHARES_SSHFS:
	case BAR_NETSHARES_N_TYPES:
	default:
		NS_NEED(ns->have_sshfs, "sshfs");
		NS_NEED(ns->have_fusermount, "fusermount3");
		break;
	}
#undef NS_NEED

	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * The bar
 * ---------------------------------------------------------------- */

static gint
ns_interval(GowlBarPlugin *plugin, gpointer data)
{
	Netshares *ns = data;
	gboolean hurry;

	g_mutex_lock(&ns->lock);
	hurry = ns->jobs > 0 || ns->toasts->len > 0 || !ns->loaded;
	g_mutex_unlock(&ns->lock);
	/* Once a second while something is in flight: that is also how
	   soon a worker's toast is shown. */
	if (hurry)
		return 1;
	return CLAMP(gowl_bar_plugin_get_setting_int(plugin, "interval", 10),
	             2, 3600);
}

/*
 * ns_apply_label:
 *
 * Icon, count and colour.  Only the thread-safe setters are used, so
 * a worker may call this as well as the sync poll.
 *
 *   idle  (color-idle, SUBTEXT): nothing configured, or nothing mounted
 *         and nothing expected to be;
 *   warn  (color-warn, YELLOW):  an automount share is down, or the
 *         last action on a share failed;
 *   ok    (color-ok,   GREEN):   otherwise.
 */
static void
ns_apply_label(GowlBarPlugin *plugin, Netshares *ns)
{
	guint i, total, mounted = 0, down_auto = 0, failed = 0;
	GString *names;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *count = NULL;
	gboolean show_count;
	const gchar *problem = NULL;

	names = g_string_new(NULL);

	g_mutex_lock(&ns->lock);
	total = ns->shares->len;
	for (i = 0; i < total; i++) {
		BarNetshare *s = g_ptr_array_index(ns->shares, i);
		NsState *st = g_hash_table_lookup(ns->states, s->id);
		gboolean up = (st != NULL && st->mounted);

		if (up) {
			mounted++;
			g_string_append_printf(names, "%s%s",
				names->len > 0 ? ", " : "", s->name);
		} else if (s->automount && (st == NULL || !st->busy)) {
			down_auto++;
		}
		if (st != NULL && st->error != NULL)
			failed++;
	}
	if (ns->config_error != NULL)
		problem = "the connection file has an error";
	g_mutex_unlock(&ns->lock);

	gowl_bar_plugin_set_icon(plugin, NS_ICON);

	show_count = gowl_bar_plugin_get_setting_bool(plugin, "show-count",
	                                              TRUE);
	if (show_count && mounted > 0)
		count = g_strdup_printf("%u", mounted);
	gowl_bar_plugin_set_label(plugin, count);

	if (total == 0 && problem == NULL) {
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-idle"),
			GOWL_BAR_COLOR_SUBTEXT);
		gowl_bar_plugin_set_tooltip(plugin,
			"Network shares: none configured");
		g_string_free(names, TRUE);
		return;
	}

	if (down_auto > 0 || failed > 0 || problem != NULL) {
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-warn"),
			GOWL_BAR_COLOR_YELLOW);
	} else if (mounted > 0) {
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-ok"),
			GOWL_BAR_COLOR_GREEN);
	} else {
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-idle"),
			GOWL_BAR_COLOR_SUBTEXT);
	}

	tip = g_strdup_printf("Network shares: %u of %u mounted%s%s%s%s",
		mounted, total,
		names->len > 0 ? " (" : "", names->str,
		names->len > 0 ? ")" : "",
		problem != NULL ? " -- the connection file has an error"
		: (failed > 0 ? " -- a share needs attention" : ""));
	gowl_bar_plugin_set_tooltip(plugin, tip);
	g_string_free(names, TRUE);
}

/* Show what the workers queued.  Compositor thread only: this is the
   one place the widget raises toasts it did not raise in an action. */
static void
ns_flush_toasts(GowlBarPlugin *plugin, Netshares *ns)
{
	g_autoptr(GPtrArray) pending = NULL;
	guint i;

	g_mutex_lock(&ns->lock);
	if (ns->toasts->len == 0) {
		g_mutex_unlock(&ns->lock);
		return;
	}
	pending = ns->toasts;
	ns->toasts = g_ptr_array_new_with_free_func(
		(GDestroyNotify)ns_toast_free);
	g_mutex_unlock(&ns->lock);

	for (i = 0; i < pending->len; i++) {
		NsToast *t = g_ptr_array_index(pending, i);

		gowl_bar_plugin_notify(plugin, t->urgency, t->summary, t->body);
	}
}

static void
ns_poll(GowlBarPlugin *plugin, gpointer data)
{
	Netshares *ns = data;

	ns_flush_toasts(plugin, ns);
	ns_apply_label(plugin, ns);
}

static void
ns_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	Netshares *ns = data;
	g_autofree gchar *path = NULL;

	(void)settings;
	/* A different `config' means a different file: read it on the
	   next poll rather than trusting the mtime of the old one. */
	path = ns_config_path(plugin);
	g_mutex_lock(&ns->lock);
	if (g_strcmp0(path, ns->config_path) != 0)
		ns->reload = TRUE;
	g_mutex_unlock(&ns->lock);
	ns_apply_label(plugin, ns);
}

/* ----------------------------------------------------------------
 * Worker: reading the world
 * ---------------------------------------------------------------- */

/*
 * ns_read_config:
 * @env: the settings
 * @shares: (out): the connections
 * @warnings: (out): the load's warnings
 * @exists: (out): whether the file is there
 * @error: return location for a #GError
 *
 * A missing file is not an error: it is the first run.
 */
static gboolean
ns_read_config(const NsEnv *env, GPtrArray **shares, GPtrArray **warnings,
               gboolean *exists, GError **error)
{
	g_autofree gchar *text = NULL;
	gsize len = 0;
	GStatBuf st;

	*exists = FALSE;
	if (g_stat(env->config, &st) != 0) {
		*shares = g_ptr_array_new_with_free_func(
			(GDestroyNotify)bar_netshares_share_free);
		*warnings = g_ptr_array_new_with_free_func(g_free);
		return TRUE;
	}
	*exists = TRUE;
	if (!S_ISREG(st.st_mode) || st.st_size > NS_MAX_CONFIG) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "%s is not a regular file under 256 KiB",
		            env->config);
		return FALSE;
	}
	if (!g_file_get_contents(env->config, &text, &len, error))
		return FALSE;
	*shares = bar_netshares_load(text, warnings, error);
	return *shares != NULL;
}

static GPtrArray *
ns_read_mounts(const NsEnv *env)
{
	g_autofree gchar *text = NULL;
	gsize len = 0;

	if (!g_file_get_contents(env->mountinfo, &text, &len, NULL) ||
	    len > NS_MAX_MOUNTINFO)
		return bar_netshares_parse_mountinfo(NULL);
	return bar_netshares_parse_mountinfo(text);
}

/* Is @share mounted, according to @mounts?  Resolves its mount point
   (on the worker -- that touches the filesystem).  */
static gboolean
ns_is_mounted(const NsEnv *env, GPtrArray *mounts, const BarNetshare *share,
              gchar **canonical)
{
	gboolean privileged;

	if (!bar_netshares_resolve_mountpoint(share->mountpoint, env->home,
	                                      canonical, &privileged, NULL))
		return FALSE;
	return bar_netshares_find_mount(mounts, share->type, *canonical)
		!= NULL;
}

static void ns_do_mount(GowlBarPlugin *plugin, Netshares *ns,
                        const NsEnv *env, const BarNetshare *share,
                        const gchar *password);

/*
 * ns_poll_async:
 *
 * Re-reads the connection file when it changed, what is installed, and
 * the mount table; then mounts any automount connection not yet tried
 * this session.
 */
static void
ns_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	Netshares *ns = data;
	NsEnv env;
	g_autoptr(GPtrArray) mounts = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	g_autoptr(GPtrArray) automount = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *fuser = NULL;
	gboolean reload, exists = FALSE, have[6];
	GStatBuf st;
	gint64 mtime = 0;
	goffset size = 0;
	gboolean stat_ok;
	guint i;

	ns_env_load(plugin, &env);

	/* What is installed.  Cheap enough to ask every poll, and it means
	   installing cifs-utils is noticed without a restart. */
	{
		g_autofree gchar *a = bar_netshares_find_program(env.pkexec, NULL);
		g_autofree gchar *b = bar_netshares_find_program("mount.cifs",
		                                                env.helper_path);
		g_autofree gchar *c = bar_netshares_find_program("mount.nfs",
		                                                env.helper_path);
		g_autofree gchar *d = bar_netshares_find_program(env.sshfs, NULL);
		g_autofree gchar *e = bar_netshares_find_program(env.secret_tool,
		                                                NULL);

		fuser = ns_fusermount(&env);
		have[0] = a != NULL;
		have[1] = b != NULL;
		have[2] = c != NULL;
		have[3] = d != NULL;
		have[4] = fuser != NULL;
		have[5] = e != NULL;
	}

	stat_ok = (g_stat(env.config, &st) == 0);
	if (stat_ok) {
		mtime = (gint64)st.st_mtime * G_USEC_PER_SEC;
#ifdef __linux__
		mtime += st.st_mtim.tv_nsec / 1000;
#endif
		size = st.st_size;
	}

	g_mutex_lock(&ns->lock);
	ns->have_pkexec     = have[0];
	ns->have_cifs       = have[1];
	ns->have_nfs        = have[2];
	ns->have_sshfs      = have[3];
	ns->have_fusermount = have[4];
	ns->have_secret     = have[5];
	reload = ns->reload || !ns->loaded ||
	         g_strcmp0(ns->config_path, env.config) != 0 ||
	         stat_ok != ns->config_exists ||
	         mtime != ns->config_mtime || size != ns->config_size;
	ns->reload = FALSE;
	g_mutex_unlock(&ns->lock);

	if (reload) {
		if (ns_read_config(&env, &shares, &warnings, &exists, &error)) {
			g_mutex_lock(&ns->lock);
			g_ptr_array_unref(ns->shares);
			ns->shares = g_steal_pointer(&shares);
			g_ptr_array_unref(ns->warnings);
			ns->warnings = g_steal_pointer(&warnings);
			g_clear_pointer(&ns->config_error, g_free);
			/* Forget the state of connections that are gone. */
			{
				GHashTableIter it;
				gpointer key;

				g_hash_table_iter_init(&it, ns->states);
				while (g_hash_table_iter_next(&it, &key, NULL))
					if (bar_netshares_find(ns->shares, key) == NULL)
						g_hash_table_iter_remove(&it);
			}
			g_mutex_unlock(&ns->lock);
		} else {
			g_autofree gchar *msg = NULL;
			gboolean first;

			msg = g_utf8_make_valid(error->message, -1);
			g_mutex_lock(&ns->lock);
			/* Toast once per distinct error, not every poll. */
			first = g_strcmp0(ns->config_error, msg) != 0;
			g_free(ns->config_error);
			ns->config_error = g_strdup(msg);
			if (first)
				ns_queue_toast(ns, GOWL_BAR_TOAST_NORMAL,
					"Network shares: the connection file "
					"has an error", msg);
			g_mutex_unlock(&ns->lock);
		}
		g_mutex_lock(&ns->lock);
		g_free(ns->config_path);
		ns->config_path   = g_strdup(env.config);
		ns->config_mtime  = mtime;
		ns->config_size   = size;
		ns->config_exists = stat_ok;
		ns->loaded        = TRUE;
		g_mutex_unlock(&ns->lock);
	}

	/* The mount table, matched against a snapshot of the connections
	   taken under the lock: resolving each mount point touches the
	   filesystem and must not happen with the lock held. */
	mounts = ns_read_mounts(&env);
	g_mutex_lock(&ns->lock);
	shares = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_netshares_share_free);
	for (i = 0; i < ns->shares->len; i++)
		g_ptr_array_add(shares, bar_netshares_share_copy(
			g_ptr_array_index(ns->shares, i)));
	g_mutex_unlock(&ns->lock);

	automount = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_netshares_share_free);
	for (i = 0; i < shares->len; i++) {
		BarNetshare *s = g_ptr_array_index(shares, i);
		g_autofree gchar *canonical = NULL;
		gboolean up;
		NsState *state;

		up = ns_is_mounted(&env, mounts, s, &canonical);
		g_mutex_lock(&ns->lock);
		state = ns_state(ns, s->id);
		/* A share mid-mount is the job's to report; a poll that
		   read the table a moment before the mount finished would
		   otherwise flip it back. */
		if (!state->busy) {
			/* Coming up clears a failure to come up; an unmount
			   that failed leaves the share mounted, so its error
			   (busy) survives. */
			if (up && !state->mounted)
				g_clear_pointer(&state->error, g_free);
			state->mounted = up;
			if (!up)
				state->busy_unmount = FALSE;
		}
		g_free(state->canonical);
		state->canonical = g_steal_pointer(&canonical);
		/* Automount once per connection per session: a share that is
		   already up, or that the user later unmounts, is not
		   mounted again behind their back. */
		if (s->automount && !state->auto_tried) {
			state->auto_tried = TRUE;
			if (!up && !state->busy) {
				state->busy = TRUE;
				ns->jobs++;
				g_ptr_array_add(automount,
				                bar_netshares_share_copy(s));
			}
		}
		g_mutex_unlock(&ns->lock);
	}

	/* Automount, in order.  Each may raise a polkit prompt, which is
	   why it is opt-in per connection. */
	for (i = 0; i < automount->len; i++) {
		ns_do_mount(plugin, ns, &env, g_ptr_array_index(automount, i),
		            NULL);
		g_mutex_lock(&ns->lock);
		ns->jobs--;
		g_mutex_unlock(&ns->lock);
	}

	ns_apply_label(plugin, ns);
	gowl_bar_plugin_request_panel_refresh(plugin);
	ns_env_clear(&env);
}

/* ----------------------------------------------------------------
 * Worker: acting on the world
 * ---------------------------------------------------------------- */

/* Record a failure on @id and say so.  Takes the lock. */
static void
ns_fail(Netshares *ns, const BarNetshare *share, const gchar *summary,
        const gchar *why)
{
	NsState *st;
	g_autofree gchar *head = NULL;

	g_mutex_lock(&ns->lock);
	st = ns_state(ns, share->id);
	g_free(st->error);
	st->error = g_strdup(why);
	head = g_strdup_printf(summary, share->name);
	ns_queue_toast(ns, GOWL_BAR_TOAST_NORMAL, head, why);
	g_mutex_unlock(&ns->lock);
}

/*
 * ns_lookup_password:
 *
 * `secret-tool lookup'.  The answer is a password, so the run's output
 * is wiped on clear and never logged.
 *
 * Returns: (transfer full) (nullable): the password; free with
 *   bar_netshares_secret_free()
 */
static gchar *
ns_lookup_password(const NsEnv *env, const BarNetshare *share)
{
	g_auto(GStrv) argv = NULL;
	BarNetsharesRun run;
	gchar *secret = NULL;

	argv = bar_netshares_secret_argv(env->secret_tool, "lookup",
	                                 share->id, NULL);
	if (!bar_netshares_run((const gchar * const *)argv, NULL,
	                       env->timeout, &run, NULL))
		return NULL;
	if (run.status == 0 && run.out != NULL && run.out[0] != '\0') {
		gsize len = strlen(run.out);

		/* secret-tool prints a newline after the secret when the
		   output is a terminal; strip one, never more. */
		if (len > 0 && run.out[len - 1] == '\n')
			run.out[len - 1] = '\0';
		secret = g_strdup(run.out);
	}
	bar_netshares_run_clear(&run);
	return secret;
}

/*
 * ns_do_mount:
 * @password: (nullable): use this rather than asking the keyring (the
 *   add form's Save & mount has just been given it)
 *
 * Mounts one connection, start to finish.  Worker only.  The caller
 * has set the state busy; this clears it.
 */
static void
ns_do_mount(GowlBarPlugin *plugin, Netshares *ns, const NsEnv *env,
            const BarNetshare *share, const gchar *password)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) mounts = NULL;
	g_autofree gchar *canonical = NULL;
	g_autofree gchar *cred = NULL;
	g_autofree gchar *missing = NULL;
	g_autofree gchar *why = NULL;
	g_auto(GStrv) argv = NULL;
	gchar *secret = NULL;
	gboolean privileged = FALSE, create = FALSE, needs_secret;
	BarNetsharesTools tools;
	BarNetsharesRun run;
	NsState *st;

	needs_secret = share->type == BAR_NETSHARES_CIFS &&
	               share->user != NULL && share->user[0] != '\0';

	g_mutex_lock(&ns->lock);
	missing = ns_missing(ns, share->type, needs_secret && password == NULL);
	g_mutex_unlock(&ns->lock);
	if (missing != NULL) {
		why = g_strdup_printf("Not installed: %s", missing);
		ns_fail(ns, share, "Cannot mount %s", why);
		goto done;
	}

	if (!bar_netshares_resolve_mountpoint(share->mountpoint, env->home,
	                                      &canonical, &privileged,
	                                      &error)) {
		ns_fail(ns, share, "Cannot mount %s", error->message);
		goto done;
	}

	mounts = ns_read_mounts(env);
	if (bar_netshares_find_mount(mounts, share->type, canonical) != NULL) {
		g_mutex_lock(&ns->lock);
		ns_state(ns, share->id)->mounted = TRUE;
		g_mutex_unlock(&ns->lock);
		goto done;
	}

	if (!g_file_test(canonical, G_FILE_TEST_IS_DIR)) {
		if (!privileged) {
			if (g_mkdir_with_parents(canonical, 0700) != 0) {
				why = g_strdup_printf("cannot create %s: %s",
				                      canonical, g_strerror(errno));
				ns_fail(ns, share, "Cannot mount %s", why);
				goto done;
			}
		} else if (share->type == BAR_NETSHARES_SSHFS) {
			/* sshfs runs as you, and you cannot create a folder
			   in /mnt; root making it for a FUSE mount it then
			   does not own would not help either. */
			why = g_strdup_printf("%s does not exist; create it and "
			                      "make it yours first", canonical);
			ns_fail(ns, share, "Cannot mount %s", why);
			goto done;
		} else {
			create = TRUE;
		}
	}

	if (needs_secret) {
		secret = (password != NULL) ? g_strdup(password)
		                            : ns_lookup_password(env, share);
		if (secret == NULL) {
			ns_fail(ns, share, "Cannot mount %s",
			        "No password is stored for this share; set "
			        "one with Password");
			g_mutex_lock(&ns->lock);
			g_free(ns->password_for);
			ns->password_for = g_strdup(share->id);
			ns->password_rev++;
			g_mutex_unlock(&ns->lock);
			goto done;
		}
		cred = bar_netshares_write_credentials(env->runtime, share->id,
		                                       share->user, secret,
		                                       &error);
		if (cred == NULL) {
			ns_fail(ns, share, "Cannot mount %s", error->message);
			goto done;
		}
	}

	tools.pkexec     = env->pkexec;
	tools.sshfs      = env->sshfs;
	tools.fusermount = env->fusermount;
	argv = bar_netshares_mount_argv(share, canonical, &tools, cred,
	                                (guint)getuid(), (guint)getgid(),
	                                create, env->nfs_version);

	if (!bar_netshares_run((const gchar * const *)argv, NULL, env->timeout,
	                       &run, &error)) {
		if (cred != NULL)
			g_unlink(cred);
		ns_fail(ns, share, "Cannot mount %s", error->message);
		goto done;
	}
	/* The credentials file lives exactly as long as the mount call,
	   success or not. */
	if (cred != NULL)
		g_unlink(cred);

	if (run.timed_out) {
		why = g_strdup_printf("timed out after %d s (is a polkit agent "
		                      "running, and the server reachable?)",
		                      env->timeout);
	} else if (run.status != 0) {
		why = bar_netshares_last_line(run.err, secret);
		if (why == NULL)
			why = g_strdup_printf("mount failed (exit %d)", run.status);
	}
	bar_netshares_run_clear(&run);

	if (why != NULL) {
		ns_fail(ns, share, "Could not mount %s", why);
		goto done;
	}

	g_mutex_lock(&ns->lock);
	st = ns_state(ns, share->id);
	st->mounted = TRUE;
	st->busy_unmount = FALSE;
	g_clear_pointer(&st->error, g_free);
	g_free(st->canonical);
	st->canonical = g_strdup(canonical);
	{
		g_autofree gchar *head = g_strdup_printf("Mounted %s",
		                                         share->name);

		ns_queue_toast(ns, GOWL_BAR_TOAST_LOW, head, canonical);
	}
	g_mutex_unlock(&ns->lock);

done:
	bar_netshares_secret_free(secret);
	g_mutex_lock(&ns->lock);
	ns_state(ns, share->id)->busy = FALSE;
	g_mutex_unlock(&ns->lock);
	ns_apply_label(plugin, ns);
}

static void
ns_do_unmount(GowlBarPlugin *plugin, Netshares *ns, const NsEnv *env,
              const BarNetshare *share, gboolean lazy)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *canonical = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *fuser = NULL;
	g_auto(GStrv) argv = NULL;
	BarNetsharesTools tools;
	BarNetsharesRun run;
	gboolean busy = FALSE;
	NsState *st;

	if (!bar_netshares_resolve_mountpoint(share->mountpoint, env->home,
	                                      &canonical, NULL, &error)) {
		ns_fail(ns, share, "Cannot unmount %s", error->message);
		goto done;
	}

	if (share->type == BAR_NETSHARES_SSHFS) {
		fuser = ns_fusermount(env);
		if (fuser == NULL) {
			ns_fail(ns, share, "Cannot unmount %s",
			        "Not installed: fusermount3 (fuse3)");
			goto done;
		}
	}

	tools.pkexec     = env->pkexec;
	tools.sshfs      = env->sshfs;
	tools.fusermount = (fuser != NULL) ? fuser : env->fusermount;
	argv = bar_netshares_unmount_argv(share, canonical, &tools, lazy);

	if (!bar_netshares_run((const gchar * const *)argv, NULL, env->timeout,
	                       &run, &error)) {
		ns_fail(ns, share, "Cannot unmount %s", error->message);
		goto done;
	}
	if (run.timed_out) {
		why = g_strdup_printf("timed out after %d s", env->timeout);
	} else if (run.status != 0) {
		busy = bar_netshares_is_busy(run.err);
		why = busy ? g_strdup("busy: files on it are still open")
		           : bar_netshares_last_line(run.err, NULL);
		if (why == NULL)
			why = g_strdup_printf("unmount failed (exit %d)",
			                      run.status);
	}
	bar_netshares_run_clear(&run);

	if (why != NULL) {
		ns_fail(ns, share, "Could not unmount %s", why);
		g_mutex_lock(&ns->lock);
		ns_state(ns, share->id)->busy_unmount = busy;
		g_mutex_unlock(&ns->lock);
		goto done;
	}

	g_mutex_lock(&ns->lock);
	st = ns_state(ns, share->id);
	st->mounted = FALSE;
	st->busy_unmount = FALSE;
	g_clear_pointer(&st->error, g_free);
	{
		g_autofree gchar *head = g_strdup_printf("Unmounted %s",
		                                         share->name);

		ns_queue_toast(ns, GOWL_BAR_TOAST_LOW, head, NULL);
	}
	g_mutex_unlock(&ns->lock);

done:
	g_mutex_lock(&ns->lock);
	ns_state(ns, share->id)->busy = FALSE;
	g_mutex_unlock(&ns->lock);
	ns_apply_label(plugin, ns);
}

/* `secret-tool store', the password on stdin.  Returns success. */
static gboolean
ns_store_password(Netshares *ns, const NsEnv *env, const BarNetshare *share,
                  const gchar *password)
{
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *missing = NULL;
	BarNetsharesRun run;

	g_mutex_lock(&ns->lock);
	if (!ns->have_secret)
		missing = g_strdup("Not installed: secret-tool (libsecret)");
	g_mutex_unlock(&ns->lock);
	if (missing != NULL) {
		ns_fail(ns, share, "Cannot save the password for %s", missing);
		return FALSE;
	}

	argv = bar_netshares_secret_argv(env->secret_tool, "store", share->id,
	                                 share->name);
	if (!bar_netshares_run((const gchar * const *)argv, password,
	                       env->timeout, &run, NULL)) {
		ns_fail(ns, share, "Cannot save the password for %s",
		        "secret-tool could not be started");
		return FALSE;
	}
	if (run.timed_out)
		why = g_strdup("timed out: is the keyring unlocked?");
	else if (run.status != 0)
		why = bar_netshares_last_line(run.err, password);
	if (why == NULL && run.status != 0)
		why = g_strdup_printf("secret-tool failed (exit %d)", run.status);
	bar_netshares_run_clear(&run);

	if (why != NULL) {
		ns_fail(ns, share, "Cannot save the password for %s", why);
		return FALSE;
	}
	return TRUE;
}

/*
 * ns_write_config:
 *
 * Writes @shares atomically at 0600.  A symlinked file (a dotfiles
 * checkout) is written THROUGH: replacing the link with a file would
 * silently fork the user's configuration.
 */
static gboolean
ns_write_config(const NsEnv *env, GPtrArray *shares, GError **error)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *target = NULL;
	gchar *real;

	real = realpath(env->config, NULL);
	target = g_strdup(real != NULL ? real : env->config);
	free(real);

	dir = g_path_get_dirname(target);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "cannot create %s", dir);
		return FALSE;
	}
	text = bar_netshares_to_yaml(shares);
	if (!g_file_set_contents_full(target, text, -1,
	                              G_FILE_SET_CONTENTS_CONSISTENT, 0600,
	                              error))
		return FALSE;
	/* GLib keeps an existing file's mode over the one asked for, so a
	   hand-made 0644 file would stay readable to every local user.
	   It holds no secret, but it does map out the user's servers. */
	g_chmod(target, 0600);
	return TRUE;
}

/*
 * ns_load_for_edit:
 *
 * The file as it is on disk, for add or forget.  A file with any
 * warning is refused: rewriting it would drop the connections and keys
 * the loader refused, and losing someone's half-written entry to a
 * click elsewhere in the panel is worse than asking them to fix it.
 */
static GPtrArray *
ns_load_for_edit(const NsEnv *env, GError **error)
{
	g_autoptr(GPtrArray) shares = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	gboolean exists;

	if (!ns_read_config(env, &shares, &warnings, &exists, error))
		return NULL;
	if (warnings != NULL && warnings->len > 0) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "%s has problems (%s); fix them first, or saving "
		            "would drop them",
		            env->config,
		            (const gchar *)g_ptr_array_index(warnings, 0));
		return NULL;
	}
	return g_steal_pointer(&shares);
}

static void
ns_do_forget(Netshares *ns, const NsEnv *env, const BarNetshare *share)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	g_auto(GStrv) argv = NULL;
	BarNetsharesRun run;
	guint i;
	gboolean found = FALSE;

	shares = ns_load_for_edit(env, &error);
	if (shares == NULL) {
		ns_fail(ns, share, "Cannot forget %s", error->message);
		return;
	}
	for (i = 0; i < shares->len; i++) {
		BarNetshare *s = g_ptr_array_index(shares, i);

		if (g_strcmp0(s->id, share->id) == 0) {
			g_ptr_array_remove_index(shares, i);
			found = TRUE;
			break;
		}
	}
	if (!found) {
		ns_fail(ns, share, "Cannot forget %s",
		        "it is no longer in the connection file");
		return;
	}
	if (!ns_write_config(env, shares, &error)) {
		ns_fail(ns, share, "Cannot forget %s", error->message);
		return;
	}

	/* The keyring entry goes too.  A share with no secret makes
	   secret-tool exit non-zero, which is not worth reporting. */
	argv = bar_netshares_secret_argv(env->secret_tool, "clear", share->id,
	                                 NULL);
	if (bar_netshares_run((const gchar * const *)argv, NULL, env->timeout,
	                      &run, NULL))
		bar_netshares_run_clear(&run);

	g_mutex_lock(&ns->lock);
	ns->reload = TRUE;
	g_hash_table_remove(ns->states, share->id);
	{
		g_autofree gchar *head = g_strdup_printf("Forgot %s",
		                                         share->name);

		ns_queue_toast(ns, GOWL_BAR_TOAST_LOW, head,
		               "Removed from the file and the keyring");
	}
	g_mutex_unlock(&ns->lock);
}

/* Worker half of Save and Save & mount from the add form. */
static void
ns_do_save(GowlBarPlugin *plugin, Netshares *ns, const NsEnv *env,
           NsJob *job)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) shares = NULL;
	g_autofree gchar *canonical = NULL;
	g_autofree gchar *id = NULL;
	BarNetshare *share = job->share;
	gboolean privileged;

	if (!bar_netshares_resolve_mountpoint(share->mountpoint, env->home,
	                                      &canonical, &privileged,
	                                      &error))
		goto fail;

	shares = ns_load_for_edit(env, &error);
	if (shares == NULL)
		goto fail;

	/* The id was suggested on the compositor thread against what was
	   loaded then; the file is the authority. */
	id = bar_netshares_slug(share->name, shares);
	g_free(share->id);
	share->id = g_steal_pointer(&id);

	g_ptr_array_add(shares, bar_netshares_share_copy(share));
	if (!ns_write_config(env, shares, &error))
		goto fail;

	if (job->password != NULL && job->password[0] != '\0')
		ns_store_password(ns, env, share, job->password);

	g_mutex_lock(&ns->lock);
	ns->reload = TRUE;
	ns->form_busy = FALSE;
	ns->form_open = FALSE;
	ns_form_clear(ns);
	{
		g_autofree gchar *head = g_strdup_printf("Saved %s", share->name);

		ns_queue_toast(ns, GOWL_BAR_TOAST_LOW, head, env->config);
	}
	if (job->mount_after) {
		ns_state(ns, share->id)->busy = TRUE;
		/* Shown in the list straight away rather than at the next
		   poll, so the mount has a row to be busy on. */
		if (bar_netshares_find(ns->shares, share->id) == NULL)
			g_ptr_array_add(ns->shares,
			                bar_netshares_share_copy(share));
	}
	g_mutex_unlock(&ns->lock);

	if (job->mount_after)
		ns_do_mount(plugin, ns, env, share, job->password);
	return;

fail:
	g_mutex_lock(&ns->lock);
	ns->form_busy = FALSE;
	g_free(ns->form_error);
	ns->form_error = g_utf8_make_valid(error->message, -1);
	g_mutex_unlock(&ns->lock);
}

static void
ns_job_run(GowlBarPlugin *plugin, gpointer user_data)
{
	NsJob *job = user_data;
	Netshares *ns = job->ns;
	NsEnv env;

	ns_env_load(plugin, &env);

	switch (job->kind) {
	case NS_JOB_MOUNT:
		ns_do_mount(plugin, ns, &env, job->share, NULL);
		break;
	case NS_JOB_UNMOUNT:
	case NS_JOB_UNMOUNT_LAZY:
		ns_do_unmount(plugin, ns, &env, job->share,
		              job->kind == NS_JOB_UNMOUNT_LAZY);
		break;
	case NS_JOB_STORE_PASSWORD:
		if (ns_store_password(ns, &env, job->share, job->password)) {
			g_autofree gchar *head = NULL;
			NsState *st;

			head = g_strdup_printf("Password saved for %s",
			                       job->share->name);
			g_mutex_lock(&ns->lock);
			st = ns_state(ns, job->share->id);
			g_clear_pointer(&st->error, g_free);
			ns_queue_toast(ns, GOWL_BAR_TOAST_LOW, head,
			               "Stored in the keyring");
			g_mutex_unlock(&ns->lock);
		}
		break;
	case NS_JOB_FORGET:
		ns_do_forget(ns, &env, job->share);
		/* On success the state is gone; on failure it must not stay
		   busy, or the row could never be clicked again. */
		g_mutex_lock(&ns->lock);
		if (g_hash_table_lookup(ns->states, job->share->id) != NULL)
			ns_state(ns, job->share->id)->busy = FALSE;
		g_mutex_unlock(&ns->lock);
		break;
	case NS_JOB_SAVE:
		ns_do_save(plugin, ns, &env, job);
		break;
	case NS_JOB_REFRESH:
	default:
		g_mutex_lock(&ns->lock);
		ns->reload = TRUE;
		g_mutex_unlock(&ns->lock);
		break;
	}

	/* Every job ends with a fresh look at the file and the table, so
	   the panel shows what is, not what we expect. */
	g_mutex_lock(&ns->lock);
	ns->jobs--;
	g_mutex_unlock(&ns->lock);
	ns_poll_async(plugin, ns);
	ns_env_clear(&env);
}

/*
 * ns_start:
 * @share: (nullable): the connection, copied
 * @password: (nullable) (transfer full): a secret the job owns
 *
 * Queues a job.  Caller holds the lock (the job count is bumped under
 * it); the job itself runs on a worker.
 */
static NsJob *
ns_job_new(Netshares *ns, NsJobKind kind, const BarNetshare *share,
           gchar *password)
{
	NsJob *job;

	job = g_new0(NsJob, 1);
	job->ns       = ns;
	job->kind     = kind;
	job->share    = (share != NULL) ? bar_netshares_share_copy(share) : NULL;
	job->password = password;
	ns->jobs++;
	return job;
}

static void
ns_queue(GowlBarPlugin *plugin, NsJob *job)
{
	gowl_bar_plugin_queue_work(plugin, ns_job_run, job,
	                           (GDestroyNotify)ns_job_free);
}

/* ----------------------------------------------------------------
 * Actions (compositor thread)
 * ---------------------------------------------------------------- */

/* Mount or unmount @id.  Never blocks: flips the state and queues. */
static void
ns_toggle(GowlBarPlugin *plugin, Netshares *ns, const gchar *id,
          gint force)
{
	BarNetshare *share;
	NsState *st;
	NsJob *job = NULL;
	gboolean mount;

	g_mutex_lock(&ns->lock);
	share = bar_netshares_find(ns->shares, id);
	if (share == NULL) {
		g_mutex_unlock(&ns->lock);
		return;
	}
	st = ns_state(ns, id);
	if (st->busy) {
		g_mutex_unlock(&ns->lock);
		return;
	}
	mount = (force == 0) ? !st->mounted : (force > 0);
	st->busy = TRUE;
	g_clear_pointer(&st->error, g_free);
	job = ns_job_new(ns, mount ? NS_JOB_MOUNT : NS_JOB_UNMOUNT, share,
	                 NULL);
	g_mutex_unlock(&ns->lock);
	ns_queue(plugin, job);
}

/*
 * ns_arm:
 * @armed: the timestamp field
 *
 * The two-step confirm: the first click arms for five seconds, the
 * second within that window fires.
 *
 * Returns: %TRUE when this click should fire
 */
static gboolean
ns_arm(gint64 *armed)
{
	gint64 now = g_get_monotonic_time();

	if (*armed != 0 && now - *armed < NS_ARM_US) {
		*armed = 0;
		return TRUE;
	}
	*armed = now;
	return FALSE;
}

static gboolean
ns_armed(gint64 armed)
{
	return armed != 0 && g_get_monotonic_time() - armed < NS_ARM_US;
}

static void
ns_open(GowlBarPlugin *plugin, Netshares *ns, const gchar *id)
{
	BarNetshare *share;
	NsState *st;
	g_autofree gchar *path = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *name = NULL;
	gboolean mounted;
	const gchar *cmd;

	g_mutex_lock(&ns->lock);
	share = bar_netshares_find(ns->shares, id);
	st = (share != NULL) ? ns_state(ns, id) : NULL;
	if (st != NULL) {
		path = g_strdup(st->canonical);
		name = g_strdup(share->name);
	}
	mounted = (st != NULL && st->mounted);
	g_mutex_unlock(&ns->lock);
	if (st == NULL)
		return;

	if (!mounted || path == NULL) {
		g_autofree gchar *head = g_strdup_printf("%s is not mounted", name);

		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW, head,
		                       "Click the share to mount it first.");
		return;
	}
	cmd = gowl_bar_plugin_get_setting(plugin, "open-command");
	quoted = g_shell_quote(path);
	line = g_strdup_printf("%s %s",
		(cmd != NULL && cmd[0] != '\0') ? cmd : "xdg-open", quoted);
	gowl_bar_plugin_spawn(plugin, line);
}

static void
ns_forget(GowlBarPlugin *plugin, Netshares *ns, const gchar *id)
{
	BarNetshare *share;
	NsState *st;
	NsJob *job = NULL;
	g_autofree gchar *refuse = NULL;

	g_mutex_lock(&ns->lock);
	share = bar_netshares_find(ns->shares, id);
	if (share == NULL) {
		g_mutex_unlock(&ns->lock);
		return;
	}
	st = ns_state(ns, id);
	if (st->busy) {
		g_mutex_unlock(&ns->lock);
		return;
	}
	if (st->mounted) {
		st->forget_armed = 0;
		refuse = g_strdup_printf("Unmount %s before forgetting it",
		                         share->name);
	} else if (ns_arm(&st->forget_armed)) {
		st->busy = TRUE;
		job = ns_job_new(ns, NS_JOB_FORGET, share, NULL);
	}
	g_mutex_unlock(&ns->lock);

	if (refuse != NULL)
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW, refuse, NULL);
	if (job != NULL)
		ns_queue(plugin, job);
}

static void
ns_lazy(GowlBarPlugin *plugin, Netshares *ns, const gchar *id)
{
	BarNetshare *share;
	NsState *st;
	NsJob *job = NULL;

	g_mutex_lock(&ns->lock);
	share = bar_netshares_find(ns->shares, id);
	st = (share != NULL) ? ns_state(ns, id) : NULL;
	if (st != NULL && !st->busy && st->mounted &&
	    ns_arm(&st->lazy_armed)) {
		st->busy = TRUE;
		job = ns_job_new(ns, NS_JOB_UNMOUNT_LAZY, share, NULL);
	}
	g_mutex_unlock(&ns->lock);
	if (job != NULL)
		ns_queue(plugin, job);
}

static void
ns_password_open(Netshares *ns, const gchar *id)
{
	g_mutex_lock(&ns->lock);
	if (bar_netshares_find(ns->shares, id) != NULL) {
		g_free(ns->password_for);
		ns->password_for = g_strdup(id);
		g_clear_pointer(&ns->password_text, bar_netshares_secret_free);
		ns->password_rev++;
	}
	g_mutex_unlock(&ns->lock);
}

static void
ns_password_close(Netshares *ns)
{
	g_clear_pointer(&ns->password_for, g_free);
	g_clear_pointer(&ns->password_text, bar_netshares_secret_free);
	ns->password_rev++;
}

static void
ns_password_save(GowlBarPlugin *plugin, Netshares *ns)
{
	BarNetshare *share;
	NsJob *job = NULL;

	g_mutex_lock(&ns->lock);
	share = bar_netshares_find(ns->shares, ns->password_for);
	if (share != NULL && ns->password_text != NULL &&
	    ns->password_text[0] != '\0') {
		job = ns_job_new(ns, NS_JOB_STORE_PASSWORD, share,
		                 g_steal_pointer(&ns->password_text));
		ns_password_close(ns);
	}
	g_mutex_unlock(&ns->lock);
	if (job != NULL)
		ns_queue(plugin, job);
}

/*
 * ns_form_share:
 *
 * The connection the add form describes.  Caller holds the lock.
 *
 * Returns: (transfer full): the connection, not yet validated
 */
static BarNetshare *
ns_form_share(Netshares *ns)
{
	BarNetshare *share;

	share = bar_netshares_share_new();
	share->name   = g_strstrip(g_strdup(ns->form_name != NULL
	                                    ? ns->form_name : ""));
	share->type   = ns->form_type;
	share->server = g_strstrip(g_strdup(ns->form_server != NULL
	                                    ? ns->form_server : ""));
	share->share  = g_strstrip(g_strdup(ns->form_share != NULL
	                                    ? ns->form_share : ""));
	if (ns->form_user != NULL && ns->form_user[0] != '\0')
		share->user = g_strstrip(g_strdup(ns->form_user));
	share->id = bar_netshares_slug(share->name, ns->shares);
	if (ns->form_mountpoint != NULL && ns->form_mountpoint[0] != '\0')
		share->mountpoint = g_strstrip(g_strdup(ns->form_mountpoint));
	else
		/* A default that needs no privilege to create. */
		share->mountpoint = g_strdup_printf("~/mnt/%s", share->id);
	share->automount = ns->form_automount;
	return share;
}

static void
ns_form_save(GowlBarPlugin *plugin, Netshares *ns, gboolean mount)
{
	g_autoptr(GError) error = NULL;
	BarNetshare *share;
	NsJob *job = NULL;

	g_mutex_lock(&ns->lock);
	if (ns->form_busy) {
		g_mutex_unlock(&ns->lock);
		return;
	}
	share = ns_form_share(ns);
	/* The syntactic half answers at once; the filesystem half (does
	   the mount point resolve somewhere acceptable) is the worker's. */
	if (!bar_netshares_validate(share, &error)) {
		g_free(ns->form_error);
		ns->form_error = g_strdup(error->message);
	} else {
		g_clear_pointer(&ns->form_error, g_free);
		ns->form_busy = TRUE;
		job = ns_job_new(ns, NS_JOB_SAVE, share,
			(share->type == BAR_NETSHARES_CIFS &&
			 ns->form_password != NULL)
			? g_strdup(ns->form_password) : NULL);
		job->mount_after = mount;
	}
	g_mutex_unlock(&ns->lock);
	bar_netshares_share_free(share);
	if (job != NULL)
		ns_queue(plugin, job);
}

/* The extra buttons under a connection, in order.  The panel and the
   action handler both read this, so an index means the same thing in
   both. */
typedef enum {
	NS_BTN_OPEN,
	NS_BTN_PASSWORD,
	NS_BTN_FORCE,
	NS_BTN_FORGET
} NsButton;

static guint
ns_buttons(const BarNetshare *share, const NsState *st, NsButton out[4])
{
	guint n = 0;

	out[n++] = NS_BTN_OPEN;
	if (share->type == BAR_NETSHARES_CIFS &&
	    share->user != NULL && share->user[0] != '\0')
		out[n++] = NS_BTN_PASSWORD;
	if (st != NULL && st->busy_unmount && st->mounted)
		out[n++] = NS_BTN_FORCE;
	out[n++] = NS_BTN_FORGET;
	return n;
}

static void
ns_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	Netshares *ns = data;

	(void)value;
	(void)button;
	if (item_id == NULL)
		return;

	if (g_str_has_prefix(item_id, "toggle:")) {
		ns_toggle(plugin, ns, item_id + strlen("toggle:"), 0);
	} else if (g_str_has_prefix(item_id, "mount:")) {
		ns_toggle(plugin, ns, item_id + strlen("mount:"), 1);
	} else if (g_str_has_prefix(item_id, "unmount:")) {
		ns_toggle(plugin, ns, item_id + strlen("unmount:"), -1);
	} else if (g_str_has_prefix(item_id, "lazy:")) {
		ns_lazy(plugin, ns, item_id + strlen("lazy:"));
	} else if (g_str_has_prefix(item_id, "open:")) {
		ns_open(plugin, ns, item_id + strlen("open:"));
	} else if (g_str_has_prefix(item_id, "forget:")) {
		ns_forget(plugin, ns, item_id + strlen("forget:"));
	} else if (g_str_has_prefix(item_id, "password:")) {
		ns_password_open(ns, item_id + strlen("password:"));
	} else if (g_str_has_prefix(item_id, "act:")) {
		const gchar *id = item_id + strlen("act:");
		NsButton btns[4];
		guint n = 0;
		BarNetshare *share;

		g_mutex_lock(&ns->lock);
		share = bar_netshares_find(ns->shares, id);
		if (share != NULL)
			n = ns_buttons(share, ns_state(ns, id), btns);
		g_mutex_unlock(&ns->lock);
		if (index < 0 || (guint)index >= n)
			return;
		switch (btns[index]) {
		case NS_BTN_OPEN:     ns_open(plugin, ns, id);     break;
		case NS_BTN_PASSWORD: ns_password_open(ns, id);    break;
		case NS_BTN_FORCE:    ns_lazy(plugin, ns, id);     break;
		case NS_BTN_FORGET:   ns_forget(plugin, ns, id);   break;
		default:                                           break;
		}
	} else if (g_strcmp0(item_id, "password-save") == 0) {
		if (index == 0) {
			ns_password_save(plugin, ns);
		} else {
			g_mutex_lock(&ns->lock);
			ns_password_close(ns);
			g_mutex_unlock(&ns->lock);
		}
	} else if (g_strcmp0(item_id, "add-open") == 0) {
		g_mutex_lock(&ns->lock);
		ns->form_open = TRUE;
		g_mutex_unlock(&ns->lock);
	} else if (g_strcmp0(item_id, "add-type") == 0) {
		g_mutex_lock(&ns->lock);
		if (index >= 0 && index < BAR_NETSHARES_N_TYPES)
			ns->form_type = (BarNetsharesType)index;
		g_mutex_unlock(&ns->lock);
	} else if (g_strcmp0(item_id, "add-automount") == 0) {
		g_mutex_lock(&ns->lock);
		ns->form_automount = !ns->form_automount;
		g_mutex_unlock(&ns->lock);
	} else if (g_strcmp0(item_id, "add-save") == 0) {
		if (index == 0 || index == 1) {
			ns_form_save(plugin, ns, index == 1);
		} else {
			g_mutex_lock(&ns->lock);
			ns->form_open = FALSE;
			ns_form_clear(ns);
			g_mutex_unlock(&ns->lock);
		}
	} else if (g_strcmp0(item_id, "tools") == 0) {
		if (index == 0) {
			NsJob *job;

			g_mutex_lock(&ns->lock);
			job = ns_job_new(ns, NS_JOB_REFRESH, NULL, NULL);
			g_mutex_unlock(&ns->lock);
			ns_queue(plugin, job);
		} else if (index == 1) {
			g_autofree gchar *path = ns_config_path(plugin);
			g_autofree gchar *quoted = g_shell_quote(path);
			g_autofree gchar *line = NULL;
			const gchar *cmd;

			cmd = gowl_bar_plugin_get_setting(plugin, "edit-command");
			line = g_strdup_printf("%s %s",
				(cmd != NULL && cmd[0] != '\0') ? cmd : "xdg-open",
				quoted);
			gowl_bar_plugin_spawn(plugin, line);
		}
	}
}

/*
 * ns_text:
 *
 * The add form's fields and the password field.  Every edit arrives
 * here; Return submits.
 */
static void
ns_text(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
        const gchar *text, gboolean submitted)
{
	Netshares *ns = data;
	gchar **slot = NULL;
	gboolean password = FALSE;

	if (item_id == NULL)
		return;

	g_mutex_lock(&ns->lock);
	if (g_strcmp0(item_id, "password-entry") == 0) {
		slot = &ns->password_text;
		password = TRUE;
	} else if (g_strcmp0(item_id, "add-name") == 0) {
		slot = &ns->form_name;
	} else if (g_strcmp0(item_id, "add-server") == 0) {
		slot = &ns->form_server;
	} else if (g_strcmp0(item_id, "add-share") == 0) {
		slot = &ns->form_share;
	} else if (g_strcmp0(item_id, "add-user") == 0) {
		slot = &ns->form_user;
	} else if (g_strcmp0(item_id, "add-mountpoint") == 0) {
		slot = &ns->form_mountpoint;
	} else if (g_strcmp0(item_id, "add-password") == 0) {
		slot = &ns->form_password;
		password = TRUE;
	}
	if (slot != NULL) {
		if (password)
			bar_netshares_secret_free(*slot);
		else
			g_free(*slot);
		*slot = g_strdup(text != NULL ? text : "");
	}
	g_mutex_unlock(&ns->lock);

	if (!submitted || slot == NULL)
		return;
	if (g_strcmp0(item_id, "password-entry") == 0)
		ns_password_save(plugin, ns);
	else
		ns_form_save(plugin, ns, FALSE);
}

/* ----------------------------------------------------------------
 * The panel
 * ---------------------------------------------------------------- */

static const gchar *
ns_type_badge(BarNetsharesType type)
{
	switch (type) {
	case BAR_NETSHARES_CIFS:  return "SMB";
	case BAR_NETSHARES_NFS:   return "NFS";
	case BAR_NETSHARES_SSHFS: return "SSH";
	case BAR_NETSHARES_N_TYPES:
	default:                  return "?";
	}
}

static const gchar *
ns_type_icon(BarNetsharesType type)
{
	switch (type) {
	case BAR_NETSHARES_CIFS:  return NS_ICON_CIFS;
	case BAR_NETSHARES_NFS:   return NS_ICON_NFS;
	case BAR_NETSHARES_SSHFS:
	case BAR_NETSHARES_N_TYPES:
	default:                  return NS_ICON_SSHFS;
	}
}

/* Remember which connection a panel item belongs to, for panel_key. */
static void
ns_note_row(Netshares *ns, GowlBarPanel *panel, const gchar *id)
{
	guint index = gowl_bar_panel_n_items(panel) - 1;

	while (ns->row_ids->len <= index)
		g_ptr_array_add(ns->row_ids, NULL);
	g_free(g_ptr_array_index(ns->row_ids, index));
	g_ptr_array_index(ns->row_ids, index) = g_strdup(id);
}

static void
ns_panel_connection(Netshares *ns, GowlBarPanel *panel, BarNetshare *s)
{
	NsState *st;
	GowlBarPanelItem *item;
	g_autofree gchar *row_id = NULL;
	g_autofree gchar *act_id = NULL;
	g_autofree gchar *device = NULL;
	g_autofree gchar *subtitle = NULL;
	g_autofree gchar *missing = NULL;
	NsButton btns[4];
	guint n, b;

	st = ns_state(ns, s->id);
	device = bar_netshares_device(s);
	missing = ns_missing(ns, s->type, s->type == BAR_NETSHARES_CIFS &&
	                     s->user != NULL && s->user[0] != '\0');

	if (st->error != NULL)
		subtitle = g_strdup(st->error);
	else if (missing != NULL)
		subtitle = g_strdup_printf("Needs %s", missing);
	else
		subtitle = g_strdup_printf("%s  ->  %s", device,
			st->canonical != NULL ? st->canonical : s->mountpoint);

	row_id = g_strdup_printf("toggle:%s", s->id);
	item = gowl_bar_panel_add_row(panel, row_id, ns_type_icon(s->type),
	                              s->name, subtitle);
	ns_note_row(ns, panel, s->id);
	gowl_bar_panel_item_set_badge(item, ns_type_badge(s->type));
	gowl_bar_panel_item_set_active(item, st->mounted);
	gowl_bar_panel_item_set_busy(item, st->busy);
	if (st->busy) {
		gowl_bar_panel_item_set_value(item, "working...");
		gowl_bar_panel_item_set_value_color(item, GOWL_BAR_COLOR_YELLOW);
	} else if (st->mounted) {
		gowl_bar_panel_item_set_value(item, st->busy_unmount
		                              ? "busy" : "mounted");
		gowl_bar_panel_item_set_value_color(item, st->busy_unmount
			? GOWL_BAR_COLOR_YELLOW : GOWL_BAR_COLOR_GREEN);
	} else {
		gowl_bar_panel_item_set_value(item, "not mounted");
		gowl_bar_panel_item_set_value_color(item, GOWL_BAR_COLOR_MUTED);
	}
	if (st->error != NULL || missing != NULL)
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);

	act_id = g_strdup_printf("act:%s", s->id);
	item = gowl_bar_panel_add_buttons(panel, act_id);
	ns_note_row(ns, panel, s->id);
	n = ns_buttons(s, st, btns);
	for (b = 0; b < n; b++) {
		switch (btns[b]) {
		case NS_BTN_OPEN:
			gowl_bar_panel_add_button(item, "Open", FALSE);
			break;
		case NS_BTN_PASSWORD:
			gowl_bar_panel_add_button(item, "Password",
				g_strcmp0(ns->password_for, s->id) == 0);
			break;
		case NS_BTN_FORCE:
			gowl_bar_panel_add_button(item,
				ns_armed(st->lazy_armed)
				? "Click again to force" : "Force unmount",
				ns_armed(st->lazy_armed));
			break;
		case NS_BTN_FORGET:
		default:
			gowl_bar_panel_add_button(item,
				ns_armed(st->forget_armed)
				? "Click again to forget" : "Forget",
				ns_armed(st->forget_armed));
			break;
		}
	}
}

static void
ns_panel_form(Netshares *ns, GowlBarPanel *panel)
{
	GowlBarPanelItem *item;
	gchar rev[16];
	g_autofree gchar *missing = NULL;
	const gchar *share_label;
	gint t;

	g_snprintf(rev, sizeof(rev), "%u", ns->form_rev);

	gowl_bar_panel_add_section(panel, "Add connection");
	item = gowl_bar_panel_add_buttons(panel, "add-type");
	for (t = 0; t < BAR_NETSHARES_N_TYPES; t++)
		gowl_bar_panel_add_button(item,
			ns_type_badge((BarNetsharesType)t),
			ns->form_type == (BarNetsharesType)t);

#define NS_ENTRY(id_, title_, hint_, text_) \
	item = gowl_bar_panel_add_entry(panel, id_, hint_, text_); \
	gowl_bar_panel_item_set_title(item, title_); \
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION, rev)

	NS_ENTRY("add-name", "Name", "NAS media", ns->form_name);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_AUTOFOCUS,
	                             "true");
	NS_ENTRY("add-server", "Server", "nas.lan or 192.168.1.10",
	         ns->form_server);
	switch (ns->form_type) {
	case BAR_NETSHARES_NFS:
		share_label = "Export path";
		NS_ENTRY("add-share", share_label, "/srv/media", ns->form_share);
		break;
	case BAR_NETSHARES_SSHFS:
		share_label = "Remote path";
		NS_ENTRY("add-share", share_label, "empty for your home",
		         ns->form_share);
		break;
	case BAR_NETSHARES_CIFS:
	case BAR_NETSHARES_N_TYPES:
	default:
		share_label = "Share";
		NS_ENTRY("add-share", share_label, "media", ns->form_share);
		break;
	}
	if (ns->form_type != BAR_NETSHARES_NFS) {
		NS_ENTRY("add-user", "User",
		         ns->form_type == BAR_NETSHARES_CIFS
		         ? "empty for guest; DOMAIN\\user works"
		         : "empty for your login name",
		         ns->form_user);
	}
	NS_ENTRY("add-mountpoint", "Mount point",
	         "~/mnt/<name>  (or under /mnt, /media)",
	         ns->form_mountpoint);
	if (ns->form_type == BAR_NETSHARES_CIFS) {
		/* Never pre-filled: the host keeps what was typed, and the
		   panel never echoes a password back. */
		NS_ENTRY("add-password", "Password", "stored in the keyring",
		         "");
		gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_SECRET,
		                             "true");
	}
#undef NS_ENTRY

	gowl_bar_panel_add_toggle(panel, "add-automount",
	                          "Mount when the bar starts",
	                          ns->form_automount);

	missing = ns_missing(ns, ns->form_type,
	                     ns->form_type == BAR_NETSHARES_CIFS);
	if (missing != NULL) {
		g_autofree gchar *msg = g_strdup_printf("Not installed: %s",
		                                        missing);

		item = gowl_bar_panel_add_label(panel, msg);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}
	if (ns->form_error != NULL) {
		item = gowl_bar_panel_add_label(panel, ns->form_error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}

	item = gowl_bar_panel_add_buttons(panel, "add-save");
	gowl_bar_panel_add_button(item, ns->form_busy ? "Saving..." : "Save",
	                          FALSE);
	gowl_bar_panel_add_button(item, "Save & mount", FALSE);
	gowl_bar_panel_add_button(item, "Cancel", FALSE);
	gowl_bar_panel_item_set_busy(item, ns->form_busy);
}

static GowlBarPanel *
ns_panel(GowlBarPlugin *plugin, gpointer data)
{
	Netshares *ns = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	g_autofree gchar *subtitle = NULL;
	g_autofree gchar *path = NULL;
	guint i, mounted = 0;

	path = ns_config_path(plugin);
	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 460);

	g_mutex_lock(&ns->lock);
	g_ptr_array_set_size(ns->row_ids, 0);

	for (i = 0; i < ns->shares->len; i++) {
		BarNetshare *s = g_ptr_array_index(ns->shares, i);
		NsState *st = g_hash_table_lookup(ns->states, s->id);

		if (st != NULL && st->mounted)
			mounted++;
	}
	if (ns->shares->len == 0)
		subtitle = g_strdup(ns->loaded ? "Nothing configured yet"
		                               : "Reading...");
	else
		subtitle = g_strdup_printf("%u of %u mounted", mounted,
		                           ns->shares->len);
	gowl_bar_panel_add_hero(panel, NS_ICON, "Network shares", subtitle);

	if (ns->config_error != NULL) {
		item = gowl_bar_panel_add_label(panel, ns->config_error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}
	for (i = 0; i < ns->warnings->len && i < 4; i++) {
		item = gowl_bar_panel_add_label(panel,
			g_ptr_array_index(ns->warnings, i));
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	if (ns->shares->len == 0 && ns->loaded) {
		g_autofree gchar *msg = g_strdup_printf(
			"Add a CIFS (SMB), NFS or SSHFS share below, or write "
			"connections into %s.", path);

		gowl_bar_panel_add_label(panel, msg);
	} else if (ns->shares->len > 0) {
		gowl_bar_panel_add_section(panel, "Connections");
		for (i = 0; i < ns->shares->len; i++)
			ns_panel_connection(ns, panel,
			                    g_ptr_array_index(ns->shares, i));
	}

	/* The password field, for whichever connection asked. */
	if (ns->password_for != NULL) {
		BarNetshare *s = bar_netshares_find(ns->shares, ns->password_for);

		if (s != NULL) {
			g_autofree gchar *title = NULL;
			gchar rev[16];

			title = g_strdup_printf("Password for %s", s->name);
			g_snprintf(rev, sizeof(rev), "%u", ns->password_rev);
			gowl_bar_panel_add_separator(panel);
			gowl_bar_panel_add_section(panel, title);
			item = gowl_bar_panel_add_entry(panel, "password-entry",
			                                "stored in the keyring",
			                                "");
			gowl_bar_panel_item_set_prop(item,
				GOWL_BAR_ENTRY_PROP_SECRET, "true");
			gowl_bar_panel_item_set_prop(item,
				GOWL_BAR_ENTRY_PROP_AUTOFOCUS, "true");
			gowl_bar_panel_item_set_prop(item,
				GOWL_BAR_ENTRY_PROP_REVISION, rev);
			item = gowl_bar_panel_add_buttons(panel, "password-save");
			gowl_bar_panel_add_button(item, "Save", FALSE);
			gowl_bar_panel_add_button(item, "Cancel", FALSE);
		}
	}

	gowl_bar_panel_add_separator(panel);
	if (ns->form_open) {
		ns_panel_form(ns, panel);
	} else {
		item = gowl_bar_panel_add_row(panel, "add-open", NS_ICON_ADD,
		                              "Add connection",
		                              "CIFS (SMB), NFS or SSHFS");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_ACCENT);
	}
	g_mutex_unlock(&ns->lock);

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_field(panel, "File", path);
	item = gowl_bar_panel_add_buttons(panel, "tools");
	gowl_bar_panel_add_button(item, "Refresh", FALSE);
	gowl_bar_panel_add_button(item, "Edit file", FALSE);
	return panel;
}

/*
 * ns_key:
 *
 * With a connection's row (or its buttons) focused: `o' opens it, `p'
 * asks for its password, `m' mounts or unmounts it.  Return already
 * toggles through the row itself.
 */
static gboolean
ns_key(GowlBarPlugin *plugin, gpointer data, guint keysym, guint modifiers,
       gint focused_item)
{
	Netshares *ns = data;
	g_autofree gchar *id = NULL;

	if (modifiers != 0 || focused_item < 0)
		return FALSE;
	g_mutex_lock(&ns->lock);
	if ((guint)focused_item < ns->row_ids->len)
		id = g_strdup(g_ptr_array_index(ns->row_ids,
		                                (guint)focused_item));
	g_mutex_unlock(&ns->lock);
	if (id == NULL)
		return FALSE;

	switch (keysym) {
	case XKB_KEY_o:
		ns_open(plugin, ns, id);
		return TRUE;
	case XKB_KEY_p:
		ns_password_open(ns, id);
		gowl_bar_plugin_refresh_panel(plugin);
		return TRUE;
	case XKB_KEY_m:
		ns_toggle(plugin, ns, id, 0);
		gowl_bar_plugin_refresh_panel(plugin);
		return TRUE;
	default:
		return FALSE;
	}
}

static void
ns_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	Netshares *ns = data;

	(void)plugin;
	/* A typed password does not outlive the panel it was typed in. */
	g_mutex_lock(&ns->lock);
	g_clear_pointer(&ns->password_text, bar_netshares_secret_free);
	g_clear_pointer(&ns->form_password, bar_netshares_secret_free);
	g_mutex_unlock(&ns->lock);
}

static const GowlBarPluginVTable netshares_vtable = {
	sizeof(GowlBarPluginVTable),
	ns_create, ns_destroy,
	NULL, NULL, ns_configure,
	ns_interval, ns_poll, ns_poll_async,
	NULL, NULL,
	NULL, NULL,
	ns_panel, ns_action,
	NULL, ns_panel_closed,
	ns_key,
	NULL,
	ns_text, NULL
};

/**
 * bar_register_netshares_plugins:
 * @registry: the registry
 *
 * Registers `netshares' and its aliases `netmount' and `shares'.
 */
void
bar_register_netshares_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "netshares",
		"Network shares",
		"Mount and unmount CIFS, NFS and SSHFS shares",
		&netshares_vtable);
	gowl_bar_registry_register_alias(registry, "netmount", "netshares");
	gowl_bar_registry_register_alias(registry, "shares", "netshares");
}
