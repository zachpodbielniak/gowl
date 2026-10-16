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

#ifndef BAR_NETSHARES_MODEL_H
#define BAR_NETSHARES_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * The pure half of the `netshares' widget: the connection file, the
 * mount table, the command lines, the credentials file and a bounded
 * subprocess runner.
 *
 * Nothing here knows about a plugin, a panel or a theme, so every
 * decision that matters for safety -- what reaches argv, what reaches
 * a root-owned mount, which paths are acceptable mount points -- can be
 * driven from tests/test-bar-netshares.c with the exact hostile input
 * that would break it.
 */

/**
 * BarNetsharesType:
 * @BAR_NETSHARES_CIFS: an SMB/CIFS share, mounted as root through pkexec
 * @BAR_NETSHARES_NFS: an NFS export, mounted as root through pkexec
 * @BAR_NETSHARES_SSHFS: an SFTP path, mounted as the user through FUSE
 * @BAR_NETSHARES_N_TYPES: the number of types
 *
 * The kinds of share the widget can mount.
 */
typedef enum {
	BAR_NETSHARES_CIFS = 0,
	BAR_NETSHARES_NFS,
	BAR_NETSHARES_SSHFS,
	BAR_NETSHARES_N_TYPES
} BarNetsharesType;

/**
 * BarNetshare:
 * @id: a stable identifier, `[A-Za-z0-9_-]', unique in the file
 * @name: what the panel shows
 * @type: the protocol
 * @server: a host name or address
 * @share: the CIFS share name, the NFS export path, or the SSHFS
 *   remote path (empty for the remote home)
 * @user: (nullable): the account; `DOMAIN\user' is accepted for CIFS
 * @mountpoint: the local folder as written (may start with `~/')
 * @encrypt: CIFS only: require SMB3 encryption (`seal')
 * @noexec: mount with `noexec'
 * @readonly: mount read-only
 * @automount: mount when the widget starts
 * @port: a non-default port, or 0
 * @nfs_version: (nullable): NFS only: the `vers=' value
 * @extra: (nullable) (array zero-terminated=1): extra `-o' options,
 *   each already validated by bar_netshares_option_ok()
 *
 * One connection from the connection file.
 */
typedef struct {
	gchar            *id;
	gchar            *name;
	BarNetsharesType  type;
	gchar            *server;
	gchar            *share;
	gchar            *user;
	gchar            *mountpoint;
	gboolean          encrypt;
	gboolean          noexec;
	gboolean          readonly;
	gboolean          automount;
	gint              port;
	gchar            *nfs_version;
	gchar           **extra;
} BarNetshare;

/**
 * BarNetsharesMount:
 * @mountpoint: the unescaped mount point
 * @fstype: the filesystem type (`cifs', `nfs4', `fuse.sshfs', ...)
 * @source: the unescaped mount source
 *
 * One line of /proc/self/mountinfo, the parts this widget reads.
 */
typedef struct {
	gchar *mountpoint;
	gchar *fstype;
	gchar *source;
} BarNetsharesMount;

/**
 * BarNetsharesTools:
 * @pkexec: the privilege helper, normally `pkexec'
 * @sshfs: the SSHFS client, normally `sshfs'
 * @fusermount: the FUSE unmount helper, normally `fusermount3'
 *
 * The programs the command lines name.  Settings, so a test can point
 * them at stand-ins that record what they were asked.
 */
typedef struct {
	const gchar *pkexec;
	const gchar *sshfs;
	const gchar *fusermount;
} BarNetsharesTools;

/**
 * BarNetsharesRun:
 * @spawned: whether the program started at all
 * @status: its exit status, or -1 when it died on a signal
 * @timed_out: whether it was killed at the deadline
 * @out: (nullable): standard output, bounded
 * @err: (nullable): standard error, bounded
 *
 * What bar_netshares_run() saw.
 */
typedef struct {
	gboolean  spawned;
	gint      status;
	gboolean  timed_out;
	gchar    *out;
	gchar    *err;
} BarNetsharesRun;

/* The error domain for every failure below that reaches the user. */
#define BAR_NETSHARES_ERROR (bar_netshares_error_quark())
GQuark bar_netshares_error_quark (void);

/* --- Connections ------------------------------------------------- */

BarNetshare *bar_netshares_share_new  (void);
BarNetshare *bar_netshares_share_copy (const BarNetshare *share);
void         bar_netshares_share_free (BarNetshare *share);

const gchar *bar_netshares_type_name  (BarNetsharesType type);
gboolean     bar_netshares_type_parse (const gchar      *text,
                                        BarNetsharesType *out);

gboolean bar_netshares_option_ok (const gchar *option, GError **error);
gboolean bar_netshares_validate  (const BarNetshare *share, GError **error);

GPtrArray *bar_netshares_load    (const gchar  *text,
                                   GPtrArray   **warnings,
                                   GError      **error);
gchar     *bar_netshares_to_yaml (GPtrArray *shares);
gchar     *bar_netshares_slug    (const gchar *name, GPtrArray *existing);
BarNetshare *bar_netshares_find  (GPtrArray *shares, const gchar *id);

/* --- Mount points ------------------------------------------------ */

gchar   *bar_netshares_expand_home       (const gchar *path,
                                          const gchar *home);
gboolean bar_netshares_resolve_mountpoint (const gchar  *path,
                                           const gchar  *home,
                                           gchar       **canonical,
                                           gboolean     *privileged,
                                           GError      **error);

/* --- The mount table --------------------------------------------- */

void       bar_netshares_mount_free       (BarNetsharesMount *mount);
gchar     *bar_netshares_unescape_octal   (const gchar *text);
GPtrArray *bar_netshares_parse_mountinfo  (const gchar *text);
gboolean   bar_netshares_fstype_matches   (BarNetsharesType type,
                                           const gchar     *fstype);
const BarNetsharesMount *
           bar_netshares_find_mount       (GPtrArray        *mounts,
                                           BarNetsharesType  type,
                                           const gchar      *mountpoint);

/* --- Command lines ----------------------------------------------- */

gchar  *bar_netshares_device         (const BarNetshare *share);
gchar  *bar_netshares_mount_options  (const BarNetshare *share,
                                      const gchar       *cred_path,
                                      guint              uid,
                                      guint              gid,
                                      const gchar       *nfs_version);
gchar **bar_netshares_mount_argv     (const BarNetshare       *share,
                                      const gchar             *mountpoint,
                                      const BarNetsharesTools *tools,
                                      const gchar             *cred_path,
                                      guint                    uid,
                                      guint                    gid,
                                      gboolean                 create,
                                      const gchar             *nfs_version);
gchar **bar_netshares_unmount_argv   (const BarNetshare       *share,
                                      const gchar             *mountpoint,
                                      const BarNetsharesTools *tools,
                                      gboolean                 lazy);
gchar **bar_netshares_secret_argv    (const gchar *tool,
                                      const gchar *verb,
                                      const gchar *id,
                                      const gchar *name);

/* --- Credentials ------------------------------------------------- */

gchar *bar_netshares_credentials_text  (const gchar *user,
                                        const gchar *password,
                                        GError     **error);
gchar *bar_netshares_write_credentials (const gchar *dir,
                                        const gchar *id,
                                        const gchar *user,
                                        const gchar *password,
                                        GError     **error);
void   bar_netshares_secret_free       (gchar *secret);

/* --- Running and reporting --------------------------------------- */

gboolean bar_netshares_run          (const gchar * const *argv,
                                     const gchar         *input,
                                     gint                 timeout_s,
                                     BarNetsharesRun     *result,
                                     GError             **error);
void     bar_netshares_run_clear    (BarNetsharesRun *result);
gchar   *bar_netshares_last_line    (const gchar *text,
                                     const gchar *secret);
gboolean bar_netshares_is_busy      (const gchar *text);
gchar   *bar_netshares_find_program (const gchar *name,
                                     const gchar *search_path);
const gchar *bar_netshares_package_for (const gchar *program);

G_END_DECLS

#endif /* BAR_NETSHARES_MODEL_H */
