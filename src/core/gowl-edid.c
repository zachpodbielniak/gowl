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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-edid"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "gowl-edid.h"

#define EDID_BLOCK        (128)
#define EDID_EXT_COUNT    (126)

/* CTA-861 extension block */
#define CTA_TAG           (0x02)
#define CTA_DTD_OFFSET    (2)
#define CTA_COLLECTION    (4)

/* Data block tag codes (top three bits of the header byte) */
#define CTA_TAG_EXTENDED  (7)
/* Extended tag codes */
#define CTA_EXT_COLORIMETRY   (0x05)
#define CTA_EXT_HDR_STATIC    (0x06)

/* Electro-Optical Transfer Function bits in the HDR block */
#define ET_ST2084_PQ      (1u << 2)
#define ET_HLG            (1u << 3)

/* Colorimetry byte 1, bit 7 */
#define COLORIMETRY_BT2020_RGB (1u << 7)

static const guint8 edid_magic[8] = {
	0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

/*
 * CTA-861 luminance coding.
 *
 * Max and max-frame-average share one curve; the minimum is expressed
 * as a fraction of the maximum, which is why it is decoded after it and
 * is meaningless without it.
 */
static gdouble
decode_max_luminance(guint8 cv)
{
	return 50.0 * pow(2.0, (gdouble)cv / 32.0);
}

static gdouble
decode_min_luminance(guint8 cv, gdouble max)
{
	gdouble f = (gdouble)cv / 255.0;

	return max * f * f / 100.0;
}

/*
 * One CTA extension block's data block collection.
 *
 * Bytes 4 .. d-1, where d is the offset the block itself gives for the
 * start of its detailed timing descriptors.  A d outside that range, or
 * a data block that would run past it, means the block is malformed:
 * the walk stops rather than reading whatever follows.
 */
static void
parse_cta_block(const guint8 *blk, GowlEdidHdr *out)
{
	guint d;
	guint i;

	if (blk[0] != CTA_TAG)
		return;

	d = blk[CTA_DTD_OFFSET];
	if (d < CTA_COLLECTION || d > EDID_BLOCK - 1)
		return;

	i = CTA_COLLECTION;
	while (i < d) {
		guint tag = (guint)(blk[i] >> 5) & 0x07u;
		guint len = (guint)blk[i] & 0x1Fu;
		const guint8 *body = &blk[i + 1];

		/* `len' counts the bytes after the header byte, and for an
		 * extended block the first of those is the extended tag. */
		if (i + 1 + len > d)
			return;

		if (tag == CTA_TAG_EXTENDED && len >= 1) {
			guint ext = body[0];

			if (ext == CTA_EXT_COLORIMETRY && len >= 2) {
				out->supports_bt2020_rgb =
					(body[1] & COLORIMETRY_BT2020_RGB) != 0;
			} else if (ext == CTA_EXT_HDR_STATIC && len >= 2) {
				out->has_static_metadata = TRUE;
				out->supports_pq  = (body[1] & ET_ST2084_PQ) != 0;
				out->supports_hlg = (body[1] & ET_HLG) != 0;
				/* body[2] is the static metadata descriptor
				 * support byte; the luminances follow it, and
				 * each one is optional. */
				if (len >= 4)
					out->max_luminance =
						decode_max_luminance(body[3]);
				if (len >= 5)
					out->max_frame_average =
						decode_max_luminance(body[4]);
				if (len >= 6 && out->max_luminance > 0.0)
					out->min_luminance = decode_min_luminance(
						body[5], out->max_luminance);
			}
		}
		i += 1 + len;
	}
}

gboolean
gowl_edid_parse_hdr(const guint8 *edid, gsize len, GowlEdidHdr *out)
{
	gsize blocks;
	gsize i;

	g_return_val_if_fail(out != NULL, FALSE);

	memset(out, 0, sizeof *out);

	if (edid == NULL || len < EDID_BLOCK)
		return FALSE;
	if (memcmp(edid, edid_magic, sizeof edid_magic) != 0)
		return FALSE;

	/* Trust what is actually there over the declared extension count:
	 * a truncated read is more likely than a lying header, and walking
	 * past the buffer on the strength of one byte is how a display
	 * becomes a crash. */
	blocks = len / EDID_BLOCK;
	for (i = 1; i < blocks; i++)
		parse_cta_block(edid + i * EDID_BLOCK, out);

	return TRUE;
}

gboolean
gowl_edid_read_connector(const gchar *connector, GowlEdidHdr *out)
{
	static const gchar *drm_dir = "/sys/class/drm";
	GDir *dir;
	const gchar *name;
	gboolean parsed = FALSE;

	g_return_val_if_fail(out != NULL, FALSE);

	memset(out, 0, sizeof *out);
	if (connector == NULL || connector[0] == '\0')
		return FALSE;

	dir = g_dir_open(drm_dir, 0, NULL);
	if (dir == NULL)
		return FALSE;

	while (!parsed && (name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *data = NULL;
		const gchar *dash;
		gsize size = 0;

		/* Entries are "card<N>-<connector>"; compare what follows the
		 * first dash rather than a suffix, so "DP-1" cannot match
		 * "card0-eDP-1". */
		if (!g_str_has_prefix(name, "card"))
			continue;
		dash = strchr(name, '-');
		if (dash == NULL || g_strcmp0(dash + 1, connector) != 0)
			continue;

		path = g_build_filename(drm_dir, name, "edid", NULL);
		/* sysfs reports a size of 0 for these, so the contents have
		 * to be read to find out whether there are any: a
		 * disconnected output has an empty one. */
		if (!g_file_get_contents(path, &data, &size, NULL))
			continue;
		if (size == 0)
			continue;

		parsed = gowl_edid_parse_hdr((const guint8 *)data, size, out);
		if (!parsed)
			g_debug("%s: the kernel's EDID did not parse", connector);
	}

	g_dir_close(dir);
	return parsed;
}
