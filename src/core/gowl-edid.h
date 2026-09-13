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
 * SECTION:gowl-edid
 * @title: EDID
 * @short_description: what a display says about its own HDR range
 *
 * wlroots reports WHETHER an output can do BT.2020 and PQ, but not how
 * bright it gets.  Those numbers live in the display's EDID, in the
 * CTA-861 HDR Static Metadata Data Block, and nothing in wlroots
 * surfaces them -- so switching an output into HDR meant declaring a
 * mastering display invented out of nothing, conventionally 1000 cd/m².
 *
 * That is not a harmless guess.  The mastering luminance is metadata
 * the sink uses to decide whether to tone-map: tell a 700 cd/m² panel
 * that the content was mastered at 1000 and it will squeeze everything
 * down to fit a range the content never used, which dims the whole
 * desktop.  The panel's own numbers say "this already fits you", which
 * for a compositor that passes its composited output through unchanged
 * is both true and what you want.
 *
 * Small deliberately: enough to answer "how bright, how dark, and does
 * it do PQ", parsed from bytes so it can be tested without a display.
 */

#ifndef GOWL_EDID_H
#define GOWL_EDID_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GowlEdidHdr:
 * @has_static_metadata: the HDR Static Metadata Data Block was present
 * @supports_pq: the display accepts the SMPTE ST.2084 PQ EOTF (ET_2)
 * @supports_hlg: ... and Hybrid Log-Gamma (ET_3)
 * @supports_bt2020_rgb: the Colorimetry Data Block names BT.2020 RGB
 * @max_luminance: desired content max luminance, cd/m², 0 if unstated
 * @max_frame_average: desired max frame-average luminance, cd/m², 0 if unstated
 * @min_luminance: desired content min luminance, cd/m², 0 if unstated
 *
 * What one display said about its own HDR range.  Every luminance is
 * optional in the EDID -- a display may advertise PQ and say nothing
 * about how bright it is -- so 0 means "not stated" rather than zero
 * nits, and a caller must keep its own default for that case.
 */
typedef struct {
	gboolean has_static_metadata;
	gboolean supports_pq;
	gboolean supports_hlg;
	gboolean supports_bt2020_rgb;
	gdouble  max_luminance;
	gdouble  max_frame_average;
	gdouble  min_luminance;
} GowlEdidHdr;

/**
 * gowl_edid_parse_hdr:
 * @edid: (array length=len): the raw EDID, base block first
 * @len: its length in bytes
 * @out: (out caller-allocates): filled in; zeroed first
 *
 * Reads the CTA-861 Colorimetry and HDR Static Metadata data blocks out
 * of every CTA extension block in @edid.  Extension blocks of other
 * kinds (DisplayID, and anything else) are skipped, and a malformed
 * data block collection stops the walk for that block rather than
 * reading past it.
 *
 * Returns: %TRUE if @edid is a plausible EDID, whether or not it turned
 *          out to say anything about HDR
 */
gboolean gowl_edid_parse_hdr (const guint8 *edid,
                              gsize         len,
                              GowlEdidHdr  *out);

/**
 * gowl_edid_read_connector:
 * @connector: a DRM connector name, e.g. "eDP-1"
 * @out: (out caller-allocates): filled in on success
 *
 * Reads and parses the EDID the kernel has for @connector, from
 * /sys/class/drm/cardN-@connector/edid.
 *
 * This is the DRM backend's path and nothing else's: a nested or
 * headless output has no connector and gets %FALSE, which is the
 * caller's cue to keep its defaults.
 *
 * Returns: %TRUE if an EDID was read and parsed
 */
gboolean gowl_edid_read_connector (const gchar *connector,
                                   GowlEdidHdr *out);

G_END_DECLS

#endif /* GOWL_EDID_H */
