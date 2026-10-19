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
 * gowl-gabbrev-model.h - What a gabbrev configuration says, and the
 * loader that reads it.
 *
 * The format is espanso's, extended.  Anything espanso reads, this
 * reads the same way: match files (`matches:', `global_vars:',
 * `imports:'), espanso configuration directories (`config/default.yml',
 * app-specific `config/NAME.yml' with `filter_class' and friends, and
 * `match/' (recursively) with the `_'-prefix rule), the variable types and the
 * options.  On top of that (docs/gabbrev.org has the full list):
 *
 *   - options and matches may share one file, so a single section under
 *     `gabbrev:' in gowl's config.yaml is a whole configuration;
 *   - `filter:' -- gowl's filter language, on a match or a profile --
 *     next to espanso's regex `filter_*';
 *   - `profiles:' -- app-specific configurations written inline;
 *   - `files:' and `espanso:' -- more sources, from any source;
 *   - more variable types (env, uuid, selection, window, file, c,
 *     and whatever the embedder registers).
 *
 * Pure GLib and yaml-glib: no compositor, so the tests read real files.
 */

#ifndef GOWL_GABBREV_MODEL_H
#define GOWL_GABBREV_MODEL_H

#include <glib.h>
#include <yaml-glib.h>

#include "util/gowl-filter.h"

G_BEGIN_DECLS

/* Where a match came from when it was not a file. */
#define GABBREV_ORIGIN_BUILTIN "<builtin>"
#define GABBREV_ORIGIN_CONFIG  "<config>"
#define GABBREV_ORIGIN_CODE    "<code>"
#define GABBREV_ORIGIN_RUNTIME "<runtime>"

/* The deepest `imports:' chain followed before it is called a loop. */
#define GABBREV_MAX_IMPORT_DEPTH (16)

typedef enum {
	GABBREV_BACKEND_AUTO = 0,   /* keys when typeable and short, else paste */
	GABBREV_BACKEND_KEYS,       /* espanso's `inject' */
	GABBREV_BACKEND_CLIPBOARD
} GabbrevBackend;

typedef enum {
	GABBREV_CASE_NONE = 0,      /* propagate_case off */
	GABBREV_CASE_CAPITALIZE,    /* `Hello world' */
	GABBREV_CASE_CAPITALIZE_WORDS,
	GABBREV_CASE_UPPERCASE
} GabbrevCaseStyle;

/* Which options a GabbrevOptions sets: a profile overrides only these. */
typedef enum {
	GABBREV_OPT_ENABLE              = 1 << 0,
	GABBREV_OPT_BACKEND             = 1 << 1,
	GABBREV_OPT_CLIPBOARD_THRESHOLD = 1 << 2,
	GABBREV_OPT_KEY_DELAY           = 1 << 3,
	GABBREV_OPT_PASTE_SHORTCUT      = 1 << 4,
	GABBREV_OPT_PRESERVE_CLIPBOARD  = 1 << 5,
	GABBREV_OPT_RESTORE_DELAY       = 1 << 6,
	GABBREV_OPT_PRE_PASTE_DELAY     = 1 << 7,
	GABBREV_OPT_WORD_SEPARATORS     = 1 << 8,
	GABBREV_OPT_UNDO_BACKSPACE      = 1 << 9,
	GABBREV_OPT_BACKSPACE_LIMIT     = 1 << 10,
	GABBREV_OPT_REGEX_BUFFER        = 1 << 11,
	GABBREV_OPT_NOTIFICATIONS       = 1 << 12,
	GABBREV_OPT_TOGGLE_KEY          = 1 << 13,
	GABBREV_OPT_SEARCH_SHORTCUT     = 1 << 14,
	GABBREV_OPT_SEARCH_TRIGGER      = 1 << 15,
	GABBREV_OPT_APPLY_PATCH         = 1 << 16,
	GABBREV_OPT_POST_PASTE_DELAY    = 1 << 17,
	GABBREV_OPT_POST_PICKER_DELAY   = 1 << 18,
	GABBREV_OPT_REFOCUS             = 1 << 19,
	GABBREV_OPT_TIMEOUT             = 1 << 20
} GabbrevOptionBits;

typedef struct {
	guint           set;                  /* GabbrevOptionBits */
	gboolean        enable;
	GabbrevBackend  backend;
	guint           clipboard_threshold;  /* chars; auto pastes above it */
	guint           key_delay_ms;         /* between injected keys */
	gchar          *paste_shortcut;       /* "CTRL+V"; NULL: automatic */
	gboolean        preserve_clipboard;
	guint           restore_delay_ms;
	guint           pre_paste_delay_ms;
	guint           post_paste_delay_ms;  /* before keys typed after a paste */
	gchar          *word_separators;      /* UTF-8, one separator per char */
	gboolean        undo_backspace;
	guint           backspace_limit;      /* typos fixed inside a trigger */
	guint           regex_buffer;         /* chars a regex trigger sees */
	gboolean        notifications;
	gchar          *toggle_key;           /* "ALT" (double tap) or "Super+e" */
	gchar          *search_shortcut;      /* chord, or "off" */
	gchar          *search_trigger;       /* typed text, or NULL */
	gboolean        apply_patch;          /* terminals paste with Shift */
	guint           post_picker_delay_ms; /* after a picker or form closes */
	gboolean        refocus;              /* focus the target back before typing */
	guint           timeout_ms;           /* an expansion's whole budget */
} GabbrevOptions;

void     gabbrev_options_init_defaults (GabbrevOptions       *opts);
void     gabbrev_options_clear         (GabbrevOptions       *opts);
void     gabbrev_options_merge         (GabbrevOptions       *into,
                                        const GabbrevOptions *over);
void     gabbrev_options_copy          (GabbrevOptions       *dst,
                                        const GabbrevOptions *src);

/* One variable of a match, or a global one. */
typedef struct {
	gchar     *name;
	gchar     *type;
	YamlNode  *params;        /* a mapping, or NULL */
	gboolean   inject_vars;   /* substitute {{...}} into params (default) */
	GStrv      depends_on;
} GabbrevVar;

GabbrevVar *gabbrev_var_new  (const gchar *name,
                              const gchar *type,
                              YamlNode    *params);
void        gabbrev_var_free (GabbrevVar  *var);

/* One match: what is typed, and what it becomes. */
typedef struct {
	gint              ref_count;
	gchar            *id;            /* label, else first trigger, else regex */
	GPtrArray        *triggers;      /* gchar* */
	gchar            *regex_text;
	GRegex           *regex;         /* anchored at the end of the buffer */
	gchar            *replace;       /* NULL with image/html */
	gchar            *image_path;
	gchar            *html;
	gboolean          markdown;      /* replace came from `markdown:' */
	GPtrArray        *vars;          /* GabbrevVar* */
	gboolean          left_word;
	gboolean          right_word;
	gboolean          propagate_case;
	GabbrevCaseStyle  uppercase_style;
	gint              force_mode;    /* -1 unset, else a GabbrevBackend */
	gchar            *paste_shortcut;
	gchar            *label;
	GStrv             search_terms;
	gboolean          search_only;   /* passive_only: never typed */
	GowlFilter       *filter;        /* gowl filter, superset */
	gchar            *filter_text;
	GRegex           *filter_title;
	GRegex           *filter_class;
	GRegex           *filter_exec;
	gboolean          delete_first;  /* rub the trigger out before resolving */
	gboolean          notify;        /* a toast once it is typed */
	guint             timeout_ms;    /* 0: the profile's */
	gchar            *source;        /* file path or a GABBREV_ORIGIN_* */
	guint             order;         /* load order; later wins a tie */
} GabbrevMatch;

GabbrevMatch *gabbrev_match_new   (void);
GabbrevMatch *gabbrev_match_ref   (GabbrevMatch *match);
void          gabbrev_match_unref (GabbrevMatch *match);
gboolean      gabbrev_match_is_static (const GabbrevMatch *match);

/* An app-specific configuration: options that apply in some windows,
   and which match files are in force there. */
typedef struct {
	gchar          *name;
	GRegex         *filter_title;
	GRegex         *filter_class;
	GRegex         *filter_exec;
	gchar          *filter_os;
	GowlFilter     *filter;
	gchar          *filter_text;
	GabbrevOptions  options;
	GHashTable     *files;          /* source paths allowed; NULL = all */
} GabbrevProfile;

void gabbrev_profile_free (GabbrevProfile *profile);

/* Everything loaded, ready for the engine. */
typedef struct {
	gint            ref_count;
	GPtrArray      *matches;        /* GabbrevMatch*, load order */
	GPtrArray      *global_vars;    /* GabbrevVar* */
	GabbrevOptions  options;        /* the default profile's */
	GPtrArray      *profiles;       /* GabbrevProfile*, first match wins */
	GHashTable     *default_files;  /* files the default profile allows;
	                                   NULL = all */
	GPtrArray      *problems;       /* gchar*, one line each */
	GPtrArray      *files;          /* gchar*, every file read */
} GabbrevSet;

GabbrevSet   *gabbrev_set_ref            (GabbrevSet         *set);
void          gabbrev_set_unref          (GabbrevSet         *set);
GabbrevMatch *gabbrev_set_find_trigger   (GabbrevSet         *set,
                                          const gchar        *trigger);
GabbrevMatch *gabbrev_set_find_id        (GabbrevSet         *set,
                                          const gchar        *id);
const GabbrevVar *gabbrev_set_find_global (GabbrevSet        *set,
                                          const gchar        *name);

/* What the loader is told about the window, for profile matching. */
typedef struct {
	const gchar *app_id;     /* espanso's `class' */
	const gchar *title;
	const gchar *exec;       /* the program's path */
	GHashTable  *fields;     /* gowl filter fields, may be NULL */
} GabbrevWindow;

gboolean      gabbrev_regexes_match      (GRegex              *title,
                                          GRegex              *klass,
                                          GRegex              *exec,
                                          const GabbrevWindow *window);
const GabbrevProfile *gabbrev_set_profile_for (GabbrevSet          *set,
                                          const GabbrevWindow *window);
gboolean      gabbrev_set_match_allowed  (GabbrevSet           *set,
                                          const GabbrevProfile *profile,
                                          const GabbrevMatch   *match);

/* The loader. */
typedef struct _GabbrevLoader GabbrevLoader;

GabbrevLoader *gabbrev_loader_new        (void);
void           gabbrev_loader_free       (GabbrevLoader      *loader);
void           gabbrev_loader_add_builtin(GabbrevLoader      *loader);
void           gabbrev_loader_add_path   (GabbrevLoader      *loader,
                                          const gchar        *path,
                                          const gchar        *base_dir);
void           gabbrev_loader_add_node   (GabbrevLoader      *loader,
                                          YamlNode           *mapping,
                                          const gchar        *base_dir,
                                          const gchar        *origin);
gboolean       gabbrev_loader_add_text   (GabbrevLoader      *loader,
                                          const gchar        *yaml,
                                          const gchar        *base_dir,
                                          const gchar        *origin);
void           gabbrev_loader_problem    (GabbrevLoader      *loader,
                                          const gchar        *format,
                                          ...) G_GNUC_PRINTF(2, 3);
GabbrevSet    *gabbrev_loader_finish     (GabbrevLoader      *loader);

/* Shared helpers */
gchar         *gabbrev_expand_path       (const gchar        *path,
                                          const gchar        *base_dir);
gboolean       gabbrev_glob_match        (const gchar        *pattern,
                                          const gchar        *path);
gchar         *gabbrev_node_to_string    (YamlNode           *node);
gchar         *gabbrev_node_to_json      (YamlNode           *node);
const gchar   *gabbrev_builtin_yaml      (void);
const gchar * const *gabbrev_filter_fields (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevLoader, gabbrev_loader_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevSet, gabbrev_set_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevMatch, gabbrev_match_unref)

G_END_DECLS

#endif /* GOWL_GABBREV_MODEL_H */
