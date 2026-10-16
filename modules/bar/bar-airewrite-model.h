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

#ifndef BAR_AIREWRITE_MODEL_H
#define BAR_AIREWRITE_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * The pure half of the `ai-rewrite' widget: the action list and its
 * two wire formats, prompt composition, the word diff, the paste-key
 * choice and the paste/restore state machine.
 *
 * Nothing in here touches a plugin, the theme, the compositor or a
 * subprocess, so every decision the widget makes can be tested against
 * the exact input that went wrong (tests/test-bar-ai-rewrite.c reaches
 * these through bar.so's exported symbols).
 */

/* Most actions one list may hold, and the longest prompt field taken.
   A pushed or configured list beyond these is cut, not refused: one
   absurd entry must not cost the user every other action. */
#define BAR_AIREWRITE_MAX_ACTIONS 36
#define BAR_AIREWRITE_MAX_FIELD   16384

/* Modifier bits, numerically identical to GowlKeyMod so the widget can
   hand them to the compositor unchanged (it asserts as much). */
#define BAR_AIREWRITE_MOD_SHIFT (1u << 0)
#define BAR_AIREWRITE_MOD_CTRL  (1u << 2)
#define BAR_AIREWRITE_MOD_ALT   (1u << 3)
#define BAR_AIREWRITE_MOD_LOGO  (1u << 6)

/**
 * BarAirewriteAction:
 * @key: the hotkey, `a'-`z' or `0'-`9', or 0 for none
 * @id: a stable identifier, unique within a list
 * @label: what the panel row says
 * @system: (nullable): the system prompt
 * @instruction: what to do with the text; `{input}' is replaced with
 *   what the user typed when @input is set
 * @input: (nullable): when set, the action asks for a line of text
 *   first and this is the field's placeholder (Custom, Ask)
 * @provider: (nullable): a provider for this action alone
 * @model: (nullable): a model for this action alone
 * @preview: %FALSE pastes the answer without showing it first
 * @diff: %TRUE shows the answer as a word diff against its input;
 *   %FALSE shows it as plain text (an answer, not a rewrite)
 *
 * One thing the widget can do to a piece of text.
 */
typedef struct {
	gchar     key;
	gchar    *id;
	gchar    *label;
	gchar    *system;
	gchar    *instruction;
	gchar    *input;
	gchar    *provider;
	gchar    *model;
	gboolean  preview;
	gboolean  diff;
} BarAirewriteAction;

BarAirewriteAction *bar_airewrite_action_new  (gchar        key,
                                               const gchar *id,
                                               const gchar *label,
                                               const gchar *system,
                                               const gchar *instruction);
BarAirewriteAction *bar_airewrite_action_copy (const BarAirewriteAction *self);
void                bar_airewrite_action_free (BarAirewriteAction *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BarAirewriteAction, bar_airewrite_action_free)

/**
 * bar_airewrite_key_usable:
 * @key: a candidate hotkey
 *
 * Whether @key can be an action's hotkey.  Lower-case letters and
 * digits only (Shift+letter means "and paste"), and never a key the
 * panel already means something by: `j' and `k' move the selection
 * (the bar host owns them), `y' copies and `v' pastes.
 *
 * Returns: %TRUE when @key can be bound
 */
gboolean bar_airewrite_key_usable (gchar key);

/**
 * bar_airewrite_builtin_actions:
 *
 * The standalone defaults: Fix spelling & grammar (f), Improve (i),
 * Shorten (s), Translate to English (e) and Custom (c).
 *
 * Returns: (transfer full) (element-type BarAirewriteAction): a new list
 */
GPtrArray *bar_airewrite_builtin_actions (void);

/* --- The pushed (line) format -------------------------------------- */

/**
 * bar_airewrite_escape:
 * @text: (nullable): a field
 *
 * Escapes a field for the pushed action format: backslash, tab,
 * newline and carriage return become `\\', `\t', `\n' and `\r'.
 * Nothing else is touched, so UTF-8 passes through as it is.
 *
 * Returns: (transfer full): the escaped field ("" for %NULL)
 */
gchar *bar_airewrite_escape   (const gchar *text);

/**
 * bar_airewrite_unescape:
 * @text: an escaped field
 *
 * The inverse of bar_airewrite_escape().  An unknown escape keeps its
 * character (`\q' is `q'); a trailing lone backslash is kept.
 *
 * Returns: (transfer full): the field
 */
gchar *bar_airewrite_unescape (const gchar *text);

/**
 * bar_airewrite_actions_serialize:
 * @actions: (element-type BarAirewriteAction): a list
 *
 * Writes @actions in the format the widget's `actions' setting takes:
 * a `#ai-rewrite-actions 1' header, then one action per line, nine
 * tab-separated escaped fields:
 *
 *   key  id  label  flags  input  provider  model  system  instruction
 *
 * flags holds `p' for preview and `d' for diff.  This is what the cmacs
 * side writes; it is here so the round trip can be tested in one place.
 *
 * Returns: (transfer full): the text
 */
gchar *bar_airewrite_actions_serialize (GPtrArray *actions);

/**
 * bar_airewrite_actions_parse:
 * @spec: (nullable): the value of the `actions' setting
 * @n_rejected: (out) (optional): how many lines were refused
 *
 * Reads the format bar_airewrite_actions_serialize() writes.  Blank
 * lines and `#' comments are skipped; a line without exactly nine
 * fields, without an id or a label, with text that is not UTF-8, or
 * with an id already seen is refused and counted.  A hotkey that is
 * not usable (see bar_airewrite_key_usable()) is dropped, keeping the
 * action.  Fields are cut at %BAR_AIREWRITE_MAX_FIELD bytes and the
 * list at %BAR_AIREWRITE_MAX_ACTIONS.
 *
 * Returns: (transfer full) (element-type BarAirewriteAction): the
 *   actions, possibly none
 */
GPtrArray *bar_airewrite_actions_parse (const gchar *spec,
                                        guint       *n_rejected);

/**
 * bar_airewrite_actions_from_yaml:
 * @yaml: the text of ~/.config/gowl/ai-rewrite.yaml
 * @replace_builtins: (out) (optional): the file's `replace-builtins'
 * @error: (out) (optional): why the file was refused
 *
 * Reads an `actions:' list of mappings with the fields of
 * #BarAirewriteAction (`key', `id', `label', `system', `instruction',
 * `input', `provider', `model', `preview', `diff').  An entry without
 * an id or a label is skipped, not fatal.  An empty file is an empty
 * list.
 *
 * Returns: (transfer full) (element-type BarAirewriteAction)
 *   (nullable): the actions, or %NULL with @error set when the file is
 *   not YAML or not a mapping
 */
GPtrArray *bar_airewrite_actions_from_yaml (const gchar  *yaml,
                                            gboolean     *replace_builtins,
                                            GError      **error);

/**
 * bar_airewrite_actions_merge:
 * @base: (element-type BarAirewriteAction): the list so far
 * @extra: (nullable) (element-type BarAirewriteAction): more actions
 * @replace: %TRUE to drop @base entirely
 *
 * Lays @extra over @base.  An extra action with the id of a base one
 * takes its place in the order; the rest are appended.  A hotkey is
 * held by the first action to claim it, extra actions winning over the
 * base ones: a base action whose key an extra one takes keeps its row
 * and loses only the key.
 *
 * Returns: (transfer full) (element-type BarAirewriteAction): a new
 *   list of copies
 */
GPtrArray *bar_airewrite_actions_merge (GPtrArray *base,
                                        GPtrArray *extra,
                                        gboolean   replace);

const BarAirewriteAction *bar_airewrite_actions_find_key (GPtrArray   *actions,
                                                          gchar        key);
const BarAirewriteAction *bar_airewrite_actions_find_id  (GPtrArray   *actions,
                                                          const gchar *id);

/**
 * bar_airewrite_compose_prompt:
 * @action: the action
 * @input: (nullable): what the user typed for an action that asks
 * @text: the text to work on
 *
 * The user message sent to the model: the instruction, `{input}'
 * replaced by @input (every occurrence), trimmed, then a blank line,
 * then @text exactly as it was selected:
 *
 *   <instruction>\n\n<text>
 *
 * An instruction that is empty after replacement sends @text alone.
 *
 * Returns: (transfer full): the prompt
 */
gchar *bar_airewrite_compose_prompt (const BarAirewriteAction *action,
                                     const gchar              *input,
                                     const gchar              *text);

/**
 * bar_airewrite_clean_answer:
 * @answer: what the model returned
 * @original: the text it was asked to rewrite
 *
 * Undoes the wrapping models add despite being told not to: a ``` fence
 * or a pair of quotes around the whole answer that @original did not
 * have, and leading or trailing blank space it did not have.  Trailing
 * whitespace @original ended with is put back, so pasting a rewritten
 * line keeps its newline.
 *
 * Returns: (transfer full): the answer to show and paste
 */
gchar *bar_airewrite_clean_answer (const gchar *answer,
                                   const gchar *original);

/**
 * bar_airewrite_preview:
 * @text: (nullable): the source text
 * @max_chars: the longest preview, in characters
 * @max_lines: the most lines shown
 *
 * A short preview of the source for the panel's header: cut at
 * @max_lines lines or @max_chars characters with an ellipsis, made
 * valid UTF-8 first.
 *
 * Returns: (transfer full): the preview
 */
gchar *bar_airewrite_preview (const gchar *text,
                              guint        max_chars,
                              guint        max_lines);

/* --- The word diff -------------------------------------------------- */

typedef enum {
	BAR_AIREWRITE_DIFF_EQUAL = 0,
	BAR_AIREWRITE_DIFF_DELETE,
	BAR_AIREWRITE_DIFF_INSERT
} BarAirewriteDiffKind;

typedef struct {
	BarAirewriteDiffKind  kind;
	gchar                *text;
} BarAirewriteDiffOp;

void bar_airewrite_diff_op_free (BarAirewriteDiffOp *op);

/**
 * bar_airewrite_tokenize:
 * @text: UTF-8 text
 *
 * Splits @text into a run of letters and digits (a word), a run of
 * white space, or one other character (punctuation, a symbol, an
 * emoji) per token.  Concatenating the tokens gives back @text exactly,
 * which is what lets the diff be painted as the new text with the old
 * words struck through in it.
 *
 * Returns: (transfer full) (element-type utf8): the tokens
 */
GPtrArray *bar_airewrite_tokenize (const gchar *text);

/**
 * bar_airewrite_diff:
 * @before: the text that went in
 * @after: the text that came out
 *
 * A word-level diff: a longest common subsequence over the tokens of
 * bar_airewrite_tokenize(), after the common head and tail are taken
 * off.  Within each changed stretch every deletion comes before every
 * insertion, and white space between two changes is folded into both
 * sides, so `the cat' -> `a dog' reads as one replacement rather than
 * two with a space between.  A middle too large to compare word by
 * word (more than a million cells) is reported as one replacement.
 *
 * The EQUAL and INSERT texts concatenate to @after; the EQUAL and
 * DELETE texts to @before.
 *
 * Returns: (transfer full) (element-type BarAirewriteDiffOp): the ops
 */
GPtrArray *bar_airewrite_diff (const gchar *before,
                               const gchar *after);

/**
 * bar_airewrite_diff_to_string:
 * @ops: (element-type BarAirewriteDiffOp): a diff
 *
 * Renders @ops as `[-deleted-]{+inserted+}' markup, for tests and the
 * log.
 *
 * Returns: (transfer full): the rendering
 */
gchar *bar_airewrite_diff_to_string (GPtrArray *ops);

/* --- Pasting -------------------------------------------------------- */

/**
 * bar_airewrite_parse_keys:
 * @spec: e.g. `ctrl+v' or `Ctrl+Shift+V'
 * @mods: (out): BAR_AIREWRITE_MOD_* bits
 * @key: (out) (transfer full): the key's name, lower-cased when it is
 *   one letter (`v'), as written otherwise (`Insert')
 *
 * Understands `ctrl'/`control', `shift', `alt'/`mod1' and
 * `super'/`logo'/`mod4', in any case and order.
 *
 * Returns: %FALSE when @spec names no key or an unknown modifier
 */
gboolean bar_airewrite_parse_keys (const gchar  *spec,
                                   guint        *mods,
                                   gchar       **key);

/**
 * bar_airewrite_is_terminal:
 * @app_id: (nullable): the window's app id (or X11 class)
 * @terminal_ids: (nullable): comma-separated app ids
 *
 * Returns: %TRUE when @app_id is one of @terminal_ids, ignoring case
 *   and the white space around each entry
 */
gboolean bar_airewrite_is_terminal (const gchar *app_id,
                                    const gchar *terminal_ids);

/**
 * bar_airewrite_paste_keys_for:
 * @app_id: (nullable): the target window's app id
 * @keys: the ordinary paste chord (`paste-keys')
 * @terminal_keys: the terminal one (`paste-keys-terminal')
 * @terminal_ids: which app ids are terminals (`terminal-app-ids')
 *
 * Terminals take Ctrl+V as a literal control character, so they get
 * their own chord.
 *
 * Returns: (transfer none): @terminal_keys or @keys
 */
const gchar *bar_airewrite_paste_keys_for (const gchar *app_id,
                                           const gchar *keys,
                                           const gchar *terminal_keys,
                                           const gchar *terminal_ids);

/**
 * BarAirewritePastePhase:
 * @BAR_AIREWRITE_PASTE_IDLE: nothing in progress
 * @BAR_AIREWRITE_PASTE_COPYING: a worker is saving the old clipboard
 *   and putting the result on it
 * @BAR_AIREWRITE_PASTE_COPIED: the worker is done; the compositor
 *   thread acts next
 * @BAR_AIREWRITE_PASTE_SETTLING: the paste keys went; waiting before
 *   the old clipboard goes back, so the application has read the new
 *   one first
 * @BAR_AIREWRITE_PASTE_RESTORING: a worker is putting it back
 */
typedef enum {
	BAR_AIREWRITE_PASTE_IDLE = 0,
	BAR_AIREWRITE_PASTE_COPYING,
	BAR_AIREWRITE_PASTE_COPIED,
	BAR_AIREWRITE_PASTE_SETTLING,
	BAR_AIREWRITE_PASTE_RESTORING
} BarAirewritePastePhase;

/**
 * BarAirewritePasteStep:
 * @BAR_AIREWRITE_PASTE_WAIT: nothing to do yet
 * @BAR_AIREWRITE_PASTE_INJECT: send the paste chord now
 * @BAR_AIREWRITE_PASTE_RESTORE: start the restore worker now
 * @BAR_AIREWRITE_PASTE_FINISH: done; the machine is idle again
 */
typedef enum {
	BAR_AIREWRITE_PASTE_WAIT = 0,
	BAR_AIREWRITE_PASTE_INJECT,
	BAR_AIREWRITE_PASTE_RESTORE,
	BAR_AIREWRITE_PASTE_FINISH
} BarAirewritePasteStep;

/**
 * BarAirewritePaste:
 * @phase: where it is
 * @inject: paste into the window, not only onto the clipboard
 * @restore: put the previous clipboard back afterwards
 * @copied: set by the copy worker: the clipboard now holds the result
 * @have_previous: set by the copy worker: it saved a previous text
 * @settled_at: when the paste chord went (monotonic, microseconds)
 * @restore_delay: how long to leave the result on the clipboard
 *
 * The paste-back sequence as data.  Workers move COPYING to COPIED and
 * RESTORING to IDLE; everything else is bar_airewrite_paste_step(),
 * called on the compositor thread.
 */
typedef struct {
	BarAirewritePastePhase phase;
	gboolean               inject;
	gboolean               restore;
	gboolean               copied;
	gboolean               have_previous;
	gint64                 settled_at;
	gint64                 restore_delay;
} BarAirewritePaste;

/**
 * bar_airewrite_paste_step:
 * @paste: the machine
 * @now: the monotonic time, in microseconds
 *
 * Advances @paste and says what the caller must do.  A copy that
 * failed, or a copy-only request, finishes without touching anything;
 * a restore happens only after keys went, only when asked for, only
 * when there was something to restore, and only once @restore_delay
 * has passed.
 *
 * Returns: the step to take
 */
BarAirewritePasteStep bar_airewrite_paste_step (BarAirewritePaste *paste,
                                                gint64             now);

/**
 * bar_airewrite_paste_wake_ms:
 * @paste: the machine
 * @now: the monotonic time, in microseconds
 *
 * Returns: milliseconds until bar_airewrite_paste_step() is worth
 *   calling again, or -1 when the machine is idle
 */
gint bar_airewrite_paste_wake_ms (const BarAirewritePaste *paste,
                                  gint64                   now);

G_END_DECLS

#endif /* BAR_AIREWRITE_MODEL_H */
