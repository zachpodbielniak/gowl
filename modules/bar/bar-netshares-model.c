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
 * The pure half of the `netshares' widget.
 *
 * Three things in here run as root on the user's behalf -- a CIFS or
 * NFS mount is `pkexec mount ...' -- so the rules are deliberately
 * narrow and enforced where the values are READ, not where they are
 * used:
 *
 *   - every field that reaches argv is checked against a character
 *     class that cannot change the meaning of the command (no comma in
 *     anything that ends up inside `-o', no leading `-' anywhere, no
 *     control characters);
 *   - `-o' extras are an allowlist of characters AND a denylist of the
 *     options that would undo what the widget sets for safety (suid,
 *     dev, exec, uid/gid, credentials, a password);
 *   - a mount point must resolve, after following every symlink, under
 *     /mnt, /media or the user's home;
 *   - the password reaches mount.cifs only through a 0600 file in the
 *     user's runtime directory that is removed as soon as mount returns.
 *
 * Nothing here touches a GowlBarPlugin, so tests drive all of it with
 * the exact input that would break it.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "yaml-glib.h"

#include "bar-netshares-model.h"

/* Bounds on everything read from outside.  A connection file is a few
   hundred bytes; a mount helper's complaint is a line or two. */
#define NETSHARES_MAX_CONNECTIONS (64)
#define NETSHARES_MAX_OUTPUT      (64 * 1024)
#define NETSHARES_MAX_FIELD       (255)
#define NETSHARES_MAX_LINE        (200)

/**
 * bar_netshares_error_quark:
 *
 * Returns: the error domain for the widget's user-facing failures
 */
GQuark
bar_netshares_error_quark(void)
{
	return g_quark_from_static_string("bar-netshares-error-quark");
}

/* --- Connections ------------------------------------------------- */

/**
 * bar_netshares_share_new:
 *
 * A connection with the safe defaults: encrypted (CIFS), `noexec',
 * read-write, not mounted at start.
 *
 * Returns: (transfer full): a new #BarNetshare
 */
BarNetshare *
bar_netshares_share_new(void)
{
	BarNetshare *share;

	share = g_new0(BarNetshare, 1);
	share->type    = BAR_NETSHARES_CIFS;
	share->encrypt = TRUE;
	share->noexec  = TRUE;
	return share;
}

/**
 * bar_netshares_share_copy:
 * @share: a connection
 *
 * Returns: (transfer full): a deep copy of @share
 */
BarNetshare *
bar_netshares_share_copy(const BarNetshare *share)
{
	BarNetshare *copy;

	g_return_val_if_fail(share != NULL, NULL);

	copy = g_new0(BarNetshare, 1);
	*copy = *share;
	copy->id          = g_strdup(share->id);
	copy->name        = g_strdup(share->name);
	copy->server      = g_strdup(share->server);
	copy->share       = g_strdup(share->share);
	copy->user        = g_strdup(share->user);
	copy->mountpoint  = g_strdup(share->mountpoint);
	copy->nfs_version = g_strdup(share->nfs_version);
	copy->extra       = g_strdupv(share->extra);
	return copy;
}

/**
 * bar_netshares_share_free:
 * @share: (nullable): a connection
 */
void
bar_netshares_share_free(BarNetshare *share)
{
	if (share == NULL)
		return;
	g_free(share->id);
	g_free(share->name);
	g_free(share->server);
	g_free(share->share);
	g_free(share->user);
	g_free(share->mountpoint);
	g_free(share->nfs_version);
	g_strfreev(share->extra);
	g_free(share);
}

/**
 * bar_netshares_type_name:
 * @type: a type
 *
 * Returns: (transfer none): `cifs', `nfs' or `sshfs'
 */
const gchar *
bar_netshares_type_name(BarNetsharesType type)
{
	switch (type) {
	case BAR_NETSHARES_CIFS:  return "cifs";
	case BAR_NETSHARES_NFS:   return "nfs";
	case BAR_NETSHARES_SSHFS: return "sshfs";
	case BAR_NETSHARES_N_TYPES:
	default:                  return "unknown";
	}
}

/**
 * bar_netshares_type_parse:
 * @text: (nullable): a type name
 * @out: (out): the type
 *
 * `smb' is accepted for `cifs' because it is what most people call it.
 *
 * Returns: %TRUE when @text names a type
 */
gboolean
bar_netshares_type_parse(const gchar *text, BarNetsharesType *out)
{
	if (text == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(text, "cifs") == 0 ||
	    g_ascii_strcasecmp(text, "smb") == 0) {
		*out = BAR_NETSHARES_CIFS;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "nfs") == 0) {
		*out = BAR_NETSHARES_NFS;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "sshfs") == 0 ||
	    g_ascii_strcasecmp(text, "ssh") == 0) {
		*out = BAR_NETSHARES_SSHFS;
		return TRUE;
	}
	return FALSE;
}

/*
 * all_in:
 * @text: a string
 * @extra: characters allowed besides ASCII letters and digits
 *
 * Returns: %TRUE when @text is non-empty and every byte is alphanumeric
 *   or in @extra
 */
static gboolean
all_in(const gchar *text, const gchar *extra)
{
	const gchar *p;

	if (text == NULL || text[0] == '\0')
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (g_ascii_isalnum(*p))
			continue;
		if (strchr(extra, *p) == NULL)
			return FALSE;
	}
	return TRUE;
}

/*
 * has_control:
 * @text: (nullable): a string
 *
 * Returns: %TRUE when @text holds an ASCII control character
 */
static gboolean
has_control(const gchar *text)
{
	const gchar *p;

	if (text == NULL)
		return FALSE;
	for (p = text; *p != '\0'; p++)
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return TRUE;
	return FALSE;
}

/* The `-o' keys a user's extra options may not set.
 *
 * Each one undoes something this widget sets deliberately, or smuggles
 * in what must never be in argv:
 *   - suid / dev / exec: the mount is made by root, and a share whose
 *     server is hostile must not get set-uid binaries or device nodes
 *     onto this machine; `exec' is the `noexec: false' setting's job;
 *   - uid / gid / file_mode / dir_mode: the widget maps ownership to
 *     the user and keeps it private;
 *   - credentials / username / user / password / pass / password2:
 *     credentials go through the 0600 file, never argv;
 *   - allow_other / allow_root: a FUSE mount visible to other users;
 *   - ssh_command / sftp_server / ssh_protocol: run a different
 *     program than ssh.
 */
static const gchar *const denied_options[] = {
	"suid", "dev", "exec", "setuid", "setuids",
	"uid", "gid", "file_mode", "dir_mode", "forceuid", "forcegid",
	"credentials", "cred", "username", "user", "password", "pass",
	"password2", "allow_other", "allow_root",
	"ssh_command", "sftp_server", "ssh_protocol", "directport",
	"passive", "slave",
	NULL
};

/**
 * bar_netshares_option_ok:
 * @option: one extra `-o' option, possibly comma-separated
 * @error: return location for a #GError
 *
 * Checks an extra mount option against the allowlist pattern
 * `[A-Za-z0-9_=.,:-]' and the deny list above.  Comma-separated
 * pieces are checked one by one, since that is how mount reads them:
 * `rw,suid' is two options and the second is refused.
 *
 * Returns: %TRUE when @option may be passed
 */
gboolean
bar_netshares_option_ok(const gchar *option, GError **error)
{
	g_auto(GStrv) pieces = NULL;
	guint i, d;

	if (!all_in(option, "_=.,:-") ||
	    strlen(option) > NETSHARES_MAX_FIELD) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "option \"%s\" may only use letters, digits and "
		            "_ = . , : -",
		            (option != NULL && g_utf8_validate(option, -1, NULL)
		             && !has_control(option)) ? option : "?");
		return FALSE;
	}

	pieces = g_strsplit(option, ",", -1);
	for (i = 0; pieces[i] != NULL; i++) {
		g_autofree gchar *key = NULL;
		gchar *eq;

		if (pieces[i][0] == '\0' || pieces[i][0] == '-' ||
		    pieces[i][0] == '=') {
			g_set_error(error, BAR_NETSHARES_ERROR, 0,
			            "option \"%s\" is malformed", option);
			return FALSE;
		}
		key = g_ascii_strdown(pieces[i], -1);
		eq = strchr(key, '=');
		if (eq != NULL)
			*eq = '\0';
		for (d = 0; denied_options[d] != NULL; d++) {
			if (strcmp(key, denied_options[d]) == 0) {
				g_set_error(error, BAR_NETSHARES_ERROR, 0,
				            "option \"%s\" is not allowed: the "
				            "widget sets it itself or it would "
				            "weaken the mount", pieces[i]);
				return FALSE;
			}
		}
	}
	return TRUE;
}

/*
 * server_ok:
 * @server: a host
 *
 * A DNS name, an IPv4 address or an IPv6 address (bracketed or not).
 * Nothing that could start an option or carry a path, a comma or a
 * space into the device argument.
 */
static gboolean
server_ok(const gchar *server)
{
	if (server == NULL || server[0] == '-' || server[0] == '.' ||
	    strlen(server) > NETSHARES_MAX_FIELD)
		return FALSE;
	if (server[0] == '[') {
		gsize len = strlen(server);

		if (len < 3 || server[len - 1] != ']')
			return FALSE;
		{
			g_autofree gchar *inner = g_strndup(server + 1, len - 2);

			return all_in(inner, ":.");
		}
	}
	return all_in(server, ".-:");
}

/*
 * path_text_ok:
 * @text: a share name or remote path
 *
 * Spaces are allowed: shares called `Shared Music' exist and the
 * device is a single argv element, so a space cannot split it.  What
 * is refused is anything a mount helper or the remote side may read
 * as structure: a comma (an option separator for mount.cifs, which
 * parses the device too), quotes, a backslash, shell metacharacters
 * kept out on principle, and control characters.
 */
static gboolean
path_text_ok(const gchar *text)
{
	const gchar *p;

	if (text == NULL || text[0] == '-' ||
	    strlen(text) > NETSHARES_MAX_FIELD ||
	    !g_utf8_validate(text, -1, NULL) || has_control(text))
		return FALSE;
	for (p = text; *p != '\0'; p++)
		if (strchr(",\"'\\`$;|&<>*?", *p) != NULL)
			return FALSE;
	return TRUE;
}

/*
 * has_dotdot:
 * @path: a path
 *
 * Returns: %TRUE when any component of @path is `.' or `..'
 */
static gboolean
has_dotdot(const gchar *path)
{
	g_auto(GStrv) parts = NULL;
	guint i;

	parts = g_strsplit(path, "/", -1);
	for (i = 0; parts[i] != NULL; i++)
		if (strcmp(parts[i], "..") == 0 || strcmp(parts[i], ".") == 0)
			return TRUE;
	return FALSE;
}

/**
 * bar_netshares_validate:
 * @share: a connection
 * @error: return location for a #GError
 *
 * The syntactic checks: everything that can be decided without the
 * filesystem.  The mount point is resolved separately, on a worker,
 * by bar_netshares_resolve_mountpoint().
 *
 * Returns: %TRUE when @share is safe to put on a command line
 */
gboolean
bar_netshares_validate(const BarNetshare *share, GError **error)
{
	guint i;

	g_return_val_if_fail(share != NULL, FALSE);

	if (!all_in(share->id, "_-") || share->id[0] == '-' ||
	    strlen(share->id) > 64) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "id must be letters, digits, _ and -");
		return FALSE;
	}
	if (share->name == NULL || share->name[0] == '\0' ||
	    strlen(share->name) > 128 ||
	    !g_utf8_validate(share->name, -1, NULL) ||
	    has_control(share->name)) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "a name is required (printable, up to 128 bytes)");
		return FALSE;
	}
	if (share->type >= BAR_NETSHARES_N_TYPES) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0, "unknown type");
		return FALSE;
	}
	if (!server_ok(share->server)) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "server must be a host name or an address");
		return FALSE;
	}

	/* The share field means something different per type. */
	switch (share->type) {
	case BAR_NETSHARES_CIFS:
		if (!path_text_ok(share->share) || share->share[0] == '\0' ||
		    strchr(share->share, '/') != NULL) {
			g_set_error(error, BAR_NETSHARES_ERROR, 0,
			            "share must be a share name (no / , "
			            "quotes or backslashes)");
			return FALSE;
		}
		break;
	case BAR_NETSHARES_NFS:
		if (!path_text_ok(share->share) || share->share[0] != '/' ||
		    strchr(share->share, ':') != NULL ||
		    has_dotdot(share->share)) {
			g_set_error(error, BAR_NETSHARES_ERROR, 0,
			            "export must be an absolute path such as "
			            "/srv/media");
			return FALSE;
		}
		break;
	case BAR_NETSHARES_SSHFS:
		/* Empty is the remote home directory. */
		if (share->share != NULL && share->share[0] != '\0' &&
		    (!path_text_ok(share->share) ||
		     strchr(share->share, ':') != NULL)) {
			g_set_error(error, BAR_NETSHARES_ERROR, 0,
			            "remote path may not hold : , quotes or "
			            "shell characters");
			return FALSE;
		}
		break;
	case BAR_NETSHARES_N_TYPES:
	default:
		break;
	}

	/* The user: in a credentials file for CIFS (one line, so no
	   newline), in argv as `user@host' for SSHFS (so no @ or :). */
	if (share->user != NULL && share->user[0] != '\0') {
		gboolean ok;

		if (share->type == BAR_NETSHARES_CIFS) {
			const gchar *slash = strchr(share->user, '\\');

			ok = all_in(share->user, "._-@\\") &&
			     share->user[0] != '-' &&
			     (slash == NULL ||
			      (slash != share->user &&
			       strchr(slash + 1, '\\') == NULL &&
			       slash[1] != '\0'));
		} else {
			ok = all_in(share->user, "._-") &&
			     share->user[0] != '-';
		}
		if (!ok || strlen(share->user) > 128) {
			g_set_error(error, BAR_NETSHARES_ERROR, 0,
			            "user may hold letters, digits and . _ -%s",
			            share->type == BAR_NETSHARES_CIFS
			            ? " (and DOMAIN\\user or user@domain)" : "");
			return FALSE;
		}
	}

	if (share->mountpoint == NULL || share->mountpoint[0] == '\0' ||
	    !g_utf8_validate(share->mountpoint, -1, NULL) ||
	    has_control(share->mountpoint) ||
	    strlen(share->mountpoint) > 1024 ||
	    !(share->mountpoint[0] == '/' ||
	      g_str_has_prefix(share->mountpoint, "~/")) ||
	    has_dotdot(share->mountpoint)) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "mount point must be an absolute path or ~/..., "
		            "without . or .. components");
		return FALSE;
	}

	if (share->port < 0 || share->port > 65535) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "port must be 1-65535");
		return FALSE;
	}
	if (share->nfs_version != NULL && share->nfs_version[0] != '\0' &&
	    (!all_in(share->nfs_version, ".") ||
	     strlen(share->nfs_version) > 5)) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "nfs-version must look like 4.2");
		return FALSE;
	}

	for (i = 0; share->extra != NULL && share->extra[i] != NULL; i++)
		if (!bar_netshares_option_ok(share->extra[i], error))
			return FALSE;

	return TRUE;
}

/*
 * scalar_member:
 * @map: a mapping
 * @key: a key
 *
 * Returns: (nullable): the scalar text at @key, or %NULL when absent,
 *   null, or not a scalar
 */
static const gchar *
scalar_member(YamlMapping *map, const gchar *key)
{
	YamlNode *node;

	if (map == NULL || !yaml_mapping_has_member(map, key))
		return NULL;
	node = yaml_mapping_get_member(map, key);
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

/*
 * parse_bool:
 * @text: (nullable): a YAML scalar
 * @fallback: the value when @text is absent
 * @ok: (out): cleared when @text is present but not a boolean
 */
static gboolean
parse_bool(const gchar *text, gboolean fallback, gboolean *ok)
{
	if (text == NULL || text[0] == '\0')
		return fallback;
	if (g_ascii_strcasecmp(text, "true") == 0 ||
	    g_ascii_strcasecmp(text, "yes") == 0 ||
	    g_ascii_strcasecmp(text, "on") == 0 ||
	    strcmp(text, "1") == 0)
		return TRUE;
	if (g_ascii_strcasecmp(text, "false") == 0 ||
	    g_ascii_strcasecmp(text, "no") == 0 ||
	    g_ascii_strcasecmp(text, "off") == 0 ||
	    strcmp(text, "0") == 0)
		return FALSE;
	*ok = FALSE;
	return fallback;
}

/* The keys a connection and its options may carry.  Anything else is
   reported, because a misspelt `automout:' silently doing nothing is
   exactly the kind of bug a config file hides. */
static const gchar *const connection_keys[] = {
	"id", "name", "type", "server", "share", "path", "export", "user",
	"mountpoint", "automount", "options", NULL
};
static const gchar *const option_keys[] = {
	"encrypt", "noexec", "readonly", "port", "extra", "nfs-version",
	NULL
};

/*
 * warn_unknown_keys:
 * @map: a mapping
 * @known: the accepted keys
 * @where: what the mapping is, for the message
 * @warnings: where to append
 */
static void
warn_unknown_keys(YamlMapping *map, const gchar *const *known,
                  const gchar *where, GPtrArray *warnings)
{
	guint i, n;

	n = yaml_mapping_get_size(map);
	for (i = 0; i < n; i++) {
		const gchar *key = yaml_mapping_get_key(map, i);

		if (key == NULL || g_strv_contains(known, key))
			continue;
		{
			g_autofree gchar *safe = g_utf8_make_valid(key, -1);

			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: unknown key \"%.40s\" ignored", where, safe));
		}
	}
}

/*
 * share_from_mapping:
 * @map: one element of `connections'
 * @index: its position, for messages
 * @warnings: where to say why it was refused
 *
 * Returns: (transfer full) (nullable): the connection, or %NULL when it
 *   was refused (and a warning says why)
 */
static BarNetshare *
share_from_mapping(YamlMapping *map, guint index, GPtrArray *warnings)
{
	g_autoptr(GError) error = NULL;
	BarNetshare *share;
	YamlMapping *opts = NULL;
	const gchar *text;
	g_autofree gchar *where = NULL;
	gboolean ok = TRUE;

	share = bar_netshares_share_new();

	share->id   = g_strdup(scalar_member(map, "id"));
	share->name = g_strdup(scalar_member(map, "name"));
	if (share->name == NULL)
		share->name = g_strdup(share->id);
	if (share->id == NULL && share->name != NULL)
		share->id = bar_netshares_slug(share->name, NULL);

	where = g_strdup_printf("connection %u%s%s", index + 1,
		(share->id != NULL && all_in(share->id, "_-")) ? " " : "",
		(share->id != NULL && all_in(share->id, "_-")) ? share->id : "");

	text = scalar_member(map, "type");
	if (!bar_netshares_type_parse(text != NULL ? text : "cifs",
	                              &share->type)) {
		g_autofree gchar *safe = g_utf8_make_valid(text, -1);

		g_ptr_array_add(warnings, g_strdup_printf(
			"%s: unknown type \"%.20s\" (cifs, nfs or sshfs)",
			where, safe));
		bar_netshares_share_free(share);
		return NULL;
	}

	share->server = g_strdup(scalar_member(map, "server"));
	/* `share', `export' and `path' are one field: whichever word
	   reads naturally for the type. */
	text = scalar_member(map, "share");
	if (text == NULL)
		text = scalar_member(map, "export");
	if (text == NULL)
		text = scalar_member(map, "path");
	share->share      = g_strdup(text != NULL ? text : "");
	share->user       = g_strdup(scalar_member(map, "user"));
	share->mountpoint = g_strdup(scalar_member(map, "mountpoint"));
	share->automount  = parse_bool(scalar_member(map, "automount"),
	                               FALSE, &ok);

	warn_unknown_keys(map, connection_keys, where, warnings);

	if (yaml_mapping_has_member(map, "options")) {
		YamlNode *node = yaml_mapping_get_member(map, "options");

		if (node != NULL &&
		    yaml_node_get_node_type(node) == YAML_NODE_MAPPING)
			opts = yaml_node_get_mapping(node);
		else if (node != NULL &&
		         yaml_node_get_node_type(node) != YAML_NODE_NULL)
			ok = FALSE;
	}
	if (opts != NULL) {
		warn_unknown_keys(opts, option_keys, where, warnings);
		share->encrypt  = parse_bool(scalar_member(opts, "encrypt"),
		                             TRUE, &ok);
		share->noexec   = parse_bool(scalar_member(opts, "noexec"),
		                             TRUE, &ok);
		share->readonly = parse_bool(scalar_member(opts, "readonly"),
		                             FALSE, &ok);
		text = scalar_member(opts, "port");
		if (text != NULL && text[0] != '\0') {
			gint64 port = 0;

			if (!g_ascii_string_to_signed(text, 10, 1, 65535,
			                              &port, NULL))
				ok = FALSE;
			share->port = (gint)port;
		}
		share->nfs_version = g_strdup(scalar_member(opts,
		                                            "nfs-version"));
		if (yaml_mapping_has_member(opts, "extra")) {
			YamlNode *node = yaml_mapping_get_member(opts, "extra");
			GPtrArray *extra = g_ptr_array_new();

			if (node != NULL && yaml_node_get_node_type(node)
			    == YAML_NODE_SEQUENCE) {
				YamlSequence *seq = yaml_node_get_sequence(node);
				guint i, n = yaml_sequence_get_length(seq);

				for (i = 0; i < n && i < 32; i++) {
					YamlNode *el;

					el = yaml_sequence_get_element(seq, i);
					if (el == NULL || yaml_node_get_node_type(el)
					    != YAML_NODE_SCALAR) {
						ok = FALSE;
						continue;
					}
					g_ptr_array_add(extra, g_strdup(
						yaml_node_get_scalar(el)));
				}
			} else if (node != NULL && yaml_node_get_node_type(node)
			           == YAML_NODE_SCALAR) {
				/* `extra: vers=3.0' -- one option, not a list. */
				g_ptr_array_add(extra,
					g_strdup(yaml_node_get_scalar(node)));
			} else if (node != NULL && yaml_node_get_node_type(node)
			           != YAML_NODE_NULL) {
				ok = FALSE;
			}
			g_ptr_array_add(extra, NULL);
			share->extra = (gchar **)g_ptr_array_free(extra, FALSE);
		}
	}

	if (!ok) {
		g_ptr_array_add(warnings, g_strdup_printf(
			"%s: a boolean, port or option list is malformed", where));
		bar_netshares_share_free(share);
		return NULL;
	}
	if (!bar_netshares_validate(share, &error)) {
		g_ptr_array_add(warnings, g_strdup_printf("%s: %s", where,
		                                          error->message));
		bar_netshares_share_free(share);
		return NULL;
	}
	return share;
}

/**
 * bar_netshares_load:
 * @text: the connection file's contents
 * @warnings: (out) (optional) (transfer full) (element-type utf8): why
 *   any connection was refused or a key ignored
 * @error: return location for a #GError
 *
 * Parses the connection file.  A file that is not YAML, or whose shape
 * is wrong, is an error; a single bad connection is not -- it is left
 * out with a warning, so one typo costs one share rather than all of
 * them.  An empty file is an empty list.
 *
 * Returns: (transfer full) (element-type BarNetshare) (nullable): the
 *   connections, or %NULL on error
 */
GPtrArray *
bar_netshares_load(const gchar *text, GPtrArray **warnings, GError **error)
{
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GPtrArray) out = NULL;
	g_autoptr(GPtrArray) warn = NULL;
	YamlNode *root;
	YamlMapping *map;
	YamlNode *conns;
	YamlSequence *seq;
	guint i, n;

	out  = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_netshares_share_free);
	warn = g_ptr_array_new_with_free_func(g_free);

	if (text == NULL || !g_utf8_validate(text, -1, NULL)) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "the connection file is not UTF-8 text");
		return NULL;
	}

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, text, -1, error))
		return NULL;

	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) == YAML_NODE_NULL)
		goto done;
	if (yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "the connection file must be a mapping with a "
		            "`connections:' list");
		return NULL;
	}
	map = yaml_node_get_mapping(root);
	if (!yaml_mapping_has_member(map, "connections"))
		goto done;
	conns = yaml_mapping_get_member(map, "connections");
	if (conns == NULL || yaml_node_get_node_type(conns) == YAML_NODE_NULL)
		goto done;
	if (yaml_node_get_node_type(conns) != YAML_NODE_SEQUENCE) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "`connections:' must be a list");
		return NULL;
	}

	seq = yaml_node_get_sequence(conns);
	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlNode *el = yaml_sequence_get_element(seq, i);
		BarNetshare *share;

		if (out->len >= NETSHARES_MAX_CONNECTIONS) {
			g_ptr_array_add(warn, g_strdup_printf(
				"more than %d connections; the rest are ignored",
				NETSHARES_MAX_CONNECTIONS));
			break;
		}
		if (el == NULL ||
		    yaml_node_get_node_type(el) != YAML_NODE_MAPPING) {
			g_ptr_array_add(warn, g_strdup_printf(
				"connection %u is not a mapping", i + 1));
			continue;
		}
		share = share_from_mapping(yaml_node_get_mapping(el), i, warn);
		if (share == NULL)
			continue;
		if (bar_netshares_find(out, share->id) != NULL) {
			g_ptr_array_add(warn, g_strdup_printf(
				"connection %u: id \"%s\" is used twice; the "
				"second is ignored", i + 1, share->id));
			bar_netshares_share_free(share);
			continue;
		}
		g_ptr_array_add(out, share);
	}

done:
	if (warnings != NULL)
		*warnings = g_steal_pointer(&warn);
	return g_steal_pointer(&out);
}

/*
 * yaml_quote:
 * @out: where to append
 * @text: a string
 *
 * Appends @text as a double-quoted YAML scalar, so a name with a colon
 * or a `#' cannot change the file's structure.
 */
static void
yaml_quote(GString *out, const gchar *text)
{
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = (text != NULL) ? text : ""; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		if (*p == '\n') {
			g_string_append(out, "\\n");
			continue;
		}
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

/**
 * bar_netshares_to_yaml:
 * @shares: (element-type BarNetshare): the connections
 *
 * The connection file for @shares.  Every string is double-quoted,
 * every option written out, so the file reads back to exactly what
 * was saved.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
bar_netshares_to_yaml(GPtrArray *shares)
{
	GString *out;
	guint i, j;

	out = g_string_new(
		"# gowl netshares: network shares the bar widget can mount.\n"
		"# Written by the widget; comments added here are not kept.\n"
		"# Passwords are NOT stored here -- they live in the keyring\n"
		"# (secret-tool, service gowl-netshares).\n");

	if (shares == NULL || shares->len == 0) {
		g_string_append(out, "connections: []\n");
		return g_string_free(out, FALSE);
	}

	g_string_append(out, "connections:\n");
	for (i = 0; i < shares->len; i++) {
		const BarNetshare *s = g_ptr_array_index(shares, i);

		g_string_append(out, "  - id: ");
		yaml_quote(out, s->id);
		g_string_append(out, "\n    name: ");
		yaml_quote(out, s->name);
		g_string_append_printf(out, "\n    type: %s\n    server: ",
		                       bar_netshares_type_name(s->type));
		yaml_quote(out, s->server);
		g_string_append(out, "\n    share: ");
		yaml_quote(out, s->share);
		if (s->user != NULL && s->user[0] != '\0') {
			g_string_append(out, "\n    user: ");
			yaml_quote(out, s->user);
		}
		g_string_append(out, "\n    mountpoint: ");
		yaml_quote(out, s->mountpoint);
		g_string_append_printf(out,
			"\n    automount: %s\n    options:\n"
			"      encrypt: %s\n      noexec: %s\n"
			"      readonly: %s\n",
			s->automount ? "true" : "false",
			s->encrypt ? "true" : "false",
			s->noexec ? "true" : "false",
			s->readonly ? "true" : "false");
		if (s->port > 0)
			g_string_append_printf(out, "      port: %d\n", s->port);
		if (s->nfs_version != NULL && s->nfs_version[0] != '\0') {
			g_string_append(out, "      nfs-version: ");
			yaml_quote(out, s->nfs_version);
			g_string_append_c(out, '\n');
		}
		if (s->extra != NULL && s->extra[0] != NULL) {
			g_string_append(out, "      extra:\n");
			for (j = 0; s->extra[j] != NULL; j++) {
				g_string_append(out, "        - ");
				yaml_quote(out, s->extra[j]);
				g_string_append_c(out, '\n');
			}
		}
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_netshares_find:
 * @shares: (element-type BarNetshare) (nullable): the connections
 * @id: (nullable): an id
 *
 * Returns: (transfer none) (nullable): the connection with @id
 */
BarNetshare *
bar_netshares_find(GPtrArray *shares, const gchar *id)
{
	guint i;

	if (shares == NULL || id == NULL)
		return NULL;
	for (i = 0; i < shares->len; i++) {
		BarNetshare *s = g_ptr_array_index(shares, i);

		if (g_strcmp0(s->id, id) == 0)
			return s;
	}
	return NULL;
}

/**
 * bar_netshares_slug:
 * @name: a display name
 * @existing: (element-type BarNetshare) (nullable): ids to avoid
 *
 * An id for a new connection: the name lower-cased, everything that is
 * not a letter or digit folded to `-', numbered when it is taken.
 *
 * Returns: (transfer full): the id
 */
gchar *
bar_netshares_slug(const gchar *name, GPtrArray *existing)
{
	GString *s;
	const gchar *p;
	g_autofree gchar *base = NULL;
	gchar *candidate;
	guint n;

	s = g_string_new(NULL);
	for (p = (name != NULL) ? name : ""; *p != '\0' && s->len < 40; p++) {
		if (g_ascii_isalnum(*p))
			g_string_append_c(s, g_ascii_tolower(*p));
		else if (s->len > 0 && s->str[s->len - 1] != '-')
			g_string_append_c(s, '-');
	}
	while (s->len > 0 && s->str[s->len - 1] == '-')
		g_string_truncate(s, s->len - 1);
	if (s->len == 0)
		g_string_assign(s, "share");
	base = g_string_free(s, FALSE);

	candidate = g_strdup(base);
	for (n = 2; bar_netshares_find(existing, candidate) != NULL; n++) {
		g_free(candidate);
		candidate = g_strdup_printf("%s-%u", base, n);
	}
	return candidate;
}

/* --- Mount points ------------------------------------------------ */

/**
 * bar_netshares_expand_home:
 * @path: a path, possibly `~' or `~/...'
 * @home: the home directory
 *
 * Returns: (transfer full): @path with a leading `~' replaced
 */
gchar *
bar_netshares_expand_home(const gchar *path, const gchar *home)
{
	if (path == NULL)
		return NULL;
	if (strcmp(path, "~") == 0)
		return g_strdup(home);
	if (g_str_has_prefix(path, "~/"))
		return g_build_filename(home, path + 2, NULL);
	return g_strdup(path);
}

/*
 * canonicalize:
 * @path: an absolute path whose tail may not exist yet
 *
 * realpath() of the longest prefix that exists, with the rest appended
 * as written.  The rest cannot hold a symlink -- it does not exist --
 * so following every link that does exist is enough to know where the
 * mount would really land.
 *
 * Returns: (transfer full) (nullable): the canonical path
 */
static gchar *
canonicalize(const gchar *path)
{
	g_autofree gchar *prefix = NULL;
	GString *tail;
	gchar *real;
	gchar *result;

	prefix = g_strdup(path);
	tail = g_string_new(NULL);

	for (;;) {
		gchar *slash;

		real = realpath(prefix, NULL);
		if (real != NULL)
			break;
		if (errno != ENOENT && errno != ENOTDIR) {
			g_string_free(tail, TRUE);
			return NULL;
		}
		slash = strrchr(prefix, '/');
		if (slash == NULL || slash == prefix) {
			/* Even `/' did not resolve; nothing sane to say. */
			if (slash == prefix && prefix[1] != '\0') {
				g_string_prepend(tail, prefix + 1);
				prefix[1] = '\0';
				continue;
			}
			g_string_free(tail, TRUE);
			return NULL;
		}
		if (tail->len > 0)
			g_string_prepend_c(tail, '/');
		g_string_prepend(tail, slash + 1);
		*slash = '\0';
	}

	if (tail->len == 0)
		result = g_strdup(real);
	else
		result = g_build_filename(real, tail->str, NULL);
	free(real);
	g_string_free(tail, TRUE);
	return result;
}

/*
 * strictly_under:
 * @path: a canonical path
 * @root: a canonical directory
 *
 * Returns: %TRUE when @path is inside @root and is not @root itself
 */
static gboolean
strictly_under(const gchar *path, const gchar *root)
{
	gsize len;

	if (root == NULL || root[0] == '\0')
		return FALSE;
	len = strlen(root);
	if (len == 1 && root[0] == '/')
		return FALSE;
	return strncmp(path, root, len) == 0 && path[len] == '/' &&
	       path[len + 1] != '\0';
}

/**
 * bar_netshares_resolve_mountpoint:
 * @path: the mount point as configured (may start with `~/')
 * @home: the user's home directory
 * @canonical: (out) (transfer full): where the mount would really land
 * @privileged: (out): %TRUE when creating it needs root (outside home)
 * @error: return location for a #GError
 *
 * Decides whether a mount point is acceptable.  It must resolve --
 * following every symlink that exists -- strictly inside /mnt, /media
 * or @home; never one of those directories itself, never `/', and
 * never by way of a `..' component.  A link in the home directory that
 * points at /etc is followed and refused, which is the whole reason
 * this resolves rather than comparing strings: a CIFS or NFS mount is
 * made by root, and root would happily mount over whatever the link
 * named.
 *
 * Checks the filesystem, so call it from a worker: a path under a hung
 * NFS mount blocks in realpath().
 *
 * Returns: %TRUE when the mount point is acceptable
 */
gboolean
bar_netshares_resolve_mountpoint(const gchar *path, const gchar *home,
                                 gchar **canonical, gboolean *privileged,
                                 GError **error)
{
	g_autofree gchar *expanded = NULL;
	g_autofree gchar *real = NULL;
	g_autofree gchar *real_home = NULL;
	g_autofree gchar *real_mnt = NULL;
	g_autofree gchar *real_media = NULL;
	GStatBuf st;

	if (canonical != NULL)
		*canonical = NULL;
	if (privileged != NULL)
		*privileged = FALSE;

	if (path == NULL || path[0] == '\0' || home == NULL ||
	    home[0] != '/') {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "no mount point configured");
		return FALSE;
	}
	expanded = bar_netshares_expand_home(path, home);
	if (expanded[0] != '/' || has_dotdot(expanded) ||
	    has_control(expanded)) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "mount point must be absolute, without . or ..");
		return FALSE;
	}

	real = canonicalize(expanded);
	if (real == NULL || strcmp(real, "/") == 0) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "mount point cannot be resolved");
		return FALSE;
	}

	/* The roots are resolved too: on Silverblue /home is a link to
	   /var/home and $HOME may be spelt either way. */
	real_home  = canonicalize(home);
	real_mnt   = canonicalize("/mnt");
	real_media = canonicalize("/media");

	if (strictly_under(real, real_home)) {
		if (privileged != NULL)
			*privileged = FALSE;
	} else if (strictly_under(real, real_mnt) ||
	           strictly_under(real, real_media)) {
		if (privileged != NULL)
			*privileged = TRUE;
	} else {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "mount point must be inside /mnt, /media or your "
		            "home directory (it resolves to %s)", real);
		return FALSE;
	}

	if (g_stat(real, &st) == 0 && !S_ISDIR(st.st_mode)) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "mount point %s exists and is not a folder", real);
		return FALSE;
	}

	if (canonical != NULL)
		*canonical = g_steal_pointer(&real);
	return TRUE;
}

/* --- The mount table --------------------------------------------- */

/**
 * bar_netshares_mount_free:
 * @mount: (nullable): an entry
 */
void
bar_netshares_mount_free(BarNetsharesMount *mount)
{
	if (mount == NULL)
		return;
	g_free(mount->mountpoint);
	g_free(mount->fstype);
	g_free(mount->source);
	g_free(mount);
}

/**
 * bar_netshares_unescape_octal:
 * @text: a mountinfo field
 *
 * The kernel writes a space in a path as `\040', a tab as `\011', a
 * newline as `\012' and a backslash as `\134'.  A mount point with a
 * space in it never matched its own entry until this was undone.
 *
 * Returns: (transfer full): the unescaped text
 */
gchar *
bar_netshares_unescape_octal(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);
	for (p = text; *p != '\0'; p++) {
		if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' &&
		    p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
			g_string_append_c(out, (gchar)(((p[1] - '0') << 6) |
			                               ((p[2] - '0') << 3) |
			                               (p[3] - '0')));
			p += 3;
			continue;
		}
		g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_netshares_parse_mountinfo:
 * @text: the contents of /proc/self/mountinfo
 *
 * Fields are space-separated: the mount point is the fifth; after an
 * optional run of tagged fields comes a lone `-', then the filesystem
 * type and the source.  A line without the separator is skipped rather
 * than guessed at.
 *
 * Returns: (transfer full) (element-type BarNetsharesMount): the mounts
 */
GPtrArray *
bar_netshares_parse_mountinfo(const gchar *text)
{
	GPtrArray *out;
	g_auto(GStrv) lines = NULL;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_netshares_mount_free);
	if (text == NULL)
		return out;

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) f = NULL;
		BarNetsharesMount *m;
		guint n, sep;

		if (lines[i][0] == '\0')
			continue;
		f = g_strsplit(lines[i], " ", -1);
		n = g_strv_length(f);
		if (n < 7)
			continue;
		for (sep = 6; sep < n; sep++)
			if (strcmp(f[sep], "-") == 0)
				break;
		if (sep + 2 >= n)
			continue;

		m = g_new0(BarNetsharesMount, 1);
		m->mountpoint = bar_netshares_unescape_octal(f[4]);
		m->fstype     = bar_netshares_unescape_octal(f[sep + 1]);
		m->source     = bar_netshares_unescape_octal(f[sep + 2]);
		g_ptr_array_add(out, m);
	}
	return out;
}

/**
 * bar_netshares_fstype_matches:
 * @type: a connection type
 * @fstype: a mountinfo filesystem type
 *
 * CIFS shows up as `cifs' or, on newer kernels, `smb3'; NFS as `nfs' or
 * `nfs4'; SSHFS as `fuse.sshfs'.
 *
 * Returns: %TRUE when @fstype is what a @type mount looks like
 */
gboolean
bar_netshares_fstype_matches(BarNetsharesType type, const gchar *fstype)
{
	if (fstype == NULL)
		return FALSE;
	switch (type) {
	case BAR_NETSHARES_CIFS:
		return strcmp(fstype, "cifs") == 0 ||
		       strcmp(fstype, "smb3") == 0;
	case BAR_NETSHARES_NFS:
		return strcmp(fstype, "nfs") == 0 ||
		       strcmp(fstype, "nfs4") == 0;
	case BAR_NETSHARES_SSHFS:
		return strcmp(fstype, "fuse.sshfs") == 0 ||
		       strcmp(fstype, "fuse.sshfs3") == 0;
	case BAR_NETSHARES_N_TYPES:
	default:
		return FALSE;
	}
}

/**
 * bar_netshares_find_mount:
 * @mounts: (element-type BarNetsharesMount): the mount table
 * @type: the connection type
 * @mountpoint: the canonical mount point
 *
 * The LAST matching entry wins, because a later mount over the same
 * point hides the earlier one.
 *
 * Returns: (transfer none) (nullable): the entry, if mounted
 */
const BarNetsharesMount *
bar_netshares_find_mount(GPtrArray *mounts, BarNetsharesType type,
                         const gchar *mountpoint)
{
	const BarNetsharesMount *found = NULL;
	guint i;

	if (mounts == NULL || mountpoint == NULL)
		return NULL;
	for (i = 0; i < mounts->len; i++) {
		const BarNetsharesMount *m = g_ptr_array_index(mounts, i);

		if (strcmp(m->mountpoint, mountpoint) != 0)
			continue;
		found = bar_netshares_fstype_matches(type, m->fstype) ? m : NULL;
	}
	return found;
}

/* --- Command lines ----------------------------------------------- */

/**
 * bar_netshares_device:
 * @share: a validated connection
 *
 * The mount source: `//server/share', `server:/export' (an IPv6
 * address bracketed) or `[user@]server:path'.
 *
 * Returns: (transfer full): the device argument
 */
gchar *
bar_netshares_device(const BarNetshare *share)
{
	g_autofree gchar *host = NULL;

	if (share->server[0] != '[' && strchr(share->server, ':') != NULL)
		host = g_strdup_printf("[%s]", share->server);
	else
		host = g_strdup(share->server);

	switch (share->type) {
	case BAR_NETSHARES_CIFS:
		return g_strdup_printf("//%s/%s", share->server, share->share);
	case BAR_NETSHARES_NFS:
		return g_strdup_printf("%s:%s", host, share->share);
	case BAR_NETSHARES_SSHFS:
	case BAR_NETSHARES_N_TYPES:
	default:
		if (share->user != NULL && share->user[0] != '\0')
			return g_strdup_printf("%s@%s:%s", share->user, host,
			                       share->share != NULL
			                       ? share->share : "");
		return g_strdup_printf("%s:%s", host,
		                       share->share != NULL ? share->share : "");
	}
}

/*
 * append_extra:
 * @opts: the option list being built
 * @share: the connection
 *
 * Re-checks each extra option as it is appended: a connection built in
 * code rather than loaded must not slip past the filter.
 */
static void
append_extra(GString *opts, const BarNetshare *share)
{
	guint i;

	for (i = 0; share->extra != NULL && share->extra[i] != NULL; i++)
		if (bar_netshares_option_ok(share->extra[i], NULL))
			g_string_append_printf(opts, ",%s", share->extra[i]);
}

/**
 * bar_netshares_mount_options:
 * @share: a validated connection
 * @cred_path: (nullable): the CIFS credentials file, or %NULL for a
 *   guest mount
 * @uid: the user's uid, which owns the mounted files
 * @gid: the user's gid
 * @nfs_version: (nullable): the fallback `vers=' for NFS
 *
 * The `-o' list.  `nosuid,nodev' is always there for the root-made
 * mounts; FUSE has them by default.
 *
 * Returns: (transfer full): the options, comma-separated
 */
gchar *
bar_netshares_mount_options(const BarNetshare *share, const gchar *cred_path,
                            guint uid, guint gid, const gchar *nfs_version)
{
	GString *opts;
	const gchar *vers;

	opts = g_string_new(NULL);

	switch (share->type) {
	case BAR_NETSHARES_CIFS:
		if (cred_path != NULL)
			g_string_append_printf(opts, "credentials=%s", cred_path);
		else
			g_string_append(opts, "guest");
		g_string_append_printf(opts,
			",uid=%u,gid=%u,file_mode=0600,dir_mode=0700", uid, gid);
		if (share->encrypt)
			g_string_append(opts, ",seal");
		g_string_append(opts, ",nosuid,nodev");
		break;
	case BAR_NETSHARES_NFS:
		g_string_append(opts, "nosuid,nodev");
		vers = (share->nfs_version != NULL &&
		        share->nfs_version[0] != '\0')
			? share->nfs_version : nfs_version;
		if (vers != NULL && vers[0] != '\0' && all_in(vers, "."))
			g_string_append_printf(opts, ",vers=%s", vers);
		break;
	case BAR_NETSHARES_SSHFS:
	case BAR_NETSHARES_N_TYPES:
	default:
		/* BatchMode: key or agent only.  A password prompt would
		   go to a terminal nobody is looking at, and the mount
		   would sit there until the timeout. */
		g_string_append(opts,
			"reconnect,ServerAliveInterval=15,BatchMode=yes,"
			"idmap=user");
		break;
	}

	if (share->noexec)
		g_string_append(opts, ",noexec");
	g_string_append(opts, share->readonly ? ",ro" : ",rw");
	if (share->port > 0)
		g_string_append_printf(opts, ",port=%d", share->port);
	append_extra(opts, share);
	return g_string_free(opts, FALSE);
}

/*
 * The wrapper used when a mount point under /mnt or /media does not
 * exist yet.  Creating it needs root as much as the mount does, and
 * two polkit prompts for one click would be absurd -- so both happen
 * in ONE pkexec call.  The script is a constant; every value reaches it
 * as a positional parameter, never as part of the script text, so a
 * hostile path cannot be interpreted by the shell.
 */
#define NETSHARES_MKDIR_SCRIPT \
	"mkdir -p -m 0700 -- \"$1\" && chown -- \"$2\" \"$1\" && " \
	"shift 2 && exec mount \"$@\""

/**
 * bar_netshares_mount_argv:
 * @share: a validated connection
 * @mountpoint: the canonical mount point
 * @tools: the programs to name
 * @cred_path: (nullable): the CIFS credentials file
 * @uid: the user's uid
 * @gid: the user's gid
 * @create: whether the mount point must be created by root first
 * @nfs_version: (nullable): the fallback NFS version
 *
 * The full command line.  No password is ever an argument: CIFS reads
 * it from @cred_path, SSHFS uses a key or the agent.
 *
 * Returns: (transfer full): a %NULL-terminated argv
 */
gchar **
bar_netshares_mount_argv(const BarNetshare *share, const gchar *mountpoint,
                         const BarNetsharesTools *tools,
                         const gchar *cred_path, guint uid, guint gid,
                         gboolean create, const gchar *nfs_version)
{
	GPtrArray *argv;
	g_autofree gchar *device = NULL;
	g_autofree gchar *opts = NULL;

	argv   = g_ptr_array_new();
	device = bar_netshares_device(share);
	opts   = bar_netshares_mount_options(share, cred_path, uid, gid,
	                                     nfs_version);

	if (share->type == BAR_NETSHARES_SSHFS) {
		g_ptr_array_add(argv, g_strdup(tools->sshfs));
		g_ptr_array_add(argv, g_strdup(device));
		g_ptr_array_add(argv, g_strdup(mountpoint));
		g_ptr_array_add(argv, g_strdup("-o"));
		g_ptr_array_add(argv, g_strdup(opts));
		g_ptr_array_add(argv, NULL);
		return (gchar **)g_ptr_array_free(argv, FALSE);
	}

	g_ptr_array_add(argv, g_strdup(tools->pkexec));
	if (create) {
		g_ptr_array_add(argv, g_strdup("/bin/sh"));
		g_ptr_array_add(argv, g_strdup("-c"));
		g_ptr_array_add(argv, g_strdup(NETSHARES_MKDIR_SCRIPT));
		g_ptr_array_add(argv, g_strdup("gowl-netshares"));
		g_ptr_array_add(argv, g_strdup(mountpoint));
		g_ptr_array_add(argv, g_strdup_printf("%u:%u", uid, gid));
	} else {
		g_ptr_array_add(argv, g_strdup("mount"));
	}
	g_ptr_array_add(argv, g_strdup("-t"));
	g_ptr_array_add(argv, g_strdup(
		share->type == BAR_NETSHARES_CIFS ? "cifs" : "nfs"));
	g_ptr_array_add(argv, g_strdup(device));
	g_ptr_array_add(argv, g_strdup(mountpoint));
	g_ptr_array_add(argv, g_strdup("-o"));
	g_ptr_array_add(argv, g_strdup(opts));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/**
 * bar_netshares_unmount_argv:
 * @share: a connection
 * @mountpoint: the canonical mount point
 * @tools: the programs to name
 * @lazy: detach now and clean up when no longer busy
 *
 * `pkexec umount' for the root-made mounts, `fusermount3 -u' for SSHFS
 * (which the user made and can unmount without asking anyone).
 *
 * Returns: (transfer full): a %NULL-terminated argv
 */
gchar **
bar_netshares_unmount_argv(const BarNetshare *share, const gchar *mountpoint,
                           const BarNetsharesTools *tools, gboolean lazy)
{
	GPtrArray *argv;

	argv = g_ptr_array_new();
	if (share->type == BAR_NETSHARES_SSHFS) {
		g_ptr_array_add(argv, g_strdup(tools->fusermount));
		g_ptr_array_add(argv, g_strdup(lazy ? "-uz" : "-u"));
	} else {
		g_ptr_array_add(argv, g_strdup(tools->pkexec));
		g_ptr_array_add(argv, g_strdup("umount"));
		if (lazy)
			g_ptr_array_add(argv, g_strdup("-l"));
	}
	g_ptr_array_add(argv, g_strdup(mountpoint));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/**
 * bar_netshares_secret_argv:
 * @tool: the secret-tool program
 * @verb: `lookup', `store' or `clear'
 * @id: the connection id
 * @name: (nullable): the display name, for the store label
 *
 * The keyring entry is keyed on `service gowl-netshares id <id>'.  The
 * secret itself is never an argument: `store' reads it from stdin.
 *
 * Returns: (transfer full): a %NULL-terminated argv
 */
gchar **
bar_netshares_secret_argv(const gchar *tool, const gchar *verb,
                          const gchar *id, const gchar *name)
{
	GPtrArray *argv;

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, g_strdup(tool));
	g_ptr_array_add(argv, g_strdup(verb));
	if (strcmp(verb, "store") == 0)
		g_ptr_array_add(argv, g_strdup_printf("--label=gowl netshares %s",
			(name != NULL) ? name : id));
	g_ptr_array_add(argv, g_strdup("service"));
	g_ptr_array_add(argv, g_strdup("gowl-netshares"));
	g_ptr_array_add(argv, g_strdup("id"));
	g_ptr_array_add(argv, g_strdup(id));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* --- Credentials ------------------------------------------------- */

/**
 * bar_netshares_secret_free:
 * @secret: (nullable): a string holding a password
 *
 * Overwrites, then frees.  Not a guarantee against every copy the
 * allocator made, but no password is left lying in a freed block for
 * the next allocation to hand out.
 */
void
bar_netshares_secret_free(gchar *secret)
{
	if (secret == NULL)
		return;
	memset(secret, 0, strlen(secret));
	g_free(secret);
}

/**
 * bar_netshares_credentials_text:
 * @user: the user, possibly `DOMAIN\user'
 * @password: the password
 * @error: return location for a #GError
 *
 * The mount.cifs credentials file format: one `key=value' per line.
 * A newline in either value would start a new key, so it is refused.
 *
 * Returns: (transfer full) (nullable): the file's contents; free with
 *   bar_netshares_secret_free()
 */
gchar *
bar_netshares_credentials_text(const gchar *user, const gchar *password,
                               GError **error)
{
	const gchar *slash;

	if (user == NULL || user[0] == '\0' || password == NULL ||
	    strchr(user, '\n') != NULL || strchr(user, '\r') != NULL ||
	    strchr(password, '\n') != NULL || strchr(password, '\r') != NULL) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "the user or password holds a line break");
		return NULL;
	}

	slash = strchr(user, '\\');
	if (slash != NULL)
		return g_strdup_printf("username=%s\npassword=%s\ndomain=%.*s\n",
		                       slash + 1, password,
		                       (gint)(slash - user), user);
	return g_strdup_printf("username=%s\npassword=%s\n", user, password);
}

/**
 * bar_netshares_write_credentials:
 * @dir: the directory (created 0700 if missing)
 * @id: the connection id, used for the file name
 * @user: the user
 * @password: the password
 * @error: return location for a #GError
 *
 * Writes the file with O_CREAT|O_EXCL|O_NOFOLLOW at mode 0600: a stale
 * file or a planted symlink at that name is removed first and never
 * written through.  The caller deletes the file the moment mount
 * returns.
 *
 * Returns: (transfer full) (nullable): the file's path
 */
gchar *
bar_netshares_write_credentials(const gchar *dir, const gchar *id,
                                const gchar *user, const gchar *password,
                                GError **error)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *name = NULL;
	gchar *text;
	gsize len, done;
	gint fd;
	GStatBuf st;

	if (!all_in(id, "_-")) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0, "bad connection id");
		return NULL;
	}
	if (g_mkdir_with_parents(dir, 0700) != 0 ||
	    g_lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
	    st.st_uid != getuid()) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "cannot use %s for the credentials file", dir);
		return NULL;
	}
	/* Someone else's group or world bits on our directory are fixed
	   rather than refused: we own it. */
	if ((st.st_mode & 077) != 0)
		g_chmod(dir, 0700);

	text = bar_netshares_credentials_text(user, password, error);
	if (text == NULL)
		return NULL;

	name = g_strdup_printf("%s.cred", id);
	path = g_build_filename(dir, name, NULL);
	g_unlink(path);

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
	          0600);
	if (fd < 0) {
		g_set_error(error, BAR_NETSHARES_ERROR, 0,
		            "cannot create the credentials file: %s",
		            g_strerror(errno));
		bar_netshares_secret_free(text);
		return NULL;
	}
	/* The umask cannot widen 0600, but fchmod says what we mean. */
	fchmod(fd, 0600);

	len = strlen(text);
	done = 0;
	while (done < len) {
		gssize w = write(fd, text + done, len - done);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			g_set_error(error, BAR_NETSHARES_ERROR, 0,
			            "cannot write the credentials file");
			close(fd);
			g_unlink(path);
			bar_netshares_secret_free(text);
			return NULL;
		}
		done += (gsize)w;
	}
	close(fd);
	bar_netshares_secret_free(text);
	return g_steal_pointer(&path);
}

/* --- Running ----------------------------------------------------- */

/*
 * run_child_setup:
 *
 * A process group of its own, so a timeout kills the helper AND what
 * it started (ssh under sshfs) rather than only the first process.
 */
static void
run_child_setup(gpointer data)
{
	(void)data;
	setpgid(0, 0);
}

/*
 * reap_thread:
 * @data: the pid, as a pointer
 *
 * A child that survives SIGKILL is one we were not allowed to kill:
 * pkexec has turned into a root `mount'.  It will finish on its own,
 * and something has to wait for it or it lingers as a zombie.  Not a
 * GLib child watch -- that would fire on the default main context,
 * which under cmacs is the editor's thread.
 */
static gpointer
reap_thread(gpointer data)
{
	gint status;

	while (waitpid((pid_t)GPOINTER_TO_INT(data), &status, 0) < 0 &&
	       errno == EINTR)
		;
	return NULL;
}

/*
 * read_some:
 * @fd: a pipe
 * @into: where to append, bounded by NETSHARES_MAX_OUTPUT
 *
 * Returns: %FALSE at end of file or on an error other than EAGAIN
 */
static gboolean
read_some(gint fd, GString *into)
{
	gchar buf[4096];
	gssize r;

	r = read(fd, buf, sizeof(buf));
	if (r < 0)
		return (errno == EAGAIN || errno == EINTR);
	if (r == 0)
		return FALSE;
	if (into->len < NETSHARES_MAX_OUTPUT)
		g_string_append_len(into, buf,
			MIN((gsize)r, NETSHARES_MAX_OUTPUT - into->len));
	return TRUE;
}

/*
 * drain:
 * @fd: a non-blocking pipe
 * @into: where to append
 *
 * Reads what is already buffered and stops at the first EAGAIN: a
 * grandchild still holding the pipe open must not hold us too.
 */
static void
drain(gint fd, GString *into)
{
	gchar buf[4096];
	gssize r;

	for (;;) {
		r = read(fd, buf, sizeof(buf));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return;
		if (into->len < NETSHARES_MAX_OUTPUT)
			g_string_append_len(into, buf,
				MIN((gsize)r, NETSHARES_MAX_OUTPUT - into->len));
	}
}

/*
 * write_input:
 * @fd: the child's stdin, non-blocking
 * @input: what to write
 * @deadline: monotonic us to give up at
 *
 * Two ways a write to a child can hurt the compositor.  A child that
 * exits without reading turns it into a SIGPIPE, whose default action
 * ends the PROCESS -- so SIGPIPE is blocked on this thread for the
 * length of the write and any that arrived is consumed before the mask
 * is restored.  And a child that neither reads nor exits would block a
 * blocking write forever -- so the pipe is non-blocking and the write
 * gives up at @deadline.
 */
static void
write_input(gint fd, const gchar *input, gint64 deadline)
{
	sigset_t pipe_set, old_set;
	struct timespec zero = { 0, 0 };
	gsize len, done;

	sigemptyset(&pipe_set);
	sigaddset(&pipe_set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);

	len = strlen(input);
	done = 0;
	while (done < len && g_get_monotonic_time() < deadline) {
		gssize w = write(fd, input + done, len - done);

		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && errno == EAGAIN) {
			struct pollfd pfd;

			pfd.fd = fd;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			poll(&pfd, 1, 50);
			continue;
		}
		if (w <= 0)
			break;
		done += (gsize)w;
	}

	while (sigtimedwait(&pipe_set, NULL, &zero) > 0)
		;
	pthread_sigmask(SIG_SETMASK, &old_set, NULL);
}

/**
 * bar_netshares_run:
 * @argv: (array zero-terminated=1): the command
 * @input: (nullable): written to its stdin, which is then closed
 * @timeout_s: seconds before the process group is killed
 * @result: (out caller-allocates): what happened; clear with
 *   bar_netshares_run_clear()
 * @error: return location for a #GError, set only when it could not
 *   be started
 *
 * Runs a command to completion with a deadline.  Blocking: workers
 * only.
 *
 * Two details that matter.  It stops reading when the CHILD exits, not
 * at end of file: sshfs leaves ssh running with the original stderr,
 * so the pipe never closes and waiting for EOF would turn every
 * successful mount into a timeout.  And at the deadline the whole
 * process group gets SIGTERM, then SIGKILL; a child that is by then a
 * root process we may not signal is left to a reaper thread and the
 * call still returns on time.
 *
 * Returns: %TRUE when the program was started
 */
gboolean
bar_netshares_run(const gchar * const *argv, const gchar *input,
                  gint timeout_s, BarNetsharesRun *result, GError **error)
{
	GPid pid;
	gint in_fd = -1, out_fd = -1, err_fd = -1;
	GString *out, *err;
	gint64 deadline;
	gint status = 0;
	gboolean exited = FALSE;
	gboolean out_open = TRUE, err_open = TRUE;

	memset(result, 0, sizeof(*result));
	result->status = -1;

	if (!g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
	                              G_SPAWN_DO_NOT_REAP_CHILD |
	                              G_SPAWN_SEARCH_PATH,
	                              run_child_setup, NULL, &pid,
	                              &in_fd, &out_fd, &err_fd, error))
		return FALSE;
	result->spawned = TRUE;

	deadline = g_get_monotonic_time() +
	           (gint64)MAX(timeout_s, 1) * G_USEC_PER_SEC;

	/* stdin first, bounded by a slice of the deadline: the inputs
	   here are a password, a few bytes, and a child that will not
	   take them is one to report rather than wait on. */
	fcntl(in_fd, F_SETFL, fcntl(in_fd, F_GETFL) | O_NONBLOCK);
	if (input != NULL)
		write_input(in_fd, input,
		            MIN(deadline, g_get_monotonic_time() +
		                          2 * G_USEC_PER_SEC));
	close(in_fd);

	fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL) | O_NONBLOCK);
	fcntl(err_fd, F_SETFL, fcntl(err_fd, F_GETFL) | O_NONBLOCK);

	out = g_string_new(NULL);
	err = g_string_new(NULL);

	for (;;) {
		struct pollfd fds[2];
		nfds_t nfds = 0;
		pid_t r;

		r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			exited = TRUE;
			break;
		}
		if (g_get_monotonic_time() >= deadline)
			break;

		if (out_open) {
			fds[nfds].fd = out_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}
		if (err_open) {
			fds[nfds].fd = err_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}
		if (nfds == 0) {
			/* Both pipes closed: only the exit is left. */
			g_usleep(20 * 1000);
			continue;
		}
		if (poll(fds, nfds, 50) > 0) {
			nfds_t k;

			for (k = 0; k < nfds; k++) {
				if (fds[k].revents == 0)
					continue;
				if (fds[k].fd == out_fd && !read_some(out_fd, out))
					out_open = FALSE;
				else if (fds[k].fd == err_fd &&
				         !read_some(err_fd, err))
					err_open = FALSE;
			}
		}
	}

	if (!exited) {
		gint i;

		result->timed_out = TRUE;
		kill(-pid, SIGTERM);
		for (i = 0; i < 20 && !exited; i++) {
			g_usleep(25 * 1000);
			exited = (waitpid(pid, &status, WNOHANG) == pid);
		}
		if (!exited) {
			kill(-pid, SIGKILL);
			for (i = 0; i < 40 && !exited; i++) {
				g_usleep(25 * 1000);
				exited = (waitpid(pid, &status, WNOHANG) == pid);
			}
		}
		if (!exited)
			g_thread_unref(g_thread_new("netshares-reap",
				reap_thread, GINT_TO_POINTER((gint)pid)));
	}

	/* Whatever is already in the pipes; never wait for more. */
	if (out_open)
		drain(out_fd, out);
	if (err_open)
		drain(err_fd, err);
	close(out_fd);
	close(err_fd);

	if (exited && WIFEXITED(status) && !result->timed_out)
		result->status = WEXITSTATUS(status);
	result->out = g_string_free(out, FALSE);
	result->err = g_string_free(err, FALSE);
	g_spawn_close_pid(pid);
	return TRUE;
}

/**
 * bar_netshares_run_clear:
 * @result: a result
 *
 * Frees the captured output.  Overwritten first: `secret-tool lookup'
 * prints a password there.
 */
void
bar_netshares_run_clear(BarNetsharesRun *result)
{
	bar_netshares_secret_free(result->out);
	bar_netshares_secret_free(result->err);
	result->out = NULL;
	result->err = NULL;
}

/**
 * bar_netshares_last_line:
 * @text: (nullable): a helper's stderr
 * @secret: (nullable): a string that must not survive into the result
 *
 * The line worth showing a person: the last one naming an error, else
 * the last non-empty one, made valid
 * UTF-8, control characters turned into spaces, cut to fit a panel
 * row, and with @secret masked should a helper ever echo it.
 *
 * Returns: (transfer full) (nullable): the line, or %NULL if @text has
 *   nothing to say
 */
gchar *
bar_netshares_last_line(const gchar *text, const gchar *secret)
{
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *valid = NULL;
	GString *out;
	gchar *p;
	gint i;

	if (text == NULL)
		return NULL;
	valid = g_utf8_make_valid(text, -1);
	lines = g_strsplit(valid, "\n", -1);
	/* The last line that names an error, if any: mount.cifs ends
	   every failure with "Refer to the mount.cifs(8) manual page",
	   which is true and useless, one line after the reason. */
	for (i = (gint)g_strv_length(lines) - 1; i >= 0; i--) {
		g_autofree gchar *low = NULL;

		g_strstrip(lines[i]);
		low = g_ascii_strdown(lines[i], -1);
		if (strstr(low, "error") != NULL && !g_str_has_prefix(low,
		                                                  "refer to"))
			break;
	}
	if (i < 0) {
		for (i = (gint)g_strv_length(lines) - 1; i >= 0; i--)
			if (lines[i][0] != '\0')
				break;
	}
	if (i < 0)
		return NULL;

	out = g_string_new(lines[i]);
	for (p = out->str; *p != '\0'; p++)
		if ((guchar)*p < 0x20 || *p == 0x7f)
			*p = ' ';
	if (secret != NULL && secret[0] != '\0') {
		gchar *hit;

		while ((hit = strstr(out->str, secret)) != NULL) {
			gsize at = (gsize)(hit - out->str);

			g_string_erase(out, (gssize)at, (gssize)strlen(secret));
			g_string_insert(out, (gssize)at, "***");
		}
	}
	if (g_utf8_strlen(out->str, -1) > NETSHARES_MAX_LINE) {
		gchar *end = g_utf8_offset_to_pointer(out->str,
		                                      NETSHARES_MAX_LINE);

		g_string_truncate(out, (gsize)(end - out->str));
		g_string_append(out, "...");
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_netshares_is_busy:
 * @text: (nullable): an unmount helper's stderr
 *
 * Returns: %TRUE when the unmount failed because files are open
 */
gboolean
bar_netshares_is_busy(const gchar *text)
{
	g_autofree gchar *low = NULL;

	if (text == NULL)
		return FALSE;
	low = g_ascii_strdown(text, -1);
	return strstr(low, "target is busy") != NULL ||
	       strstr(low, "device or resource busy") != NULL ||
	       strstr(low, "device is busy") != NULL;
}

/**
 * bar_netshares_find_program:
 * @name: a program name or an absolute path
 * @search_path: (nullable): a colon-separated directory list to search
 *   INSTEAD of `PATH'; %NULL for `PATH' plus /usr/sbin and /sbin
 *
 * The mount helpers live in sbin, which a user's `PATH' may lack; the
 * explicit list is how a test hides the real ones.
 *
 * Returns: (transfer full) (nullable): the full path, or %NULL
 */
gchar *
bar_netshares_find_program(const gchar *name, const gchar *search_path)
{
	g_auto(GStrv) dirs = NULL;
	gchar *found;
	guint i;

	if (name == NULL || name[0] == '\0')
		return NULL;
	if (strchr(name, '/') != NULL)
		return g_file_test(name, G_FILE_TEST_IS_EXECUTABLE)
			? g_strdup(name) : NULL;

	if (search_path == NULL) {
		found = g_find_program_in_path(name);
		if (found != NULL)
			return found;
		search_path = "/usr/sbin:/sbin";
	}
	dirs = g_strsplit(search_path, ":", -1);
	for (i = 0; dirs[i] != NULL; i++) {
		g_autofree gchar *path = NULL;

		if (dirs[i][0] == '\0')
			continue;
		path = g_build_filename(dirs[i], name, NULL);
		if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE) &&
		    !g_file_test(path, G_FILE_TEST_IS_DIR))
			return g_steal_pointer(&path);
	}
	return NULL;
}

/**
 * bar_netshares_package_for:
 * @program: a program the widget needs
 *
 * The Fedora package that ships it, so a missing tool can be named
 * with the one thing needed to fix it.
 *
 * Returns: (transfer none): the package name
 */
const gchar *
bar_netshares_package_for(const gchar *program)
{
	const gchar *base;

	base = (program != NULL && strrchr(program, '/') != NULL)
		? strrchr(program, '/') + 1 : program;
	if (base == NULL)
		return "?";
	if (strcmp(base, "mount.cifs") == 0)
		return "cifs-utils";
	if (strcmp(base, "mount.nfs") == 0)
		return "nfs-utils";
	if (strcmp(base, "sshfs") == 0)
		return "fuse-sshfs";
	if (g_str_has_prefix(base, "fusermount"))
		return "fuse3";
	if (strcmp(base, "pkexec") == 0)
		return "polkit";
	if (strcmp(base, "secret-tool") == 0)
		return "libsecret";
	if (strcmp(base, "xdg-open") == 0)
		return "xdg-utils";
	return base;
}
