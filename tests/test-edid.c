/* test-edid.c -- what a display says about its own HDR range
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Switching an output into HDR means declaring a mastering display, and
 * gowl used to declare one it made up: 1000 cd/m² peak, because that is
 * what most HDR content is graded at.  Tell a 700 cd/m² panel that and
 * it tone-maps content that already fitted, which only dims it.  The
 * numbers are in the display's EDID and wlroots does not surface them,
 * so gowl parses them.
 *
 * The fixture is a real panel: the CSOT MND508ZB1-1 in the laptop this
 * was reported from, read from /sys/class/drm/card0-eDP-1/edid.  It is
 * a useful one because it is an HDR panel whose peak is nowhere near
 * the number that used to be assumed -- the exact case the guess got
 * wrong.
 *
 * Also here: the shapes that must not crash.  An EDID arrives from a
 * cable, so a parser for one is parsing hostile input by construction.
 */

#include <glib.h>
#include <string.h>

#include "core/gowl-edid.h"

/* The real thing, 384 bytes: base block, a CTA-861 extension, and a
 * DisplayID extension that must be skipped rather than misread. */
static const guint8 panel_edid[] = {
	0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x0E, 0x77, 0x22, 0x13,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x01, 0x04, 0xB5, 0x1C, 0x13, 0x78,
	0x03, 0xCC, 0x85, 0xA4, 0x55, 0x4C, 0x9C, 0x24, 0x0D, 0x50, 0x54, 0x00,
	0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xDA, 0x90, 0x40, 0xA0, 0xB0, 0x80,
	0x71, 0x70, 0x30, 0x20, 0x66, 0x00, 0x1D, 0xBE, 0x10, 0x00, 0x00, 0x18,
	0x00, 0x00, 0x00, 0xFD, 0x00, 0x1E, 0x78, 0xF4, 0xF4, 0x4B, 0x01, 0x0A,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x43,
	0x53, 0x4F, 0x54, 0x20, 0x54, 0x33, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0xFC, 0x00, 0x4D, 0x4E, 0x44, 0x35, 0x30, 0x38, 0x5A,
	0x42, 0x31, 0x2D, 0x31, 0x0A, 0x20, 0x02, 0x9C, 0x02, 0x03, 0x24, 0x00,
	0xE3, 0x05, 0x80, 0x00, 0xE6, 0x06, 0x05, 0x01, 0x7A, 0x7A, 0x52, 0x74,
	0x1A, 0x00, 0x00, 0x03, 0x53, 0x1E, 0x78, 0x00, 0x20, 0x7A, 0xFF, 0x7A,
	0xFF, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x33, 0x70, 0x20, 0x79, 0x02, 0x00, 0x20, 0x00, 0x0C,
	0x40, 0x8B, 0xF6, 0x22, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x00,
	0x21, 0x00, 0x1D, 0x21, 0x0B, 0x6C, 0x07, 0x40, 0x0B, 0x80, 0x07, 0x80,
	0x4E, 0x0A, 0x55, 0xCD, 0xE4, 0x9B, 0x4A, 0x12, 0x0D, 0x02, 0x45, 0x54,
	0x78, 0x61, 0x78, 0x61, 0x00, 0x47, 0x13, 0x78, 0x22, 0x00, 0x14, 0x06,
	0x51, 0x0B, 0x88, 0x3F, 0x0B, 0x9F, 0x00, 0x2F, 0x00, 0x1F, 0x00, 0x7F,
	0x07, 0x70, 0x00, 0x05, 0x00, 0x05, 0x00, 0x26, 0x00, 0x09, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x2B, 0x00, 0x0C, 0x27, 0x00,
	0x1E, 0x77, 0x00, 0x00, 0x27, 0x00, 0x1E, 0x3B, 0x00, 0x00, 0x2E, 0x00,
	0x06, 0x00, 0x47, 0x78, 0x61, 0x78, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x90,
};

/* A CTA extension carrying only what this parser looks for, so a
 * failure points at the block walk rather than at 380 bytes of
 * unrelated timings. */
static void
build_minimal(guint8 *edid, const guint8 *blocks, gsize n_blocks)
{
	static const guint8 magic[8] = {
		0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
	};

	memset(edid, 0, 256);
	memcpy(edid, magic, sizeof magic);
	edid[126] = 1;                  /* one extension block */
	edid[128] = 0x02;               /* CTA-861 */
	edid[129] = 3;                  /* revision */
	edid[130] = (guint8)(4 + n_blocks); /* where the DTDs start */
	memcpy(edid + 132, blocks, n_blocks);
}

/* ── Cases ───────────────────────────────────────────────────────── */

/* The panel's own numbers, decoded from its own bytes. */
static void
real_panel(void)
{
	GowlEdidHdr hdr;

	g_assert_true(gowl_edid_parse_hdr(panel_edid, sizeof panel_edid,
	                                  &hdr));

	g_assert_true(hdr.has_static_metadata);
	g_assert_true(hdr.supports_pq);
	g_assert_false(hdr.supports_hlg);
	g_assert_true(hdr.supports_bt2020_rgb);

	/* 50 * 2^(0x7A/32) = 702.5, and the minimum is a fraction of it:
	 * 702.5 * (0x52/255)^2 / 100 = 0.726. */
	g_assert_cmpfloat(hdr.max_luminance, >, 702.0);
	g_assert_cmpfloat(hdr.max_luminance, <, 703.0);
	g_assert_cmpfloat(hdr.max_frame_average, >, 702.0);
	g_assert_cmpfloat(hdr.max_frame_average, <, 703.0);
	g_assert_cmpfloat(hdr.min_luminance, >, 0.72);
	g_assert_cmpfloat(hdr.min_luminance, <, 0.73);

	/* The number that used to be assumed is nowhere near it, which is
	 * the whole point of reading rather than guessing. */
	g_assert_cmpfloat(hdr.max_luminance, <, 1000.0);
}

/* Every luminance is optional.  A display may say it does PQ and stop
 * there, and 0 has to read as "did not say" rather than as zero nits --
 * a caller that took it literally would declare a display that emits no
 * light at all. */
static void
luminances_are_optional(void)
{
	guint8 edid[256];
	GowlEdidHdr hdr;
	/* Extended tag 0x06, length 2: the EOTF byte and nothing else. */
	static const guint8 blocks[] = { 0xE2, 0x06, 0x04 };

	build_minimal(edid, blocks, sizeof blocks);
	g_assert_true(gowl_edid_parse_hdr(edid, sizeof edid, &hdr));

	g_assert_true(hdr.has_static_metadata);
	g_assert_true(hdr.supports_pq);
	g_assert_cmpfloat(hdr.max_luminance, ==, 0.0);
	g_assert_cmpfloat(hdr.min_luminance, ==, 0.0);
	g_assert_cmpfloat(hdr.max_frame_average, ==, 0.0);
}

/* An SDR panel: no HDR block at all. */
static void
no_hdr_block(void)
{
	guint8 edid[256];
	GowlEdidHdr hdr;
	/* A colorimetry block that does NOT claim BT.2020. */
	static const guint8 blocks[] = { 0xE3, 0x05, 0x00, 0x00 };

	build_minimal(edid, blocks, sizeof blocks);
	g_assert_true(gowl_edid_parse_hdr(edid, sizeof edid, &hdr));

	g_assert_false(hdr.has_static_metadata);
	g_assert_false(hdr.supports_pq);
	g_assert_false(hdr.supports_bt2020_rgb);
	g_assert_cmpfloat(hdr.max_luminance, ==, 0.0);
}

/*
 * Rubbish must not be read past.
 *
 * An EDID comes off a cable, so the length in a data block header is
 * whatever the other end felt like sending.  A block claiming to run
 * past the end of its own collection has to stop the walk, not be
 * trusted.
 */
static void
malformed_is_refused(void)
{
	GowlEdidHdr hdr;
	guint8 edid[256];
	static const guint8 runaway[] = { 0xFF, 0x06, 0x05, 0x01, 0x7A };

	/* Too short to be an EDID. */
	g_assert_false(gowl_edid_parse_hdr(panel_edid, 8, &hdr));
	/* Not an EDID at all. */
	{
		guint8 junk[256];

		memset(junk, 0xAB, sizeof junk);
		g_assert_false(gowl_edid_parse_hdr(junk, sizeof junk, &hdr));
	}
	/* NULL, and a zero length. */
	g_assert_false(gowl_edid_parse_hdr(NULL, 0, &hdr));

	/* A header whose length runs past the data block collection: the
	 * EDID parses, and the runaway block contributes nothing. */
	build_minimal(edid, runaway, sizeof runaway);
	g_assert_true(gowl_edid_parse_hdr(edid, sizeof edid, &hdr));
	g_assert_false(hdr.has_static_metadata);

	/* A declared extension count larger than the buffer: what is
	 * actually there decides, or one byte from the cable walks off the
	 * end of the allocation. */
	build_minimal(edid, runaway, sizeof runaway);
	edid[126] = 200;
	g_assert_true(gowl_edid_parse_hdr(edid, 256, &hdr));
}

/* A connector that does not exist reads as "no EDID", which is what a
 * nested or headless output is, and leaves the output zeroed. */
static void
absent_connector(void)
{
	GowlEdidHdr hdr;

	memset(&hdr, 0xFF, sizeof hdr);
	g_assert_false(gowl_edid_read_connector("NOPE-99", &hdr));
	g_assert_false(hdr.has_static_metadata);
	g_assert_cmpfloat(hdr.max_luminance, ==, 0.0);

	g_assert_false(gowl_edid_read_connector(NULL, &hdr));
	g_assert_false(gowl_edid_read_connector("", &hdr));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/edid/real-panel", real_panel);
	g_test_add_func("/edid/luminances-are-optional", luminances_are_optional);
	g_test_add_func("/edid/no-hdr-block", no_hdr_block);
	g_test_add_func("/edid/malformed-is-refused", malformed_is_refused);
	g_test_add_func("/edid/absent-connector", absent_connector);
	return g_test_run();
}
