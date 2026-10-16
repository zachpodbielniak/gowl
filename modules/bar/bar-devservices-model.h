/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * bar-devservices-model.h -- the pure half of the `devservices' widget.
 *
 * Everything the widget decides that does not need a bar, a theme or a
 * compositor lives here: reading stack definitions, ordering services,
 * finding which project a directory belongs to, walking a process tree
 * for the focused window's working directory, reading /proc/net/tcp,
 * the state file that lets a restarted gowl still stop what it started,
 * starting a service in its own process group and stopping that group,
 * the per-service status machine, and the read-only project scan.
 *
 * Every function that reads /proc takes the proc root as a parameter,
 * so tests/test-bar-devservices.c drives them against a fabricated
 * tree instead of the live one.  Nothing here touches a GowlBarPlugin.
 */

#ifndef BAR_DEVSERVICES_MODEL_H
#define BAR_DEVSERVICES_MODEL_H

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

/* Bounds on what the widget is willing to read from disk.  A stack
   file, a package.json or a Makefile larger than this is not something
   a developer wrote by hand, and reading it on a worker every poll
   would be the widget's whole cost. */
#define BAR_DEVSERVICES_MAX_FILE   (256 * 1024)
#define BAR_DEVSERVICES_LOG_MAX    (5 * 1024 * 1024)

#define BAR_DEVSERVICES_ERROR (bar_devservices_error_quark())

/**
 * BarDevservicesError:
 * @BAR_DEVSERVICES_ERROR_PARSE: a stack file is not valid YAML or not
 *   the expected shape
 * @BAR_DEVSERVICES_ERROR_INVALID: a stack file is well-formed but says
 *   something the widget cannot act on (a missing root, a bad id, an
 *   unknown service in `needs')
 * @BAR_DEVSERVICES_ERROR_CYCLE: the `needs' graph has a cycle
 * @BAR_DEVSERVICES_ERROR_SPAWN: a service could not be started
 * @BAR_DEVSERVICES_ERROR_IO: a file could not be read or written
 */
typedef enum {
	BAR_DEVSERVICES_ERROR_PARSE,
	BAR_DEVSERVICES_ERROR_INVALID,
	BAR_DEVSERVICES_ERROR_CYCLE,
	BAR_DEVSERVICES_ERROR_SPAWN,
	BAR_DEVSERVICES_ERROR_IO
} BarDevservicesError;

GQuark bar_devservices_error_quark (void);

/**
 * BarDevservicesKind:
 * @BAR_DEVSERVICES_KIND_PROCESS: a command the widget starts itself
 * @BAR_DEVSERVICES_KIND_PODMAN: a `podman compose' service
 */
typedef enum {
	BAR_DEVSERVICES_KIND_PROCESS,
	BAR_DEVSERVICES_KIND_PODMAN
} BarDevservicesKind;

/**
 * BarDevservicesService:
 * @id: the service id, `[A-Za-z0-9_.-]+'; it names the log file
 * @label: what the panel shows
 * @kind: process or podman
 * @dir: the absolute working directory
 * @argv: (array zero-terminated=1): the command; a string `cmd' is
 *   stored as `sh -c STRING'
 * @shell: whether @argv came from a string `cmd'
 * @port: the port it listens on, or 0
 * @health: (nullable): %NULL, `port', or a URL to GET
 * @url: (nullable): a URL to open
 * @wait: seconds it may take to become healthy
 * @open: open @url once healthy
 * @env: (array zero-terminated=1) (nullable): `KEY=VALUE' entries
 * @compose: (nullable): absolute compose file, podman only
 * @compose_service: (nullable): the service name inside it
 * @compose_project: (nullable): an explicit compose project name
 * @needs: (array zero-terminated=1) (nullable): service ids this one
 *   must start after
 */
typedef struct {
	gchar               *id;
	gchar               *label;
	BarDevservicesKind   kind;
	gchar               *dir;
	gchar              **argv;
	gboolean             shell;
	gint                 port;
	gchar               *health;
	gchar               *url;
	gint                 wait;
	gboolean             open;
	gchar              **env;
	gchar               *compose;
	gchar               *compose_service;
	gchar               *compose_project;
	gchar              **needs;
} BarDevservicesService;

/**
 * BarDevservicesTask:
 * @id: the task id
 * @label: what the panel shows
 * @dir: the absolute working directory
 * @argv: (array zero-terminated=1): the command
 * @needs: (array zero-terminated=1) (nullable): services started first
 */
typedef struct {
	gchar  *id;
	gchar  *label;
	gchar  *dir;
	gchar **argv;
	gchar **needs;
} BarDevservicesTask;

/**
 * BarDevservicesStack:
 * @key: the file's stem, `[A-Za-z0-9_.-]+'; names the state and logs
 * @name: the display name
 * @root: the absolute project directory, as written (tilde expanded)
 * @file: (nullable): the file it was read from
 * @services: (element-type BarDevservicesService): in file order
 * @tasks: (element-type BarDevservicesTask): in file order
 */
typedef struct {
	gchar     *key;
	gchar     *name;
	gchar     *root;
	gchar     *file;
	GPtrArray *services;
	GPtrArray *tasks;
} BarDevservicesStack;

/**
 * bar_devservices_stack_parse:
 * @text: the YAML text
 * @key: the stack's key (the file stem)
 * @file: (nullable): the path, for messages
 * @error: return location for a #GError
 *
 * Parses one stack definition.  Fails as a whole on any bad entry: a
 * stack with a silently dropped service would start without its
 * database and report success.
 *
 * Returns: (transfer full) (nullable): the stack
 */
BarDevservicesStack *bar_devservices_stack_parse (const gchar *text,
                                                  const gchar *key,
                                                  const gchar *file,
                                                  GError     **error);

void bar_devservices_stack_free (BarDevservicesStack *stack);

/**
 * bar_devservices_stack_find_service:
 * @stack: a stack
 * @id: a service id
 *
 * Returns: the service's index, or -1
 */
gint bar_devservices_stack_find_service (const BarDevservicesStack *stack,
                                         const gchar               *id);

/**
 * bar_devservices_stack_find_task:
 * @stack: a stack
 * @id: a task id
 *
 * Returns: (transfer none) (nullable): the task
 */
BarDevservicesTask *bar_devservices_stack_find_task (
	const BarDevservicesStack *stack, const gchar *id);

/**
 * bar_devservices_load_dir:
 * @dir: the config directory
 * @errors: (element-type utf8): one `file: message' per bad file
 *
 * Reads every `*.yaml' / `*.yml' in @dir, sorted by name.  A bad file
 * is reported in @errors and skipped; the others still load.
 *
 * Returns: (transfer full) (element-type BarDevservicesStack): stacks
 */
GPtrArray *bar_devservices_load_dir (const gchar *dir, GPtrArray *errors);

/**
 * bar_devservices_dir_signature:
 * @dir: the config directory
 *
 * A string that changes whenever a stack file is added, removed or
 * modified, so the worker reloads only when something moved.
 *
 * Returns: (transfer full): the signature
 */
gchar *bar_devservices_dir_signature (const gchar *dir);

/**
 * bar_devservices_valid_id:
 * @id: (nullable): a candidate id
 *
 * Returns: %TRUE when @id is non-empty `[A-Za-z0-9_.-]+' and not `.'
 *   or `..' -- ids name files and travel inside panel item ids
 */
gboolean bar_devservices_valid_id (const gchar *id);

/* --- Ordering ------------------------------------------------------ */

/**
 * bar_devservices_plan:
 * @stack: a stack
 * @only: (array zero-terminated=1) (nullable): service ids to start,
 *   with everything they need; %NULL for all of them
 * @error: return location for a #GError
 *
 * Orders services into WAVES: every service in a wave may start
 * together, and a wave starts only once the one before it is healthy.
 * A service's wave is decided by the length of its `needs' chain and,
 * within that, podman before process -- so a stack with no `needs' at
 * all comes out as the spec'd "containers first, then the processes".
 *
 * Returns: (transfer full) (element-type GArray) (nullable): waves of
 *   guint service indices, or %NULL with %BAR_DEVSERVICES_ERROR_CYCLE
 */
GPtrArray *bar_devservices_plan (const BarDevservicesStack *stack,
                                 const gchar * const       *only,
                                 GError                   **error);

/* --- Locating the project ------------------------------------------ */

/**
 * bar_devservices_path_within:
 * @root: a directory
 * @path: a path
 *
 * Whether @path is @root or below it, on a path-component boundary
 * (`/src/app' does not contain `/src/application').  Both are resolved
 * with realpath() when they exist, so a project reached through a
 * symlink still matches.
 *
 * Returns: the length of the resolved @root when it contains @path,
 *   else 0
 */
gsize bar_devservices_path_within (const gchar *root, const gchar *path);

/**
 * bar_devservices_match_root:
 * @stacks: (element-type BarDevservicesStack): the stacks
 * @cwd: (nullable): a directory
 *
 * Returns: the index of the stack whose root contains @cwd most
 *   specifically (the longest root wins: a monorepo stack and one for
 *   a package inside it), or -1
 */
gint bar_devservices_match_root (GPtrArray *stacks, const gchar *cwd);

/**
 * bar_devservices_parse_stat:
 * @text: the contents of /proc/PID/stat
 * @ppid: (out) (optional): the parent pid
 * @pgrp: (out) (optional): the process group
 * @starttime: (out) (optional): start time in clock ticks since boot
 * @comm: (out) (optional) (transfer full): the command name
 * @state: (out) (optional): the state letter
 *
 * The command name sits in parentheses and may itself contain spaces
 * and parentheses, so the fields after it are found from the LAST `)'.
 *
 * Returns: %TRUE when the line had every field
 */
gboolean bar_devservices_parse_stat (const gchar *text,
                                     gint        *ppid,
                                     gint        *pgrp,
                                     guint64     *starttime,
                                     gchar      **comm,
                                     gchar       *state);

/**
 * bar_devservices_proc_starttime:
 * @proc_root: normally `/proc'
 * @pid: a pid
 *
 * Returns: the process's start time, or 0 when it does not exist or
 *   is a zombie
 */
guint64 bar_devservices_proc_starttime (const gchar *proc_root, gint pid);

/**
 * bar_devservices_focus_cwd:
 * @proc_root: normally `/proc'
 * @pid: the focused window's process
 *
 * The working directory of the deepest shell below @pid -- the shell a
 * terminal is showing, not the terminal, whose own cwd is wherever it
 * was launched.  Among shells at the same depth (tmux panes, tabs) the
 * most recently started wins.  With no shell below it, @pid's own cwd.
 *
 * Returns: (transfer full) (nullable): the directory
 */
gchar *bar_devservices_focus_cwd (const gchar *proc_root, gint pid);

/* --- Ports ----------------------------------------------------------- */

/**
 * bar_devservices_parse_net_tcp:
 * @text: the contents of /proc/net/tcp or /proc/net/tcp6
 * @into: (element-type guint guint64): port -> socket inode, added to
 *
 * Collects every LISTEN (state `0A') socket.  Ports are hex in the
 * `local_address' column after the last `:' of either address family.
 *
 * Returns: how many listening sockets were found
 */
guint bar_devservices_parse_net_tcp (const gchar *text, GHashTable *into);

/**
 * bar_devservices_listening:
 * @proc_root: normally `/proc'
 *
 * Returns: (transfer full) (element-type guint guint64): port ->
 *   inode for every TCP socket listening, IPv4 and IPv6
 */
GHashTable *bar_devservices_listening (const gchar *proc_root);

/**
 * bar_devservices_socket_owner:
 * @proc_root: normally `/proc'
 * @inode: a socket inode
 * @uid: only processes owned by this user are considered
 *
 * Finds the process holding `socket:[INODE]' open.
 *
 * Returns: the pid, or 0
 */
gint bar_devservices_socket_owner (const gchar *proc_root,
                                   guint64      inode,
                                   uid_t        uid);

/**
 * bar_devservices_port_connects:
 * @port: a TCP port
 * @timeout_ms: how long to wait
 *
 * Whether something accepts a connection on localhost:@port, IPv4 then
 * IPv6.  Blocking: workers only.
 *
 * Returns: %TRUE when a connection was made
 */
gboolean bar_devservices_port_connects (gint port, gint timeout_ms);

/* --- Runs and the state file ---------------------------------------- */

/**
 * BarDevservicesRun:
 * @id: the service (or `task:ID') it belongs to
 * @pid: the leader's pid
 * @pgid: its process group (equal to @pid: started with setsid)
 * @starttime: the leader's /proc start time when it was started
 * @started: wall-clock seconds when it was started
 *
 * What the widget remembers about a process it started.  @starttime is
 * the identity check: a pid alone may have been reused by the time a
 * restarted gowl reads the state file.
 */
typedef struct {
	gchar   *id;
	gint     pid;
	gint     pgid;
	guint64  starttime;
	gint64   started;
} BarDevservicesRun;

BarDevservicesRun *bar_devservices_run_new  (const gchar *id);
void               bar_devservices_run_free (BarDevservicesRun *run);

/**
 * bar_devservices_state_to_text:
 * @runs: (element-type BarDevservicesRun): the runs
 *
 * Returns: (transfer full): `id.key=value' lines
 */
gchar *bar_devservices_state_to_text (GPtrArray *runs);

/**
 * bar_devservices_state_from_text:
 * @text: what bar_devservices_state_to_text() wrote
 *
 * Unknown keys, bad numbers and invalid ids are skipped, and a run
 * without a pid and a start time is dropped: a half-read entry must
 * never become a pid the widget would signal.
 *
 * Returns: (transfer full) (element-type BarDevservicesRun): the runs
 */
GPtrArray *bar_devservices_state_from_text (const gchar *text);

/**
 * bar_devservices_state_save:
 * @path: the state file
 * @runs: (element-type BarDevservicesRun): the runs
 * @error: return location for a #GError
 *
 * Writes atomically, mode 0600; an empty @runs removes the file.
 *
 * Returns: %TRUE on success
 */
gboolean bar_devservices_state_save (const gchar *path,
                                     GPtrArray   *runs,
                                     GError     **error);

/**
 * bar_devservices_state_load:
 * @path: the state file
 *
 * Returns: (transfer full) (element-type BarDevservicesRun): the runs,
 *   empty when the file is missing or unreadable
 */
GPtrArray *bar_devservices_state_load (const gchar *path);

/**
 * BarDevservicesLiveness:
 * @BAR_DEVSERVICES_GONE: nothing of the run is left
 * @BAR_DEVSERVICES_ALIVE: the leader is alive and is the process the
 *   run recorded
 * @BAR_DEVSERVICES_ORPHANS: the leader has exited but members of its
 *   group, all started after it, are still running
 * @BAR_DEVSERVICES_MISMATCH: the pid is alive but is a DIFFERENT
 *   process (its start time does not match): never signal it
 */
typedef enum {
	BAR_DEVSERVICES_GONE,
	BAR_DEVSERVICES_ALIVE,
	BAR_DEVSERVICES_ORPHANS,
	BAR_DEVSERVICES_MISMATCH
} BarDevservicesLiveness;

/**
 * bar_devservices_run_liveness:
 * @proc_root: normally `/proc'
 * @run: a run
 *
 * Returns: what is left of @run
 */
BarDevservicesLiveness bar_devservices_run_liveness (const gchar *proc_root,
                                                     const BarDevservicesRun *run);

/**
 * bar_devservices_run_signal:
 * @proc_root: normally `/proc'
 * @run: a run
 * @sig: the signal
 *
 * Signals @run's whole process group -- but only after
 * bar_devservices_run_liveness() says it is still ours.  A pid that has
 * been reused by an unrelated process is refused.
 *
 * Returns: the liveness the decision was made on
 */
BarDevservicesLiveness bar_devservices_run_signal (const gchar *proc_root,
                                                   const BarDevservicesRun *run,
                                                   gint sig);

/**
 * bar_devservices_reap:
 * @pid: a child of this process, or any pid
 * @status: (out) (optional): its wait status, when reaped
 *
 * waitpid(@pid, WNOHANG).  A pid that is not our child (a run adopted
 * from the state file after a restart) is simply not reaped.
 *
 * Returns: %TRUE when @pid was reaped now
 */
gboolean bar_devservices_reap (gint pid, gint *status);

/**
 * bar_devservices_log_rotate:
 * @path: a log file
 * @max: rotate above this many bytes
 *
 * Moves an oversized log's contents to `PATH.1' and truncates it in
 * place.  Truncating rather than renaming is what makes this safe while
 * the service runs: it holds the file open with O_APPEND, so its next
 * write lands at the new end instead of in the renamed file.
 *
 * Returns: %TRUE when the log was rotated
 */
gboolean bar_devservices_log_rotate (const gchar *path, goffset max);

/**
 * bar_devservices_log_tail:
 * @path: a log file
 *
 * The last non-empty line, ANSI-stripped, valid UTF-8 and at most 120
 * characters.  Reads only the last 4 KiB.
 *
 * Returns: (transfer full) (nullable): the line
 */
gchar *bar_devservices_log_tail (const gchar *path);

/**
 * bar_devservices_spawn:
 * @argv: (array zero-terminated=1): the command
 * @dir: (nullable): the working directory
 * @env: (array zero-terminated=1) (nullable): extra `KEY=VALUE'
 * @log_path: the log; stdout and stderr are appended to it
 * @id: the run's id
 * @proc_root: normally `/proc', for the start time
 * @error: return location for a #GError
 *
 * Starts @argv in a NEW SESSION (setsid in the child), so its process
 * group is its own and stopping it signals everything it started and
 * nothing else.  The child is not reaped by GLib: the caller reaps it
 * with bar_devservices_reap() on a worker.
 *
 * Returns: (transfer full) (nullable): the run
 */
BarDevservicesRun *bar_devservices_spawn (const gchar * const *argv,
                                          const gchar         *dir,
                                          const gchar * const *env,
                                          const gchar         *log_path,
                                          const gchar         *id,
                                          const gchar         *proc_root,
                                          GError             **error);

/* --- Podman ---------------------------------------------------------- */

/**
 * bar_devservices_compose_argv:
 * @compose_command: the compose front end, e.g. `podman compose'
 * @svc: a podman service
 * @up: %TRUE for `up -d SERVICE', %FALSE for `down SERVICE'
 *
 * Returns: (transfer full) (nullable): the argv, or %NULL when
 *   @compose_command does not parse
 */
gchar **bar_devservices_compose_argv (const gchar                 *compose_command,
                                      const BarDevservicesService *svc,
                                      gboolean                     up);

/**
 * bar_devservices_podman_running:
 * @ps_json: the stdout of `podman ps --format json'
 * @svc: a podman service
 *
 * Whether a RUNNING container carries @svc's compose service label and
 * belongs to its compose project (by working-directory label, or by a
 * project name equal to the explicit one or to the compose file's
 * directory name).
 *
 * Returns: %TRUE when it is running
 */
gboolean bar_devservices_podman_running (const gchar                 *ps_json,
                                         const BarDevservicesService *svc);

/* --- Status ---------------------------------------------------------- */

/**
 * BarDevservicesState:
 * @BAR_DEVSERVICES_OFF: not running
 * @BAR_DEVSERVICES_STARTING: running, not healthy yet
 * @BAR_DEVSERVICES_UP: running and healthy
 * @BAR_DEVSERVICES_STUCK: running but unhealthy for too many polls
 * @BAR_DEVSERVICES_STOPPING: being stopped
 * @BAR_DEVSERVICES_FAILED: exited with an error or could not start
 */
typedef enum {
	BAR_DEVSERVICES_OFF,
	BAR_DEVSERVICES_STARTING,
	BAR_DEVSERVICES_UP,
	BAR_DEVSERVICES_STUCK,
	BAR_DEVSERVICES_STOPPING,
	BAR_DEVSERVICES_FAILED
} BarDevservicesState;

/**
 * bar_devservices_state_name:
 * @state: a state
 *
 * Returns: (transfer none): its badge text
 */
const gchar *bar_devservices_state_name (BarDevservicesState state);

/**
 * bar_devservices_status_step:
 * @unhealthy: (inout): consecutive unhealthy polls so far
 * @previous: the state before this poll
 * @running: whether the service is running now
 * @healthy: whether its health check passed now
 * @stuck_after: polls of running-but-unhealthy before it is stuck
 *
 * One poll of the status machine.  Not running clears the count and is
 * OFF -- except after FAILED, which stays until the next start so the
 * reason remains on screen.  Healthy clears the count and is UP.
 * Running but unhealthy counts up: STARTING until @stuck_after polls,
 * then STUCK.  A service that was UP and stops answering counts the
 * same way, so a hung server becomes STUCK rather than staying green.
 *
 * Returns: the new state
 */
BarDevservicesState bar_devservices_status_step (gint               *unhealthy,
                                                 BarDevservicesState previous,
                                                 gboolean            running,
                                                 gboolean            healthy,
                                                 gint                stuck_after);

/**
 * BarDevservicesAgg:
 * @BAR_DEVSERVICES_AGG_OFF: nothing running
 * @BAR_DEVSERVICES_AGG_OK: something up, nothing wrong
 * @BAR_DEVSERVICES_AGG_WORKING: something starting or stopping
 * @BAR_DEVSERVICES_AGG_ERROR: something stuck or failed
 */
typedef enum {
	BAR_DEVSERVICES_AGG_OFF,
	BAR_DEVSERVICES_AGG_OK,
	BAR_DEVSERVICES_AGG_WORKING,
	BAR_DEVSERVICES_AGG_ERROR
} BarDevservicesAgg;

/**
 * bar_devservices_aggregate:
 * @states: the services' states
 * @n: how many
 * @busy: whether an action is in flight
 * @up: (out) (optional): how many are UP
 *
 * Error beats working beats ok beats off: a red widget must never be
 * repainted yellow by an unrelated service that is merely starting.
 *
 * Returns: the aggregate
 */
BarDevservicesAgg bar_devservices_aggregate (const BarDevservicesState *states,
                                             guint                      n,
                                             gboolean                   busy,
                                             guint                     *up);

/**
 * bar_devservices_port_clashes:
 * @stacks: (element-type BarDevservicesStack): the stacks
 *
 * Returns: (transfer full) (element-type utf8): one line per port two
 *   projects both declare
 */
GPtrArray *bar_devservices_port_clashes (GPtrArray *stacks);

/* --- Scanning for projects -------------------------------------------- */

/**
 * BarDevservicesCandidate:
 * @dir: the project directory
 * @key: a stack key derived from its name
 * @found: (element-type utf8): what was recognised, for the panel
 * @draft: the YAML a `Declare' would write
 */
typedef struct {
	gchar     *dir;
	gchar     *key;
	GPtrArray *found;
	gchar     *draft;
} BarDevservicesCandidate;

void bar_devservices_candidate_free (BarDevservicesCandidate *cand);

/**
 * bar_devservices_scan_dir:
 * @dir: a directory that may be a project
 *
 * Recognises a compose file, a package.json with a `dev' script,
 * pyproject.toml, Cargo.toml, go.mod and a Makefile with a `run' or
 * `dev' target, and drafts a stack for them.  Read-only, bounded, and
 * it never opens a `.env' -- that is where secrets live.
 *
 * Returns: (transfer full) (nullable): a candidate, or %NULL when
 *   nothing was recognised
 */
BarDevservicesCandidate *bar_devservices_scan_dir (const gchar *dir);

/**
 * bar_devservices_scan:
 * @search_roots: colon-separated directories, `~' allowed
 * @stacks: (element-type BarDevservicesStack) (nullable): stacks
 *   already declared; their roots are skipped
 * @limit: stop after this many candidates
 *
 * Looks ONE level below each search root.
 *
 * Returns: (transfer full) (element-type BarDevservicesCandidate):
 *   the candidates, sorted by directory
 */
GPtrArray *bar_devservices_scan (const gchar *search_roots,
                                 GPtrArray   *stacks,
                                 guint        limit);

/**
 * bar_devservices_write_draft:
 * @config_dir: the stack directory
 * @cand: a candidate
 * @path_out: (out) (optional) (transfer full): the file written
 * @error: return location for a #GError
 *
 * Writes @cand's draft as `KEY.yaml', mode 0644, creating @config_dir
 * if needed.  Never overwrites: an existing file is the user's edit.
 *
 * Returns: %TRUE when written
 */
gboolean bar_devservices_write_draft (const gchar                   *config_dir,
                                      const BarDevservicesCandidate *cand,
                                      gchar                        **path_out,
                                      GError                       **error);

/**
 * bar_devservices_safe_key:
 * @name: anything
 *
 * Returns: (transfer full): @name lower-cased with everything outside
 *   `[a-z0-9_.-]' replaced by `-', never empty, never `.'/`..'
 */
gchar *bar_devservices_safe_key (const gchar *name);

G_END_DECLS

#endif /* BAR_DEVSERVICES_MODEL_H */
