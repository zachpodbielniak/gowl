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

#ifndef BAR_AI_H
#define BAR_AI_H

#include <glib.h>

#include "barkit/gowl-bar-plugin.h"

G_BEGIN_DECLS

/**
 * BarAiBackend:
 * @BAR_AI_BACKEND_NONE: no way to reach a model; the widget says so
 * @BAR_AI_BACKEND_CMACS: ask the cmacs this bar runs inside, through
 *   `emacsctl eval'; cmacs streams the answer back as widget settings.
 *   This is what `cmacs --gowl' selects, so every provider configured
 *   in cmacs-ai is available and nothing is configured twice.
 * @BAR_AI_BACKEND_CLI: run ai-glib's own `ai' command on a worker
 *   thread and read its output.  The standalone-gowl route: providers
 *   and keys come from ~/.config/ai-glib/config.yaml.
 *
 * Where a widget's AI request goes.  Chosen per widget by the
 * `ai-backend' setting: `cmacs', `ai', `none', or `auto' (the default:
 * the `ai' command when it is installed, otherwise none).
 *
 * Both routes are out of process on purpose.  cmacs already links its
 * own ai-glib statically, and the bar module is loaded INTO cmacs under
 * `--gowl': linking a second copy here would bind against whichever
 * one the loader found first.
 */
typedef enum {
	BAR_AI_BACKEND_NONE = 0,
	BAR_AI_BACKEND_CMACS,
	BAR_AI_BACKEND_CLI
} BarAiBackend;

/**
 * BarAiState:
 * @BAR_AI_IDLE: nothing asked yet
 * @BAR_AI_WAITING: asked, nothing back yet
 * @BAR_AI_STREAMING: some of the answer is in
 * @BAR_AI_DONE: the whole answer is in
 * @BAR_AI_ERROR: it failed; the error text says why
 */
typedef enum {
	BAR_AI_IDLE = 0,
	BAR_AI_WAITING,
	BAR_AI_STREAMING,
	BAR_AI_DONE,
	BAR_AI_ERROR
} BarAiState;

/**
 * BarAi:
 *
 * One widget's AI conversation slot: at most one request in flight, and
 * the text of the latest answer.  Thread-safe: the CLI route fills it
 * from a worker, the cmacs route from the compositor thread, and the
 * widget reads it from either.
 */
typedef struct _BarAi BarAi;

/**
 * bar_ai_new:
 * @widget: the widget's registry name, e.g. "ai-rewrite"; cmacs
 *   addresses its replies to `<widget>.ai-*' settings
 *
 * Returns: (transfer full): a new idle slot
 */
BarAi       *bar_ai_new        (const gchar *widget);
void         bar_ai_free       (BarAi       *self);

/**
 * bar_ai_backend_for:
 * @plugin: the widget
 *
 * Returns: the backend its `ai-backend' setting resolves to
 */
BarAiBackend bar_ai_backend_for (GowlBarPlugin *plugin);

/**
 * bar_ai_parse_backend:
 * @spec: (nullable): an `ai-backend' value
 * @have_cli: whether the `ai' command is installed
 *
 * The decision bar_ai_backend_for() makes, with the environment passed
 * in so it can be tested.
 *
 * Returns: the backend
 */
BarAiBackend bar_ai_parse_backend (const gchar *spec, gboolean have_cli);

/**
 * bar_ai_start:
 * @self: the slot
 * @plugin: the widget asking; its `ai-provider', `ai-model',
 *   `ai-command', `emacsctl' and `ai-timeout' settings apply
 * @system: (nullable): a system prompt
 * @prompt: the user's text
 * @error: (out) (optional): why nothing was sent
 *
 * Starts a request, abandoning any still running (its child, on the CLI
 * route, is sent SIGTERM; its late answer, on either route, is
 * ignored).  Never blocks: the work happens on a worker or in cmacs.
 *
 * Returns: %TRUE when the request is on its way
 */
gboolean     bar_ai_start      (BarAi         *self,
                                 GowlBarPlugin *plugin,
                                 const gchar   *system,
                                 const gchar   *prompt,
                                 GError       **error);

/**
 * bar_ai_start_full:
 * @self: the slot
 * @plugin: the widget asking
 * @system: (nullable): a system prompt
 * @prompt: the user's text
 * @provider: (nullable): a provider for THIS request, or %NULL / ""
 *   for the widget's `ai-provider' setting
 * @model: (nullable): likewise, a model for this request
 * @error: (out) (optional): why nothing was sent
 *
 * bar_ai_start() with a per-request provider and model, for widgets
 * whose actions each name their own (an ai-rewrite action that wants a
 * small local model for "fix spelling" and a large one for "improve").
 *
 * Returns: %TRUE when the request is on its way
 */
gboolean     bar_ai_start_full (BarAi         *self,
                                 GowlBarPlugin *plugin,
                                 const gchar   *system,
                                 const gchar   *prompt,
                                 const gchar   *provider,
                                 const gchar   *model,
                                 GError       **error);

/**
 * bar_ai_cancel:
 * @self: the slot
 *
 * Abandons the request in flight, if any, and returns to idle.
 */
void         bar_ai_cancel     (BarAi *self);

/**
 * bar_ai_absorb_settings:
 * @self: the slot
 * @plugin: the widget
 *
 * The cmacs route's half of the conversation: cmacs pushes
 * `<widget>.ai-id', `.ai-state', `.ai-text' and `.ai-error' into the
 * widget's settings, and this copies them into the slot when the id is
 * the one this slot is waiting for.  Call it from the widget's
 * configure hook, which the bar runs on every push.
 *
 * Returns: %TRUE when the slot changed
 */
gboolean     bar_ai_absorb_settings (BarAi         *self,
                                      GowlBarPlugin *plugin);

/**
 * bar_ai_get:
 * @self: the slot
 * @text: (out) (optional) (transfer full): the answer so far
 * @error: (out) (optional) (transfer full): the error, if any
 *
 * Returns: the state
 */
BarAiState   bar_ai_get        (BarAi  *self,
                                 gchar **text,
                                 gchar **error);

/**
 * bar_ai_is_busy:
 * @self: the slot
 *
 * Returns: %TRUE while waiting or streaming
 */
gboolean     bar_ai_is_busy    (BarAi *self);

const gchar *bar_ai_state_name (BarAiState state);

/**
 * bar_ai_get_request_id:
 * @self: the slot
 *
 * The id cmacs must echo back in `<widget>.ai-id' for its answer to be
 * taken.  Exposed for diagnostics and tests.
 *
 * Returns: (transfer full) (nullable): the current request id
 */
gchar       *bar_ai_get_request_id (BarAi *self);

/* --- Pure helpers, exposed for the tests -------------------------- */

/**
 * bar_ai_cli_argv:
 * @command: the `ai' executable
 * @provider: (nullable): a provider name; empty or %NULL for ai's saved
 *   default
 * @model: (nullable): a model id
 * @system: (nullable): a system prompt
 *
 * The argv for one CLI request.  The prompt travels on stdin, never in
 * argv: it can be long, and `ps' should not show it.  `--no-expand'
 * stops ai treating an `@' or a leading `/' in the user's text as a
 * mention or a command.
 *
 * Returns: (transfer full): a NULL-terminated argv
 */
gchar      **bar_ai_cli_argv   (const gchar *command,
                                 const gchar *provider,
                                 const gchar *model,
                                 const gchar *system);

/**
 * bar_ai_cmacs_form:
 * @widget: the widget's registry name
 * @request_id: the request id cmacs must echo back
 * @system: (nullable): a system prompt
 * @prompt: the user's text
 * @provider: (nullable): a cmacs-ai provider name, or %NULL for its
 *   default
 * @model: (nullable): a model id
 *
 * The Elisp `emacsctl eval' runs for one cmacs request:
 * `(cmacs-gowl-bar-ai-run WIDGET ID SYSTEM PROMPT PROVIDER MODEL)',
 * every argument a string literal or nil.  Returns %NULL when any text
 * is not valid UTF-8 or contains a NUL, since neither survives D-Bus.
 *
 * Returns: (transfer full) (nullable): the form
 */
gchar       *bar_ai_cmacs_form (const gchar *widget,
                                 const gchar *request_id,
                                 const gchar *system,
                                 const gchar *prompt,
                                 const gchar *provider,
                                 const gchar *model);

G_END_DECLS

#endif /* BAR_AI_H */
