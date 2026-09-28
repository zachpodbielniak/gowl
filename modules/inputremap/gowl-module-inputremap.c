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
 * gowl-module-inputremap.c - Per-device input remapping (opt-in).
 *
 * Implements GowlInputRemapper over the rule engine: the config's
 * `input-remap:' rules plus rules added at runtime.  Loaded only when
 * asked for -- `modules: inputremap: {enabled: true}' in gowl's YAML,
 * or (gowl-enable-module "inputremap") in cmacs -- and with it absent
 * the compositor's input path is untouched.
 *
 * Everything else the module offers is on IPC, so the socket, MCP, the
 * cmacs D-Bus surface and Elisp all reach the same code:
 *
 *   inputremap-status               enabled, rule and claim counts
 *   inputremap-devices              every keyboard and pointer (JSON)
 *   inputremap-list                 the rules in force (JSON)
 *   inputremap-add YAML             add or replace a runtime rule
 *   inputremap-remove NAME          remove a rule
 *   inputremap-clear                remove every runtime rule
 *   inputremap-reload               re-read the config's rules
 *   inputremap-enable / -disable    claim / release without unloading
 *   inputremap-identify [SECONDS]   report the next device pressed
 *   inputremap-identify-result      what identify found (JSON)
 *   inputremap-identify-cancel      stop listening
 *   inputremap-log LEVEL            none, claim, match or all
 *
 * Replies are one line: "OK ..." or "ERROR ...".  Structured replies
 * are "OK " followed by JSON.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-inputremap"

#include <glib-object.h>
#include <gmodule.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wayland-server-core.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-input-remapper.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "ipc/gowl-ipc.h"

#include "gowl-inputremap-engine.h"

#define IR_PREFIX                "inputremap-"
#define IR_DEFAULT_IDENTIFY_SECS (10)
#define IR_MAX_IDENTIFY_SECS     (300)

/**
 * GowlInputRemapLogLevel:
 * @IR_LOG_NONE: log nothing
 * @IR_LOG_CLAIM: log devices being claimed and released (the default)
 * @IR_LOG_MATCH: also log every remapped press
 * @IR_LOG_ALL: also log presses on claimed devices that pass through
 *
 * How much the module writes to its log.  A rule with `log: true' is
 * logged at %IR_LOG_MATCH whatever the level.
 */
typedef enum {
	IR_LOG_NONE,
	IR_LOG_CLAIM,
	IR_LOG_MATCH,
	IR_LOG_ALL
} GowlInputRemapLogLevel;

static const gchar *const ir_log_names[] = {
	"none", "claim", "match", "all", NULL
};

#define GOWL_TYPE_MODULE_INPUTREMAP (gowl_module_inputremap_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleInputremap, gowl_module_inputremap,
                     GOWL, MODULE_INPUTREMAP, GowlModule)

struct _GowlModuleInputremap {
	GowlModule              parent_instance;

	GowlCompositor         *compositor;       /* weak */
	GowlConfig             *config;           /* ref: whose rules are loaded */
	gulong                  reloaded_handler;

	GowlInputRemapEngine   *engine;
	gboolean                enabled;

	GowlInputRemapLogLevel  log_level;
	gchar                  *log_path;
	FILE                   *log_file;

	guint                   identify_secs;
	struct wl_event_source *identify_timer;
	gboolean                identifying;
	gchar                  *identify_result;  /* JSON, or NULL */

	struct wl_listener      display_destroy;
};

static void ir_remapper_init(GowlInputRemapperInterface *iface);
static void ir_ipc_init(GowlIpcHandlerInterface *iface);
static void ir_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleInputremap, gowl_module_inputremap,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_INPUT_REMAPPER, ir_remapper_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, ir_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, ir_startup_init))

/* --- logging --- */

/*
 * One line to the log file, or to stderr when there is none, stamped
 * with the local time.  Everything the module decides worth recording
 * goes through here, so `log-file' catches all of it.
 */
static void G_GNUC_PRINTF(2, 3)
ir_log(
	GowlModuleInputremap *self,
	const gchar          *format,
	...
){
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *stamp = NULL;
	g_autofree gchar *msg = NULL;
	va_list ap;

	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);

	now = g_date_time_new_now_local();
	stamp = g_date_time_format_iso8601(now);
	if (self->log_file != NULL) {
		fprintf(self->log_file, "%s inputremap: %s\n", stamp, msg);
		fflush(self->log_file);
	} else {
		g_printerr("%s inputremap: %s\n", stamp, msg);
	}
}

/* An IPC event line for socket subscribers (and cmacs's hooks). */
static void G_GNUC_PRINTF(2, 3)
ir_event(
	GowlModuleInputremap *self,
	const gchar          *format,
	...
){
	GowlIpc *ipc;
	g_autofree gchar *msg = NULL;
	va_list ap;

	if (self->compositor == NULL)
		return;
	ipc = gowl_compositor_get_ipc(self->compositor);
	if (ipc == NULL)
		return;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	gowl_ipc_push_event(ipc, "EVENT inputremap %s", msg);
}

static void
ir_close_log(GowlModuleInputremap *self)
{
	if (self->log_file != NULL) {
		fclose(self->log_file);
		self->log_file = NULL;
	}
}

/* Opens `log-file' for appending; on failure the log goes to stderr
 * and says why, rather than silently nowhere. */
static void
ir_open_log(GowlModuleInputremap *self)
{
	g_autofree gchar *path = NULL;

	ir_close_log(self);
	if (self->log_path == NULL || *self->log_path == '\0'
	    || g_strcmp0(self->log_path, "stderr") == 0)
		return;
	path = g_str_has_prefix(self->log_path, "~/")
		? g_build_filename(g_get_home_dir(), self->log_path + 2, NULL)
		: g_strdup(self->log_path);
	self->log_file = fopen(path, "a");
	if (self->log_file == NULL)
		g_warning("inputremap: cannot open log-file '%s': %s; logging "
		          "to stderr", path, g_strerror(errno));
}

static gboolean
ir_parse_log_level(
	const gchar            *text,
	GowlInputRemapLogLevel *out
){
	guint i;

	for (i = 0; ir_log_names[i] != NULL; i++) {
		if (g_ascii_strcasecmp(text, ir_log_names[i]) == 0) {
			*out = (GowlInputRemapLogLevel)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* --- config sync --- */

static void ir_on_config_reloaded(GowlConfig *config, gpointer data);

/* Loads the rules of the config the compositor is using now.  A reload
 * replaces the config object, so this compares identities; the ref held
 * on the loaded one rules out a freed-and-reused address comparing
 * equal. */
static void
ir_sync_config(GowlModuleInputremap *self)
{
	GowlConfig *current;

	if (self->compositor == NULL)
		return;
	current = gowl_compositor_get_config(self->compositor);
	if (current == self->config)
		return;

	if (self->config != NULL) {
		if (self->reloaded_handler != 0)
			g_signal_handler_disconnect(self->config,
			                            self->reloaded_handler);
		self->reloaded_handler = 0;
		g_clear_object(&self->config);
	}
	if (current == NULL) {
		gowl_input_remap_engine_set_config_rules(self->engine, NULL);
		return;
	}
	self->config = g_object_ref(current);
	self->reloaded_handler = g_signal_connect(current, "reloaded",
		G_CALLBACK(ir_on_config_reloaded), self);
	gowl_input_remap_engine_set_config_rules(self->engine,
		gowl_config_get_input_remap_rules(current));
}

static void
ir_reevaluate(GowlModuleInputremap *self)
{
	if (self->compositor != NULL)
		gowl_compositor_input_remap_reevaluate(self->compositor);
}

/* The same config object re-read in place (a YAML load into it). */
static void
ir_on_config_reloaded(
	GowlConfig *config,
	gpointer    data
){
	GowlModuleInputremap *self;

	self = GOWL_MODULE_INPUTREMAP(data);
	gowl_input_remap_engine_set_config_rules(self->engine,
		gowl_config_get_input_remap_rules(config));
	ir_reevaluate(self);
}

/* --- GowlInputRemapper --- */

static gboolean
ir_claims_device(
	GowlInputRemapper         *remapper,
	const GowlInputDeviceInfo *info
){
	GowlModuleInputremap *self;

	self = GOWL_MODULE_INPUTREMAP(remapper);
	if (!self->enabled)
		return FALSE;
	ir_sync_config(self);
	return gowl_input_remap_engine_claims(self->engine, info);
}

static GowlInputRemapRule *
ir_map_event(
	GowlInputRemapper           *remapper,
	const GowlInputDeviceInfo   *info,
	const GowlInputRemapEvent   *event,
	const GowlInputRemapTarget **out_target
){
	GowlModuleInputremap *self;
	GowlInputRemapRule *rule;
	g_autofree gchar *input = NULL;

	self = GOWL_MODULE_INPUTREMAP(remapper);
	*out_target = NULL;
	if (!self->enabled)
		return NULL;

	rule = gowl_input_remap_engine_map(self->engine, info, event->code,
	                                   out_target);
	if (!event->pressed)
		return rule;

	/* Logging and events, presses only: the release mirrors them */
	if (rule != NULL && *out_target != NULL) {
		g_autofree gchar *target = NULL;

		input = gowl_input_remap_code_to_name(event->code);
		target = gowl_input_remap_target_to_string(*out_target);
		if (self->log_level >= IR_LOG_MATCH
		    || gowl_input_remap_rule_get_log(rule))
			ir_log(self, "%s: device %u \"%s\" %s -> %s",
			       gowl_input_remap_rule_get_name(rule), info->id,
			       info->name != NULL ? info->name : "", input, target);
		ir_event(self, "match %s %u %s",
		         gowl_input_remap_rule_get_name(rule), info->id, input);
	} else if (self->log_level >= IR_LOG_ALL) {
		input = gowl_input_remap_code_to_name(event->code);
		ir_log(self, "device %u \"%s\" %s -> %s", info->id,
		       info->name != NULL ? info->name : "", input,
		       rule != NULL && gowl_input_remap_rule_get_drop_unmatched(rule)
		       ? "drop (unmatched)" : "pass");
	}
	return rule;
}

static void
ir_device_changed(
	GowlInputRemapper         *remapper,
	const GowlInputDeviceInfo *info,
	gboolean                   claimed
){
	GowlModuleInputremap *self;

	self = GOWL_MODULE_INPUTREMAP(remapper);
	if (self->log_level >= IR_LOG_CLAIM)
		ir_log(self, "%s device %u %s %04x:%04x %s \"%s\"",
		       claimed ? "claimed" : "released", info->id,
		       info->type == GOWL_INPUT_REMAP_DEVICE_KEYBOARD
		       ? "keyboard" : "pointer",
		       (guint)info->vendor, (guint)info->product,
		       info->sysname != NULL ? info->sysname : "-",
		       info->name != NULL ? info->name : "");
	ir_event(self, "%s %u %04x:%04x", claimed ? "claim" : "release",
	         info->id, (guint)info->vendor, (guint)info->product);
}

static void
ir_remapper_init(GowlInputRemapperInterface *iface)
{
	iface->claims_device  = ir_claims_device;
	iface->map_event      = ir_map_event;
	iface->device_changed = ir_device_changed;
}

/* --- JSON replies --- */

static void
ir_json_device(
	JsonBuilder               *b,
	const GowlInputDeviceInfo *info
){
	g_autofree gchar *id = NULL;

	id = g_strdup_printf("%04x:%04x", (guint)info->vendor,
	                     (guint)info->product);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_add_int_value(b, info->id);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b,
		info->type == GOWL_INPUT_REMAP_DEVICE_KEYBOARD ? "keyboard"
		                                              : "pointer");
	json_builder_set_member_name(b, "name");
	json_builder_add_string_value(b, info->name != NULL ? info->name : "");
	json_builder_set_member_name(b, "vendor-product");
	json_builder_add_string_value(b, id);
	json_builder_set_member_name(b, "sysname");
	if (info->sysname != NULL)
		json_builder_add_string_value(b, info->sysname);
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "claimed");
	json_builder_add_boolean_value(b, info->claimed);
	json_builder_end_object(b);
}

static gchar *
ir_json_finish(JsonBuilder *b)
{
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;

	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);
	text = json_generator_to_data(gen, NULL);
	return g_strdup_printf("OK %s", text);
}

/* --- IPC commands --- */

static gchar *
ir_cmd_status(GowlModuleInputremap *self)
{
	g_autoptr(GPtrArray) devices = NULL;
	guint claimed;
	guint i;

	claimed = 0;
	if (self->compositor != NULL) {
		devices = gowl_compositor_list_input_devices(self->compositor);
		for (i = 0; i < devices->len; i++)
			if (((GowlInputDeviceInfo *)g_ptr_array_index(devices, i))->claimed)
				claimed++;
	}
	return g_strdup_printf("OK enabled=%d rules=%u claimed=%u log=%s "
	                       "identifying=%d",
	                       self->enabled ? 1 : 0,
	                       gowl_input_remap_engine_get_n_rules(self->engine),
	                       claimed, ir_log_names[self->log_level],
	                       self->identifying ? 1 : 0);
}

static gchar *
ir_cmd_devices(GowlModuleInputremap *self)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	guint i;

	if (self->compositor == NULL)
		return g_strdup("ERROR the input remapper has not started");
	devices = gowl_compositor_list_input_devices(self->compositor);
	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; i < devices->len; i++)
		ir_json_device(b, g_ptr_array_index(devices, i));
	json_builder_end_array(b);
	return ir_json_finish(b);
}

static gchar *
ir_cmd_list(GowlModuleInputremap *self)
{
	g_autoptr(GPtrArray) rules = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	guint i;
	guint d;

	ir_sync_config(self);
	rules = gowl_input_remap_engine_get_rules(self->engine);
	if (self->compositor != NULL)
		devices = gowl_compositor_list_input_devices(self->compositor);

	b = json_builder_new();
	json_builder_begin_array(b);
	for (i = 0; i < rules->len; i++) {
		GowlInputRemapRule *r = g_ptr_array_index(rules, i);
		g_autofree gchar *yaml = gowl_input_remap_rule_to_yaml(r);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, gowl_input_remap_rule_get_name(r));
		json_builder_set_member_name(b, "source");
		json_builder_add_string_value(b,
			gowl_input_remap_engine_get_source(self->engine, r)
			== GOWL_INPUT_REMAP_SOURCE_RUNTIME ? "runtime" : "config");
		json_builder_set_member_name(b, "mappings");
		json_builder_add_int_value(b, gowl_input_remap_rule_get_n_mappings(r));
		json_builder_set_member_name(b, "yaml");
		json_builder_add_string_value(b, yaml);
		/* the connected devices this rule matches */
		json_builder_set_member_name(b, "devices");
		json_builder_begin_array(b);
		for (d = 0; devices != NULL && d < devices->len; d++) {
			GowlInputDeviceInfo *info = g_ptr_array_index(devices, d);

			if (gowl_input_remap_rule_matches(r, info))
				json_builder_add_int_value(b, info->id);
		}
		json_builder_end_array(b);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return ir_json_finish(b);
}

static gchar *
ir_cmd_add(
	GowlModuleInputremap *self,
	const gchar          *args
){
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GError) error = NULL;
	gboolean replaced;

	if (args == NULL || *args == '\0')
		return g_strdup("ERROR inputremap-add needs a rule, e.g. "
		                "{name: p, match: {id: \"1a86:e026\"}, "
		                "map: {KEY_A: {button: middle}}}");
	rule = gowl_input_remap_rule_new_from_yaml(args, &error);
	if (rule == NULL)
		return g_strdup_printf("ERROR %s", error->message);

	ir_sync_config(self);
	replaced = gowl_input_remap_engine_add(self->engine, rule);
	if (self->log_level >= IR_LOG_CLAIM)
		ir_log(self, "%s rule %s", replaced ? "replaced" : "added",
		       gowl_input_remap_rule_get_name(rule));
	ir_reevaluate(self);
	return g_strdup_printf("OK %s %s", replaced ? "replaced" : "added",
	                       gowl_input_remap_rule_get_name(rule));
}

static gchar *
ir_cmd_remove(
	GowlModuleInputremap *self,
	const gchar          *args
){
	g_autofree gchar *name = NULL;
	GowlInputRemapRuleSource source;

	if (args == NULL || *args == '\0')
		return g_strdup("ERROR inputremap-remove needs a rule name");
	name = g_strstrip(g_strdup(args));
	ir_sync_config(self);
	if (!gowl_input_remap_engine_remove(self->engine, name, &source))
		return g_strdup_printf("ERROR no rule named %s", name);
	if (self->log_level >= IR_LOG_CLAIM)
		ir_log(self, "removed rule %s", name);
	ir_reevaluate(self);
	return g_strdup_printf("OK removed %s%s", name,
	                       source == GOWL_INPUT_REMAP_SOURCE_CONFIG
	                       ? " (a config rule; back on the next reload)" : "");
}

static gchar *
ir_cmd_clear(GowlModuleInputremap *self)
{
	guint n;

	n = gowl_input_remap_engine_clear_runtime(self->engine);
	ir_reevaluate(self);
	return g_strdup_printf("OK cleared %u", n);
}

static gchar *
ir_cmd_reload(GowlModuleInputremap *self)
{
	if (self->config != NULL)
		gowl_input_remap_engine_set_config_rules(self->engine,
			gowl_config_get_input_remap_rules(self->config));
	ir_sync_config(self);
	ir_reevaluate(self);
	return g_strdup_printf("OK rules=%u",
	                       gowl_input_remap_engine_get_n_rules(self->engine));
}

static gchar *
ir_cmd_set_enabled(
	GowlModuleInputremap *self,
	gboolean              enabled
){
	self->enabled = enabled;
	ir_reevaluate(self);
	return g_strdup(enabled ? "OK enabled" : "OK disabled");
}

static gchar *
ir_cmd_log(
	GowlModuleInputremap *self,
	const gchar          *args
){
	GowlInputRemapLogLevel level;
	g_autofree gchar *word = NULL;

	if (args == NULL || *args == '\0')
		return g_strdup_printf("OK %s", ir_log_names[self->log_level]);
	word = g_strstrip(g_strdup(args));
	if (!ir_parse_log_level(word, &level))
		return g_strdup("ERROR expected none, claim, match or all");
	self->log_level = level;
	return g_strdup_printf("OK %s", ir_log_names[level]);
}

/* --- identify mode --- */

static void
ir_identify_stop(GowlModuleInputremap *self)
{
	if (self->identify_timer != NULL) {
		wl_event_source_remove(self->identify_timer);
		self->identify_timer = NULL;
	}
	if (self->identifying && self->compositor != NULL)
		gowl_compositor_input_remap_identify_stop(self->compositor);
	self->identifying = FALSE;
}

/* The first press on any device: record who it was, then stop. */
static void
ir_on_identified(
	GowlCompositor            *compositor,
	const GowlInputDeviceInfo *info,
	const GowlInputRemapEvent *event,
	gpointer                   data
){
	GowlModuleInputremap *self;
	g_autoptr(JsonBuilder) b = NULL;
	g_autoptr(JsonGenerator) gen = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *input = NULL;

	(void)compositor;
	self = GOWL_MODULE_INPUTREMAP(data);
	input = gowl_input_remap_code_to_name(event->code);

	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "device");
	ir_json_device(b, info);
	json_builder_set_member_name(b, "input");
	json_builder_add_string_value(b, input);
	json_builder_set_member_name(b, "code");
	json_builder_add_int_value(b, event->code);
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	gen = json_generator_new();
	json_generator_set_root(gen, root);

	g_free(self->identify_result);
	self->identify_result = json_generator_to_data(gen, NULL);
	ir_log(self, "identified %s on device %u \"%s\" %04x:%04x", input,
	       info->id, info->name != NULL ? info->name : "",
	       (guint)info->vendor, (guint)info->product);
	ir_event(self, "identified %s", self->identify_result);
	ir_identify_stop(self);
}

static gint
ir_identify_timeout(gpointer data)
{
	GowlModuleInputremap *self;

	self = GOWL_MODULE_INPUTREMAP(data);
	self->identify_timer = NULL;   /* removed by returning */
	if (self->identifying) {
		ir_event(self, "identify-timeout");
		if (self->compositor != NULL)
			gowl_compositor_input_remap_identify_stop(self->compositor);
		self->identifying = FALSE;
	}
	return 0;
}

static gchar *
ir_cmd_identify(
	GowlModuleInputremap *self,
	const gchar          *args
){
	struct wl_event_loop *loop;
	guint secs;

	if (self->compositor == NULL)
		return g_strdup("ERROR the input remapper has not started");
	secs = self->identify_secs;
	if (args != NULL && *args != '\0') {
		g_autofree gchar *word = g_strstrip(g_strdup(args));
		guint64 v;

		if (!g_ascii_string_to_unsigned(word, 10, 1,
		                                IR_MAX_IDENTIFY_SECS, &v, NULL))
			return g_strdup_printf("ERROR seconds must be 1..%d",
			                       IR_MAX_IDENTIFY_SECS);
		secs = (guint)v;
	}

	ir_identify_stop(self);
	g_clear_pointer(&self->identify_result, g_free);
	if (!gowl_compositor_input_remap_identify_start(self->compositor,
	                                                ir_on_identified, self))
		return g_strdup("ERROR could not observe the input devices");
	self->identifying = TRUE;

	/* A wl_event_loop timer, never g_timeout_add: under cmacs --gowl
	 * the GLib default context belongs to the editor's thread, and the
	 * compositor's state is only safe to touch from its own loop. */
	loop = gowl_compositor_get_event_loop(self->compositor);
	if (loop != NULL) {
		self->identify_timer = wl_event_loop_add_timer(loop,
			ir_identify_timeout, self);
		if (self->identify_timer != NULL)
			wl_event_source_timer_update(self->identify_timer,
			                             (gint)(secs * 1000));
	}
	return g_strdup_printf("OK listening %u", secs);
}

static gchar *
ir_cmd_identify_result(GowlModuleInputremap *self)
{
	if (self->identify_result != NULL)
		return g_strdup_printf("OK %s", self->identify_result);
	return g_strdup(self->identifying ? "OK pending" : "OK none");
}

/*
 * ir_handle_command:
 * @handler: the module
 * @command: the command word
 * @args: (nullable): the rest of the line
 *
 * Claims every `inputremap-' word and nothing else.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL for a word
 *   that is not the module's
 */
static gchar *
ir_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	GowlModuleInputremap *self;
	const gchar *verb;

	if (command == NULL || !g_str_has_prefix(command, IR_PREFIX))
		return NULL;
	self = GOWL_MODULE_INPUTREMAP(handler);
	verb = command + strlen(IR_PREFIX);

	if (g_strcmp0(verb, "status") == 0)
		return ir_cmd_status(self);
	if (g_strcmp0(verb, "devices") == 0)
		return ir_cmd_devices(self);
	if (g_strcmp0(verb, "list") == 0)
		return ir_cmd_list(self);
	if (g_strcmp0(verb, "add") == 0)
		return ir_cmd_add(self, args);
	if (g_strcmp0(verb, "remove") == 0)
		return ir_cmd_remove(self, args);
	if (g_strcmp0(verb, "clear") == 0)
		return ir_cmd_clear(self);
	if (g_strcmp0(verb, "reload") == 0)
		return ir_cmd_reload(self);
	if (g_strcmp0(verb, "enable") == 0)
		return ir_cmd_set_enabled(self, TRUE);
	if (g_strcmp0(verb, "disable") == 0)
		return ir_cmd_set_enabled(self, FALSE);
	if (g_strcmp0(verb, "log") == 0)
		return ir_cmd_log(self, args);
	if (g_strcmp0(verb, "identify") == 0)
		return ir_cmd_identify(self, args);
	if (g_strcmp0(verb, "identify-result") == 0)
		return ir_cmd_identify_result(self);
	if (g_strcmp0(verb, "identify-cancel") == 0) {
		ir_identify_stop(self);
		return g_strdup("OK cancelled");
	}
	return g_strdup_printf("ERROR unknown command %s; the input remapper "
	                       "knows " IR_PREFIX "status, -devices, -list, "
	                       "-add, -remove, -clear, -reload, -enable, "
	                       "-disable, -log, -identify, -identify-result "
	                       "and -identify-cancel", command);
}

static void
ir_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = ir_handle_command;
}

/* --- GowlModule --- */

/*
 * ir_configure:
 * @mod: the module
 * @config: a #GHashTable of string settings from `modules: inputremap:'
 *
 * log (none/claim/match/all), log-file (a path, `~/' allowed, or
 * `stderr'), identify-timeout (seconds).  A bad value is refused with a
 * warning and the old one kept.  The rules themselves live in the
 * top-level `input-remap:' section, not here.
 */
static void
ir_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleInputremap *self;
	GHashTable *settings;
	const gchar *value;

	self = GOWL_MODULE_INPUTREMAP(mod);
	settings = (GHashTable *)config;
	if (settings == NULL)
		return;

	if ((value = g_hash_table_lookup(settings, "log")) != NULL) {
		GowlInputRemapLogLevel level;

		if (ir_parse_log_level(value, &level))
			self->log_level = level;
		else
			g_warning("inputremap: log '%s' -- expected none, claim, "
			          "match or all", value);
	}
	if ((value = g_hash_table_lookup(settings, "log-file")) != NULL) {
		g_free(self->log_path);
		self->log_path = g_strdup(value);
		ir_open_log(self);
	}
	if ((value = g_hash_table_lookup(settings, "identify-timeout")) != NULL) {
		guint64 v;

		if (g_ascii_string_to_unsigned(value, 10, 1, IR_MAX_IDENTIFY_SECS,
		                               &v, NULL))
			self->identify_secs = (guint)v;
		else
			g_warning("inputremap: identify-timeout '%s' -- 1..%d "
			          "seconds", value, IR_MAX_IDENTIFY_SECS);
	}
}

static gboolean
ir_activate(GowlModule *mod)
{
	GowlModuleInputremap *self;

	self = GOWL_MODULE_INPUTREMAP(mod);
	self->enabled = TRUE;
	/* Enabled after startup (cmacs's gowl-enable-module): claim what
	 * is already plugged in.  At first start the startup handler does
	 * this instead, once the compositor exists. */
	ir_sync_config(self);
	ir_reevaluate(self);
	return TRUE;
}

/* Switched off: every device goes back to the keyboard group, anything
 * held is released.  The compositor still sees this module as active
 * while this runs, so `enabled' is what makes it claim nothing. */
static void
ir_deactivate(GowlModule *mod)
{
	GowlModuleInputremap *self;

	self = GOWL_MODULE_INPUTREMAP(mod);
	ir_identify_stop(self);
	self->enabled = FALSE;
	ir_reevaluate(self);
}

static const gchar *
ir_get_name(GowlModule *mod)
{
	(void)mod;
	return "inputremap";
}

static const gchar *
ir_get_description(GowlModule *mod)
{
	(void)mod;
	return "Per-device input remapping (keys, buttons, wheel; one press, "
	       "one output)";
}

static const gchar *
ir_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlStartupHandler --- */

/* The identify timer belongs to the display's event loop, which goes
 * with the display. */
static void
ir_display_destroyed(
	struct wl_listener *listener,
	void               *data
){
	GowlModuleInputremap *self;

	(void)data;
	self = wl_container_of(listener, self, display_destroy);
	if (self->identify_timer != NULL) {
		wl_event_source_remove(self->identify_timer);
		self->identify_timer = NULL;
	}
	self->identifying = FALSE;
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
ir_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleInputremap *self;
	struct wl_display *display;

	self = GOWL_MODULE_INPUTREMAP(handler);
	if (self->compositor == NULL) {
		self->compositor = GOWL_COMPOSITOR(compositor);
		g_object_add_weak_pointer(G_OBJECT(compositor),
		                          (gpointer *)&self->compositor);
	}
	display = gowl_compositor_get_wl_display(self->compositor);
	if (display != NULL && wl_list_empty(&self->display_destroy.link))
		wl_display_add_destroy_listener(display, &self->display_destroy);

	ir_sync_config(self);
	ir_reevaluate(self);
	if (self->log_level >= IR_LOG_CLAIM)
		ir_log(self, "started with %u rule(s)",
		       gowl_input_remap_engine_get_n_rules(self->engine));
}

static void
ir_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = ir_on_startup;
}

/* --- GObject lifecycle --- */

static void
gowl_module_inputremap_finalize(GObject *object)
{
	GowlModuleInputremap *self;

	self = GOWL_MODULE_INPUTREMAP(object);
	wl_list_remove(&self->display_destroy.link);
	if (self->identify_timer != NULL)
		wl_event_source_remove(self->identify_timer);
	if (self->config != NULL && self->reloaded_handler != 0)
		g_signal_handler_disconnect(self->config, self->reloaded_handler);
	g_clear_object(&self->config);
	if (self->compositor != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
	gowl_input_remap_engine_free(self->engine);
	ir_close_log(self);
	g_free(self->log_path);
	g_free(self->identify_result);

	G_OBJECT_CLASS(gowl_module_inputremap_parent_class)->finalize(object);
}

static void
gowl_module_inputremap_class_init(GowlModuleInputremapClass *klass)
{
	GObjectClass    *object_class;
	GowlModuleClass *module_class;

	object_class = G_OBJECT_CLASS(klass);
	module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_inputremap_finalize;

	module_class->activate        = ir_activate;
	module_class->deactivate      = ir_deactivate;
	module_class->get_name        = ir_get_name;
	module_class->get_description = ir_get_description;
	module_class->get_version     = ir_get_version;
	module_class->configure       = ir_configure;
}

static void
gowl_module_inputremap_init(GowlModuleInputremap *self)
{
	wl_list_init(&self->display_destroy.link);
	self->display_destroy.notify = ir_display_destroyed;
	self->engine = gowl_input_remap_engine_new();
	self->enabled = FALSE;
	self->log_level = IR_LOG_CLAIM;
	self->identify_secs = IR_DEFAULT_IDENTIFY_SECS;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_INPUTREMAP;
}
