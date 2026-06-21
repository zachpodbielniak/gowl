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
 * gowl-wlroots-compat.h -- single source of truth for the wlroots
 * version gowl was compiled against.
 *
 * gowl supports wlroots 0.19 and 0.20, which ship as parallel,
 * ABI-incompatible libraries.  A build links exactly one, selected in
 * config.mk (newest present by default, override with `make WLROOTS=`).
 * config.mk passes the choice in as -DGOWL_WLROOTS_VERSION_MAJOR / MINOR;
 * this header derives the feature flags the rest of the tree branches on,
 * so no other file hard-codes a version number.
 *
 * Capability flags (use these, not raw version comparisons):
 *
 *   GOWL_HAVE_WLROOTS_0_20
 *       1 when wlroots >= 0.20.  Gates the ext-image-copy-capture +
 *       ext-foreign-toplevel-list stack, which is what enables
 *       per-window screencast capture (xdg-desktop-portal-wlr window
 *       sharing).  0.19 has only wlr-screencopy (monitor capture).
 *
 *   GOWL_HAVE_CAPTURE_WINDOW
 *       1 when the compiled wlroots can capture individual toplevels
 *       (currently == GOWL_HAVE_WLROOTS_0_20).  A separate name so call
 *       sites read by intent ("can we window-capture?") rather than by
 *       version, and so a future wlroots that gains/loses the capability
 *       is a one-line change here.
 */

#ifndef GOWL_WLROOTS_COMPAT_H
#define GOWL_WLROOTS_COMPAT_H

/* config.mk always defines these; fall back to 0.19 if a stray
 * translation unit is built without them so we degrade to the
 * lowest-common-denominator feature set rather than miscompile. */
#ifndef GOWL_WLROOTS_VERSION_MAJOR
#define GOWL_WLROOTS_VERSION_MAJOR 0
#endif
#ifndef GOWL_WLROOTS_VERSION_MINOR
#define GOWL_WLROOTS_VERSION_MINOR 19
#endif

/* Encode (major, minor) as a single comparable integer, e.g. 0.20 -> 20,
 * 1.0 -> 100, so checks keep working if wlroots ever reaches 1.x. */
#define GOWL_WLROOTS_VERSION \
	((GOWL_WLROOTS_VERSION_MAJOR) * 100 + (GOWL_WLROOTS_VERSION_MINOR))

#if GOWL_WLROOTS_VERSION >= 20
#  define GOWL_HAVE_WLROOTS_0_20 1
#else
#  define GOWL_HAVE_WLROOTS_0_20 0
#endif

/* Per-window screencast capture capability.  Tracks 0.20 today. */
#define GOWL_HAVE_CAPTURE_WINDOW GOWL_HAVE_WLROOTS_0_20

#endif /* GOWL_WLROOTS_COMPAT_H */
