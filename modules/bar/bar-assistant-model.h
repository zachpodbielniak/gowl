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

#ifndef BAR_ASSISTANT_MODEL_H
#define BAR_ASSISTANT_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * The pure half of the `ai-assistant' widget: the conversation, the
 * ollama replies, the memory arithmetic, the approval card, the curl
 * command lines and the activity wave.
 *
 * Nothing in here touches a GowlBarPlugin, the theme or a subprocess.
 * The only files it reads are the small /proc and /sys ones the
 * machine probe names, under a root the caller passes in --- so the
 * tests point it at a fixture tree and every number it produces can be
 * checked against an input someone wrote down.
 */

/* ── The conversation ─────────────────────────────────────────────── */

/**
 * BarAssistantRole:
 * @BAR_ASSISTANT_ROLE_USER: something the user typed
 * @BAR_ASSISTANT_ROLE_ASSISTANT: something the model answered
 */
typedef enum {
	BAR_ASSISTANT_ROLE_USER = 0,
	BAR_ASSISTANT_ROLE_ASSISTANT
} BarAssistantRole;

/**
 * BarAssistantTurn:
 * @role: who said it
 * @text: what was said; always valid UTF-8
 */
typedef struct {
	BarAssistantRole  role;
	gchar            *text;
} BarAssistantTurn;

/**
 * BarAssistantTranscript:
 *
 * The messages of one conversation, oldest first.  Not thread-safe:
 * the widget holds it under its own lock.
 */
typedef struct _BarAssistantTranscript BarAssistantTranscript;

/**
 * BAR_ASSISTANT_MAX_MESSAGE:
 *
 * The longest single message kept.  A model that loops can produce
 * megabytes; the transcript is re-sent with every question, so an
 * unbounded message is an unbounded prompt.
 */
#define BAR_ASSISTANT_MAX_MESSAGE (256 * 1024)

BarAssistantTranscript *bar_assistant_transcript_new   (void);
void                    bar_assistant_transcript_free  (BarAssistantTranscript *self);
void                    bar_assistant_transcript_clear (BarAssistantTranscript *self);

/**
 * bar_assistant_transcript_add:
 * @self: a transcript
 * @role: who said it
 * @text: (nullable): what was said; made valid UTF-8 and cut to
 *   %BAR_ASSISTANT_MAX_MESSAGE
 */
void bar_assistant_transcript_add (BarAssistantTranscript *self,
                                   BarAssistantRole        role,
                                   const gchar            *text);

/**
 * bar_assistant_transcript_pop:
 * @self: a transcript
 *
 * Drops the newest message --- a question that never left, because
 * the backend refused it, should not be re-sent with the next one.
 */
void bar_assistant_transcript_pop (BarAssistantTranscript *self);

/**
 * bar_assistant_transcript_trim:
 * @self: a transcript
 * @max_messages: how many messages to keep; 0 keeps none
 *
 * Keeps the newest @max_messages.  If that leaves an answer first,
 * it goes too: an answer whose question has been forgotten reads as
 * the model talking to itself.
 */
void bar_assistant_transcript_trim (BarAssistantTranscript *self,
                                    guint                   max_messages);

guint                   bar_assistant_transcript_len (BarAssistantTranscript *self);
const BarAssistantTurn *bar_assistant_transcript_get (BarAssistantTranscript *self,
                                                      guint                   index);

/**
 * bar_assistant_transcript_flatten:
 * @self: a transcript
 * @next_user: (nullable): the question about to be asked, or %NULL to
 *   flatten only what has been said
 * @max_bytes: the longest result wanted; 0 for no limit
 *
 * The conversation as one document, which is what both backends take:
 *
 *   User: first question
 *
 *   Assistant: first answer
 *
 *   User: @next_user
 *
 * When it would exceed @max_bytes the OLDEST messages are left out
 * first; @next_user is always kept, since it is the question.
 *
 * Returns: (transfer full): the document
 */
gchar *bar_assistant_transcript_flatten (BarAssistantTranscript *self,
                                         const gchar            *next_user,
                                         gsize                   max_bytes);

/* ── Names ────────────────────────────────────────────────────────── */

/**
 * bar_assistant_valid_provider:
 * @name: (nullable): a provider name
 *
 * Letters, digits, `.', `_' and `-', at most 64 bytes.  A provider
 * name ends up in argv and in an Elisp form; nothing else is needed.
 *
 * Returns: %TRUE when @name is acceptable
 */
gboolean bar_assistant_valid_provider (const gchar *name);

/**
 * bar_assistant_valid_model:
 * @name: (nullable): a model id
 *
 * What an ollama or provider model id can contain: letters, digits and
 * `. _ - : / @ +', at most 200 bytes, not starting with `-' (it is
 * one argv element away from being taken for an option) and with no
 * `..'.  `hf.co/LiquidAI/LFM2-1.2B-RAG-GGUF:Q8_0' passes;
 * `x" ,"keep_alive":0' does not.
 *
 * Returns: %TRUE when @name is acceptable
 */
gboolean bar_assistant_valid_model (const gchar *name);

/**
 * bar_assistant_parse_provider_listing:
 * @text: (nullable): the output of `ai --list-providers'
 *
 * The first word of every indented line after `Providers:'.  Anything
 * that is not a plausible provider name is skipped, so a changed
 * output format costs the list rather than producing nonsense in it.
 *
 * Returns: (transfer full) (array zero-terminated=1): the names, in
 *   order; empty, never %NULL
 */
gchar **bar_assistant_parse_provider_listing (const gchar *text);

/**
 * bar_assistant_split_list:
 * @csv: (nullable): a comma-separated list, as cmacs pushes it
 * @models: %TRUE to validate entries as model ids, %FALSE as provider
 *   names
 *
 * Trimmed, de-duplicated, invalid entries dropped, at most 128.
 *
 * Returns: (transfer full) (array zero-terminated=1): the entries
 */
gchar **bar_assistant_split_list (const gchar *csv, gboolean models);

/* ── ollama ───────────────────────────────────────────────────────── */

/**
 * BarAssistantModel:
 * @name: the model's name, e.g. `qwen3:8b'
 * @family: (nullable): `llama', `qwen3', ...
 * @params: (nullable): parameter count as ollama prints it, `8.0B'
 * @quant: (nullable): quantisation level, `Q4_K_M'
 * @size: bytes on disk (installed) or in memory (loaded)
 * @size_vram: bytes of @size held in VRAM; loaded models only
 * @expires_at: (nullable): when a loaded model will be unloaded
 */
typedef struct {
	gchar   *name;
	gchar   *family;
	gchar   *params;
	gchar   *quant;
	guint64  size;
	guint64  size_vram;
	gchar   *expires_at;
} BarAssistantModel;

void bar_assistant_model_free (BarAssistantModel *model);

/**
 * BAR_ASSISTANT_MAX_MODELS:
 *
 * The most models one reply is read for.  A hostile or confused
 * server can send any number; the panel can usefully show far fewer.
 */
#define BAR_ASSISTANT_MAX_MODELS (256)

/**
 * bar_assistant_parse_models:
 * @json: (nullable): a reply from `/api/tags' or `/api/ps'
 * @error: (out) (optional) (transfer full): why it could not be read
 *
 * Reads the `models' array.  Fields are read at the top level of each
 * model object only --- `details' is read on its own --- so a field of
 * the same name nested anywhere else cannot be taken for the model's.
 * Every string is made valid UTF-8 and length-capped; a model with no
 * name is skipped.
 *
 * Returns: (transfer full) (nullable) (element-type BarAssistantModel):
 *   the models, possibly none, or %NULL when @json is not an ollama
 *   reply at all
 */
GPtrArray *bar_assistant_parse_models (const gchar  *json,
                                       gchar       **error);

/**
 * BarAssistantPull:
 * @status: (nullable): ollama's status text, `pulling manifest',
 *   `downloading sha256:...', `success'
 * @error: (nullable): the error ollama reported, if this line is one
 * @total: bytes in the current layer, 0 when not reported
 * @completed: bytes of it fetched
 * @done: whether this line says the pull finished
 */
typedef struct {
	gchar   *status;
	gchar   *error;
	guint64  total;
	guint64  completed;
	gboolean done;
} BarAssistantPull;

/**
 * bar_assistant_parse_pull_line:
 * @line: one line of `/api/pull' output
 * @out: (out caller-allocates): the reading; clear it with
 *   bar_assistant_pull_clear()
 *
 * Returns: %TRUE when @line was a progress object
 */
gboolean bar_assistant_parse_pull_line (const gchar      *line,
                                        BarAssistantPull *out);
void     bar_assistant_pull_clear      (BarAssistantPull *pull);

/**
 * bar_assistant_json_escape:
 * @text: (nullable): a string
 *
 * The body of a JSON string literal for @text: quotes, backslashes and
 * every control character escaped.  Model names are validated before
 * they get here as well; this is the second lock, not the only one.
 *
 * Returns: (transfer full): the escaped text, without the quotes
 */
gchar *bar_assistant_json_escape (const gchar *text);

/**
 * bar_assistant_normalise_url:
 * @url: (nullable): the `ollama-url' setting
 *
 * `http://' or `https://', no whitespace, no trailing slash.  Anything
 * else is refused: the URL is curl's last argument, and a value
 * starting with `-' would be an option.
 *
 * Returns: (transfer full) (nullable): the URL, or %NULL
 */
gchar *bar_assistant_normalise_url (const gchar *url);

/**
 * BarAssistantOp:
 * @BAR_ASSISTANT_OP_NONE: nothing
 * @BAR_ASSISTANT_OP_TAGS: list installed models
 * @BAR_ASSISTANT_OP_PS: list loaded models
 * @BAR_ASSISTANT_OP_LOAD: load a model into memory
 * @BAR_ASSISTANT_OP_UNLOAD: unload it
 * @BAR_ASSISTANT_OP_DELETE: delete it from disk
 * @BAR_ASSISTANT_OP_PULL: download it
 * @BAR_ASSISTANT_OP_UNLOAD_ALL: unload every loaded model (approval
 *   only; it is run as one UNLOAD per model)
 */
typedef enum {
	BAR_ASSISTANT_OP_NONE = 0,
	BAR_ASSISTANT_OP_TAGS,
	BAR_ASSISTANT_OP_PS,
	BAR_ASSISTANT_OP_LOAD,
	BAR_ASSISTANT_OP_UNLOAD,
	BAR_ASSISTANT_OP_DELETE,
	BAR_ASSISTANT_OP_PULL,
	BAR_ASSISTANT_OP_UNLOAD_ALL
} BarAssistantOp;

/**
 * bar_assistant_curl_argv:
 * @curl: (nullable): the curl executable; %NULL for `curl'
 * @base_url: a URL from bar_assistant_normalise_url()
 * @op: what to ask
 * @model: (nullable): the model, for the operations that take one
 * @keep_alive: (nullable): how long a LOAD keeps the model, ollama's
 *   duration syntax (`30m'); %NULL or invalid for `30m'
 *
 * The exact argv for one ollama request.  No shell is involved, the
 * JSON body is built with bar_assistant_json_escape(), and the URL is
 * passed with `--url' so it can never be read as an option.
 *
 * Returns: (transfer full) (nullable): a NULL-terminated argv, or
 *   %NULL when @model is needed and not a valid model id
 */
gchar **bar_assistant_curl_argv (const gchar    *curl,
                                 const gchar    *base_url,
                                 BarAssistantOp  op,
                                 const gchar    *model,
                                 const gchar    *keep_alive);

/* ── Memory, and whether a model fits ─────────────────────────────── */

/**
 * BarAssistantMachine:
 * @cores: logical CPUs
 * @load: the 1, 5 and 15 minute load averages
 * @mem_total: bytes of RAM
 * @mem_avail: bytes the kernel says are available
 * @gpu_kind: (nullable): `NVIDIA' or `AMD' when a discrete-sized GPU
 *   was found
 * @vram_total: bytes of VRAM, summed over GPUs
 * @vram_used: bytes of it in use
 * @disk_free: bytes free where models are stored
 * @disk_total: bytes in that filesystem
 * @top: (array zero-terminated=1) (nullable): the processes holding
 *   the most memory, by name, largest first
 */
typedef struct {
	guint    cores;
	gdouble  load[3];
	guint64  mem_total;
	guint64  mem_avail;
	gchar   *gpu_kind;
	guint64  vram_total;
	guint64  vram_used;
	guint64  disk_free;
	guint64  disk_total;
	gchar  **top;
} BarAssistantMachine;

/**
 * BAR_ASSISTANT_MIN_VRAM:
 *
 * VRAM below this is an integrated GPU's carve-out, not somewhere a
 * model will be put; it is ignored, and RAM decides instead.
 */
#define BAR_ASSISTANT_MIN_VRAM ((guint64)2 * 1024 * 1024 * 1024)

void bar_assistant_machine_clear (BarAssistantMachine *machine);
void bar_assistant_machine_copy  (const BarAssistantMachine *src,
                                  BarAssistantMachine       *dst);

/**
 * bar_assistant_parse_nvidia_smi:
 * @text: (nullable): output of `nvidia-smi
 *   --query-gpu=memory.total,memory.used --format=csv,noheader,nounits'
 * @total: (out): bytes of VRAM, summed over GPUs
 * @used: (out): bytes in use
 *
 * Returns: %TRUE when at least one GPU line was read
 */
gboolean bar_assistant_parse_nvidia_smi (const gchar *text,
                                         guint64     *total,
                                         guint64     *used);

/**
 * bar_assistant_read_amd_vram:
 * @sys_root: the /sys to read, normally `/sys'
 * @total: (out): bytes of VRAM over every `cardN' that reports it
 * @used: (out): bytes in use
 *
 * Returns: %TRUE when any card reported VRAM
 */
gboolean bar_assistant_read_amd_vram (const gchar *sys_root,
                                      guint64     *total,
                                      guint64     *used);

/**
 * bar_assistant_read_machine:
 * @proc_root: the /proc to read
 * @sys_root: the /sys to read
 * @nvidia_text: (nullable): nvidia-smi's output, gathered by the
 *   caller (this function runs nothing)
 * @models_dir: (nullable): where models are stored, for disk space;
 *   the nearest existing parent is measured
 * @out: (out caller-allocates): the reading; clear it after
 *
 * One look at the machine.  Every file read is a small, fixed-format
 * /proc or /sys file; the process scan stops at 8192 entries.
 */
void bar_assistant_read_machine (const gchar         *proc_root,
                                 const gchar         *sys_root,
                                 const gchar         *nvidia_text,
                                 const gchar         *models_dir,
                                 BarAssistantMachine *out);

/**
 * bar_assistant_top_processes:
 * @proc_root: the /proc to read
 * @n: how many names
 *
 * Returns: (transfer full) (array zero-terminated=1): process names,
 *   most resident memory first, each name once
 */
gchar **bar_assistant_top_processes (const gchar *proc_root, guint n);

/**
 * BarAssistantFit:
 * @BAR_ASSISTANT_FIT_UNKNOWN: nothing to judge by
 * @BAR_ASSISTANT_FIT_COMFORTABLE: fits with room for its context
 * @BAR_ASSISTANT_FIT_TIGHT: fits, but only just, or only by spilling
 *   from VRAM into RAM (it will run, slowly)
 * @BAR_ASSISTANT_FIT_TOO_BIG: will not fit
 */
typedef enum {
	BAR_ASSISTANT_FIT_UNKNOWN = 0,
	BAR_ASSISTANT_FIT_COMFORTABLE,
	BAR_ASSISTANT_FIT_TIGHT,
	BAR_ASSISTANT_FIT_TOO_BIG
} BarAssistantFit;

/**
 * bar_assistant_fit:
 * @model_size: the model's size in bytes
 * @machine: the machine
 *
 * A model needs its own size plus about a fifth again for its context
 * and runtime.  With a GPU (at least %BAR_ASSISTANT_MIN_VRAM of VRAM)
 * it is comfortable when that fits in free VRAM, tight when the model
 * itself fits in free VRAM plus available RAM, and too big otherwise.
 * Without one, the same test against available RAM alone.
 *
 * Returns: the verdict
 */
BarAssistantFit bar_assistant_fit       (guint64                    model_size,
                                         const BarAssistantMachine *machine);
const gchar    *bar_assistant_fit_label (BarAssistantFit fit);

/**
 * bar_assistant_format_bytes:
 * @bytes: a byte count
 *
 * Decimal units, the way ollama itself prints sizes (`4.7 GB'), with
 * a decimal POINT whatever the locale.
 *
 * Returns: (transfer full): the text
 */
gchar *bar_assistant_format_bytes (guint64 bytes);

/**
 * bar_assistant_machine_summary:
 * @machine: the machine
 * @loaded: (nullable) (array zero-terminated=1): loaded model names
 *
 * The "live system info" paragraph prepended to the system prompt
 * when `machine-context' is on.  Plain, short, numbers with a decimal
 * point in every locale.
 *
 * Returns: (transfer full): the text
 */
gchar *bar_assistant_machine_summary (const BarAssistantMachine *machine,
                                      const gchar * const       *loaded);

/* ── The approval card ────────────────────────────────────────────── */

/**
 * BAR_ASSISTANT_APPROVAL_TTL:
 *
 * How long a confirmation card stays armed, in microseconds.  Long
 * enough to read the card; short enough that an Approve clicked an
 * hour later is not taken as consent to something long forgotten.
 */
#define BAR_ASSISTANT_APPROVAL_TTL (60 * G_USEC_PER_SEC)

/**
 * BarAssistantApproval:
 * @op: what is waiting to be approved, %BAR_ASSISTANT_OP_NONE when
 *   nothing is
 * @model: (nullable): the model it is for
 * @detail: (nullable): the card's text
 * @armed_at: monotonic time it was armed
 */
typedef struct {
	BarAssistantOp  op;
	gchar          *model;
	gchar          *detail;
	gint64          armed_at;
} BarAssistantApproval;

void     bar_assistant_approval_arm   (BarAssistantApproval *self,
                                       BarAssistantOp        op,
                                       const gchar          *model,
                                       const gchar          *detail,
                                       gint64                now);
void     bar_assistant_approval_clear (BarAssistantApproval *self);

/**
 * bar_assistant_approval_live:
 * @self: a card
 * @now: monotonic time
 *
 * Whether a card is showing.  An expired card is cleared here, so it
 * disappears the next time the panel is built and can never be
 * approved.
 *
 * Returns: %TRUE when armed and not expired
 */
gboolean bar_assistant_approval_live  (BarAssistantApproval *self,
                                       gint64                now);

/**
 * bar_assistant_approval_take:
 * @self: a card
 * @now: monotonic time
 * @model: (out) (optional) (transfer full) (nullable): the model
 *
 * Approve: returns what was waiting and clears the card.
 *
 * Returns: the operation, or %BAR_ASSISTANT_OP_NONE when nothing live
 *   was waiting
 */
BarAssistantOp bar_assistant_approval_take (BarAssistantApproval *self,
                                            gint64                now,
                                            gchar               **model);

/* ── The bar indicator ────────────────────────────────────────────── */

/**
 * BarAssistantLook:
 * @BAR_ASSISTANT_LOOK_IDLE: nothing happening
 * @BAR_ASSISTANT_LOOK_THINKING: asked, nothing back yet; or a model
 *   being loaded or pulled
 * @BAR_ASSISTANT_LOOK_STREAMING: an answer arriving
 * @BAR_ASSISTANT_LOOK_ERROR: the last thing failed
 */
typedef enum {
	BAR_ASSISTANT_LOOK_IDLE = 0,
	BAR_ASSISTANT_LOOK_THINKING,
	BAR_ASSISTANT_LOOK_STREAMING,
	BAR_ASSISTANT_LOOK_ERROR
} BarAssistantLook;

/**
 * bar_assistant_look_color_key:
 * @look: a state
 *
 * Returns: (transfer none): the setting that colours it,
 *   `color-idle', `color-thinking', `color-streaming' or `color-error'
 */
const gchar *bar_assistant_look_color_key (BarAssistantLook look);
const gchar *bar_assistant_look_name      (BarAssistantLook look);

/**
 * BAR_ASSISTANT_WAVE_MIN:
 *
 * The shortest a wave bar is drawn, as a fraction of the tallest: a
 * bar of zero height is invisible, and an indicator that vanishes when
 * nothing is happening looks like a widget that failed to load.
 */
#define BAR_ASSISTANT_WAVE_MIN (0.15)

/**
 * bar_assistant_wave:
 * @samples: (array length=n_samples) (nullable): recent activity,
 *   oldest first, each 0.0--1.0 (out-of-range values are clamped)
 * @n_samples: how many
 * @n_bars: how many bars to draw
 * @phase: a value advanced every poll while busy
 * @animate: whether to ripple; %FALSE draws the samples as they are
 * @out: (array length=n_bars) (out caller-allocates): each bar's
 *   height, %BAR_ASSISTANT_WAVE_MIN--1.0
 *
 * The newest @n_bars samples, right-aligned (a short history leaves
 * the oldest bars at the minimum), with a travelling ripple on top
 * while something is happening.
 */
void bar_assistant_wave (const gdouble *samples,
                         guint          n_samples,
                         guint          n_bars,
                         gdouble        phase,
                         gboolean       animate,
                         gdouble       *out);

/* ── Handing a conversation to cmacs ──────────────────────────────── */

/**
 * BAR_ASSISTANT_MAX_HANDOFF:
 *
 * The most transcript sent to cmacs in one command line.  Linux caps
 * one argv element at 128 KiB; this leaves room for the rest.
 */
#define BAR_ASSISTANT_MAX_HANDOFF (64 * 1024)

/**
 * bar_assistant_cmacs_open_cmdline:
 * @emacsctl: (nullable): the emacsctl command; %NULL for `emacsctl'
 * @provider: (nullable): the provider to continue with
 * @model: (nullable): its model
 * @transcript: (nullable): the conversation so far
 *
 * `emacsctl eval '(cmacs-gowl-bar-assistant-open-in-chat P M T)'' as
 * one shell command line for gowl_bar_plugin_spawn(): every Elisp
 * string escaped with bar_elisp_quote(), the form and the command each
 * passed through g_shell_quote().  A @transcript longer than
 * %BAR_ASSISTANT_MAX_HANDOFF keeps its END, which is the part being
 * continued.
 *
 * Returns: (transfer full) (nullable): the command line, or %NULL when
 *   a string is not valid UTF-8
 */
gchar *bar_assistant_cmacs_open_cmdline (const gchar *emacsctl,
                                         const gchar *provider,
                                         const gchar *model,
                                         const gchar *transcript);

G_END_DECLS

#endif /* BAR_ASSISTANT_MODEL_H */
