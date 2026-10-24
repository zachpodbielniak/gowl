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
 * gowl-hotkey-script.h - C for hotkeys, compiled by crispy.
 *
 * Two kinds of build:
 *
 *   - an ACTION, `{c: "code"}' or `{c: {file: PATH}}': a function
 *     `void gowl_hotkey_action(GowlHotkeyCall *call)'.  For inline code
 *     the function is written around the body, which sees `call',
 *     `comp' (the compositor), `focused' (the focused window or NULL),
 *     `args' and ARG(n).
 *   - a SCRIPT, `scripts: [hotkeys.c]': the .ahk of gowl.  It exports
 *     `gboolean gowl_hotkey_script_init(void)', which registers what it
 *     likes through <gowl/gowl.h> (gowl_hotkey_define(), _register_func(),
 *     _register_context(), _add_yaml()), and may export
 *     `void gowl_hotkey_script_fini(void)'.
 *
 * Building runs on a worker thread (gcc takes a while); opening and
 * every call run on the compositor thread under the fault guard, and a
 * build is never unmapped (code may still be running in it).
 */

#ifndef GOWL_HOTKEY_SCRIPT_H
#define GOWL_HOTKEY_SCRIPT_H

#include <glib.h>
#include <gmodule.h>

#include "hotkey/gowl-hotkey.h"

G_BEGIN_DECLS

#define HK_C_ACTION_SYMBOL  "gowl_hotkey_action"
#define HK_C_INIT_SYMBOL    "gowl_hotkey_script_init"
#define HK_C_FINI_SYMBOL    "gowl_hotkey_script_fini"

typedef void     (*HkCActionFunc) (GowlHotkeyCall *call);
typedef gboolean (*HkCInitFunc)   (void);
typedef void     (*HkCFiniFunc)   (void);

typedef struct {
	gchar   *code;       /* an action body; or NULL */
	gchar   *file;       /* a source file; or NULL */
	gchar   *cflags;     /* extra flags; `$(pkg-config ...)' expands */
} HkCSpec;

void      hk_c_spec_clear    (HkCSpec        *spec);
gchar    *hk_c_spec_key      (const HkCSpec  *spec);
gchar    *hk_c_full_source   (const HkCSpec  *spec,
                              GError        **error);
gchar    *hk_c_build         (const HkCSpec  *spec,
                              const gchar    *cache_dir,
                              GError        **error);
GModule  *hk_c_open          (const gchar    *so_path,
                              GError        **error);
gboolean  hk_c_call_action   (HkCActionFunc   func,
                              GowlHotkeyCall *call,
                              guint           timeout_ms,
                              GError        **error);
gboolean  hk_c_call_init     (HkCInitFunc     func,
                              guint           timeout_ms,
                              GError        **error);
gboolean  hk_c_call_fini     (HkCFiniFunc     func,
                              guint           timeout_ms,
                              GError        **error);

G_END_DECLS

#endif /* GOWL_HOTKEY_SCRIPT_H */
