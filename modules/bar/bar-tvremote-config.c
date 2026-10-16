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
 * The tvremote device list: ~/.config/gowl/tvremote.yaml.
 *
 *   default: living-room
 *   devices:
 *     - id: living-room
 *       name: Living room
 *       kind: firetv
 *       host: 192.168.1.20
 *     - id: soundbar
 *       kind: samsung
 *       host: 192.168.1.30
 *       token: "12345678"
 *       volume-via: ...
 *
 * The file holds pairing secrets -- Samsung's token, LG's client key --
 * that let anyone on the LAN who has them drive the TV without a
 * prompt.  So it is READ only when no one but its owner can read it,
 * and WRITTEN atomically at mode 0600: a temporary file created with
 * that mode and renamed over the original, so there is no moment at
 * which the secrets sit in a world-readable file, and no moment at
 * which a crash leaves half a file.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>

#include "yaml-glib.h"

#include "bar-tvremote-model.h"

/* Nobody's TV list is 64 KiB; a file that size is not ours. */
#define CONFIG_MAX_BYTES (64 * 1024)

G_DEFINE_QUARK(bar-tvremote-config-error-quark, bar_tvremote_config_error)

/* The scalar text at @key, or NULL when missing or not a scalar. */
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

/* A scalar from the file as a clean, bounded string or NULL. */
static gchar *
dup_scalar(YamlMapping *map, const gchar *key, gsize max)
{
	const gchar *v = scalar_member(map, key);

	if (v == NULL || v[0] == '\0')
		return NULL;
	return bar_tvremote_clean_text(v, max);
}

/**
 * bar_tvremote_config_parse:
 * @text: the file's contents
 * @default_id: (out) (optional) (nullable): the `default' device id
 * @error: return location for a #GError
 *
 * Devices with no usable host or an unknown kind are skipped (with a
 * warning): one bad entry must not cost the user every other TV.  A
 * missing id is made from the kind and host; a duplicate id keeps the
 * first entry.
 *
 * Returns: (transfer full) (nullable) (element-type BarTvremoteDevice):
 *   the devices, or %NULL with @error set when @text is not YAML or
 *   not a mapping
 */
GPtrArray *
bar_tvremote_config_parse(const gchar *text, gchar **default_id,
                          GError **error)
{
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	YamlNode *root, *node;
	YamlMapping *map;
	YamlSequence *seq;
	guint i, n;

	if (default_id != NULL)
		*default_id = NULL;
	devices = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_device_free);
	if (text == NULL || text[0] == '\0')
		return g_steal_pointer(&devices);

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, text, -1, error))
		return NULL;
	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) == YAML_NODE_NULL)
		return g_steal_pointer(&devices);
	map = yaml_node_get_mapping(root);
	if (map == NULL) {
		g_set_error(error, BAR_TVREMOTE_CONFIG_ERROR,
		            BAR_TVREMOTE_CONFIG_ERROR_PARSE,
		            "the device list is not a YAML mapping");
		return NULL;
	}

	if (default_id != NULL)
		*default_id = dup_scalar(map, "default", 128);

	if (!yaml_mapping_has_member(map, "devices"))
		return g_steal_pointer(&devices);
	node = yaml_mapping_get_member(map, "devices");
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SEQUENCE)
		return g_steal_pointer(&devices);
	seq = yaml_node_get_sequence(node);
	n = yaml_sequence_get_length(seq);

	for (i = 0; i < n && i < 64; i++) {
		YamlMapping *entry;
		BarTvremoteDevice *dev;
		BarTvremoteKind kind;
		g_autofree gchar *host = NULL;
		g_autofree gchar *id = NULL;
		const gchar *port;

		entry = yaml_sequence_get_mapping_element(seq, i);
		if (entry == NULL)
			continue;
		kind = bar_tvremote_kind_from_name(scalar_member(entry, "kind"));
		host = dup_scalar(entry, "host", 253);
		if (kind == BAR_TVREMOTE_KIND_UNKNOWN ||
		    !bar_tvremote_valid_host(host)) {
			g_message("tvremote: skipping device %u: needs a known "
			          "kind and a host", i + 1);
			continue;
		}
		id = dup_scalar(entry, "id", 64);
		if (id == NULL)
			id = bar_tvremote_device_make_id(devices, kind, host);
		if (bar_tvremote_device_find(devices, id) != NULL) {
			g_message("tvremote: duplicate device id `%s'", id);
			continue;
		}

		dev = bar_tvremote_device_new(id, kind, host);
		if (scalar_member(entry, "name") != NULL) {
			g_free(dev->name);
			dev->name = dup_scalar(entry, "name", 120);
			if (dev->name == NULL)
				dev->name = g_strdup(id);
		}
		port = scalar_member(entry, "port");
		if (port != NULL) {
			gint64 p = g_ascii_strtoll(port, NULL, 10);

			if (p > 0 && p < 65536)
				dev->port = (gint)p;
		}
		dev->token = dup_scalar(entry, "token", 256);
		dev->client_key = dup_scalar(entry, "client-key", 256);
		dev->volume_via = dup_scalar(entry, "volume-via", 64);
		g_ptr_array_add(devices, dev);
	}
	return g_steal_pointer(&devices);
}

/* Append @text as a double-quoted YAML scalar. */
static void
yaml_quote(GString *out, const gchar *text)
{
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = text; *p != '\0'; p++) {
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

static void
yaml_field(GString *out, const gchar *key, const gchar *value)
{
	if (value == NULL || value[0] == '\0')
		return;
	g_string_append_printf(out, "    %s: ", key);
	yaml_quote(out, value);
	g_string_append_c(out, '\n');
}

/**
 * bar_tvremote_config_to_yaml:
 * @devices: (element-type BarTvremoteDevice): the list
 * @default_id: (nullable): the default device
 *
 * Every value is double-quoted: a host name, a user's device name or a
 * token could otherwise contain a `:' or a `#' and change the file's
 * structure when it is read back.
 *
 * Returns: (transfer full): the file's contents
 */
gchar *
bar_tvremote_config_to_yaml(GPtrArray *devices, const gchar *default_id)
{
	GString *out;
	guint i;

	out = g_string_new("# gowl tvremote devices -- written by the bar's "
	                   "tvremote widget.\n# Holds pairing secrets: keep it "
	                   "mode 0600.\n");
	if (default_id != NULL && default_id[0] != '\0') {
		g_string_append(out, "default: ");
		yaml_quote(out, default_id);
		g_string_append_c(out, '\n');
	}
	if (devices == NULL || devices->len == 0) {
		g_string_append(out, "devices: []\n");
		return g_string_free(out, FALSE);
	}
	g_string_append(out, "devices:\n");
	for (i = 0; i < devices->len; i++) {
		const BarTvremoteDevice *d = g_ptr_array_index(devices, i);

		g_string_append(out, "  - id: ");
		yaml_quote(out, d->id);
		g_string_append_c(out, '\n');
		yaml_field(out, "name", d->name);
		yaml_field(out, "kind", bar_tvremote_kind_name(d->kind));
		yaml_field(out, "host", d->host);
		if (d->port > 0)
			g_string_append_printf(out, "    port: %d\n", d->port);
		yaml_field(out, "token", d->token);
		yaml_field(out, "client-key", d->client_key);
		yaml_field(out, "volume-via", d->volume_via);
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_tvremote_config_load:
 * @path: the file
 * @default_id: (out) (optional) (nullable): the default device id
 * @error: return location for a #GError
 *
 * A missing file is an empty list, not an error.  A file that group or
 * others can read is REFUSED (%BAR_TVREMOTE_CONFIG_ERROR_MODE) rather
 * than read: it holds pairing secrets, and reading it anyway would
 * teach the user that a world-readable token file works fine.
 *
 * Returns: (transfer full) (nullable) (element-type BarTvremoteDevice):
 *   the devices, or %NULL with @error set
 */
GPtrArray *
bar_tvremote_config_load(const gchar *path, gchar **default_id,
                         GError **error)
{
	GStatBuf st;
	g_autofree gchar *text = NULL;

	if (default_id != NULL)
		*default_id = NULL;
	if (g_stat(path, &st) != 0) {
		if (errno == ENOENT)
			return g_ptr_array_new_with_free_func(
				(GDestroyNotify)bar_tvremote_device_free);
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot read %s: %s", path, g_strerror(errno));
		return NULL;
	}
	if ((st.st_mode & 077) != 0) {
		g_set_error(error, BAR_TVREMOTE_CONFIG_ERROR,
		            BAR_TVREMOTE_CONFIG_ERROR_MODE,
		            "%s is readable by other users and holds pairing "
		            "keys; run chmod 600 on it", path);
		return NULL;
	}
	if (st.st_size > CONFIG_MAX_BYTES) {
		g_set_error(error, BAR_TVREMOTE_CONFIG_ERROR,
		            BAR_TVREMOTE_CONFIG_ERROR_SIZE,
		            "%s is too large to be a device list", path);
		return NULL;
	}
	if (!g_file_get_contents(path, &text, NULL, error))
		return NULL;
	return bar_tvremote_config_parse(text, default_id, error);
}

/*
 * Write @text to @path atomically at mode 0600.
 *
 * Not g_file_set_contents_full(): replacing an EXISTING file, it keeps
 * that file's mode, so a list someone once made 0644 would stay
 * world-readable with fresh secrets in it.  Here the temporary file is
 * created 0600 (mkstemp), fchmod'd to be sure, synced, and renamed over
 * the old one -- so the secrets are never readable by others, and a
 * crash leaves either the old file or the new one, never half of one.
 */
static gboolean
write_private(const gchar *path, const gchar *text, GError **error)
{
	g_autofree gchar *tmp = NULL;
	gsize len = strlen(text), done = 0;
	gint fd;

	tmp = g_strdup_printf("%s.XXXXXX", path);
	fd = g_mkstemp_full(tmp, O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot create %s: %s", tmp, g_strerror(errno));
		return FALSE;
	}
	if (fchmod(fd, 0600) != 0)
		goto fail;
	while (done < len) {
		gssize n = write(fd, text + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto fail;
		done += (gsize)n;
	}
	if (fsync(fd) != 0)
		goto fail;
	if (close(fd) != 0) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (g_rename(tmp, path) != 0)
		goto fail;
	return TRUE;

fail:
	g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
	            "cannot write %s: %s", path, g_strerror(errno));
	if (fd >= 0)
		close(fd);
	g_unlink(tmp);
	return FALSE;
}

/**
 * bar_tvremote_config_save:
 * @path: the file
 * @devices: (element-type BarTvremoteDevice): the list
 * @default_id: (nullable): the default device
 * @error: return location for a #GError
 *
 * Writes atomically at mode 0600 (see the file comment), creating the
 * directory at 0700 when it is missing.
 *
 * Returns: %TRUE on success
 */
gboolean
bar_tvremote_config_save(const gchar *path, GPtrArray *devices,
                         const gchar *default_id, GError **error)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *text = NULL;

	dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot create %s: %s", dir, g_strerror(errno));
		return FALSE;
	}
	text = bar_tvremote_config_to_yaml(devices, default_id);
	return write_private(path, text, error);
}
