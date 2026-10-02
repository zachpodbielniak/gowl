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
 * gowl-macro-record.h - Turn a recording of real input into a macro.
 *
 * Pure GLib and json-glib: it reads the input recorder's payload (the
 * JSON `gowl_input_recorder_drain()' returns) and writes the C source
 * of a timeline macro that replays it.  Nothing here touches the
 * compositor, so the tests drive it with hand-written payloads.
 */

#ifndef GOWL_MACRO_RECORD_H
#define GOWL_MACRO_RECORD_H

#include <glib.h>

G_BEGIN_DECLS

#define GOWL_MACRO_RECORD_ERROR (gowl_macro_record_error_quark())

/**
 * GowlMacroRecordError:
 * @GOWL_MACRO_RECORD_ERROR_PARSE: the payload is not the recorder's JSON
 * @GOWL_MACRO_RECORD_ERROR_EMPTY: nothing worth replaying was recorded
 *
 * Why a recording could not be turned into a macro.
 */
typedef enum {
	GOWL_MACRO_RECORD_ERROR_PARSE,
	GOWL_MACRO_RECORD_ERROR_EMPTY
} GowlMacroRecordError;

/**
 * GowlMacroRecordStats:
 * @steps: steps written (keys, clicks, moves, scrolls; not waits)
 * @duration_ms: how long the replay takes at normal speed
 * @dropped: events left out -- a release whose press came before the
 *   recording, a press never released (the stop key), a lone modifier
 * @suppressed: events the recorder itself withheld (lock screen,
 *   password prompts), from the payload
 *
 * What gowl_macro_record_to_source() made of a recording.
 */
typedef struct {
	guint  steps;
	guint  duration_ms;
	guint  dropped;
	guint  suppressed;
} GowlMacroRecordStats;

GQuark  gowl_macro_record_error_quark  (void);
gchar  *gowl_macro_record_to_source    (const gchar          *payload,
                                        const gchar          *name,
                                        guint                 max_gap_ms,
                                        GowlMacroRecordStats *stats,
                                        GError              **error);

G_END_DECLS

#endif /* GOWL_MACRO_RECORD_H */
