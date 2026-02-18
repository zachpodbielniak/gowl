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
 * gowl-module-mcp.c - MCP server module entry point.
 *
 * This module exposes the compositor's state and controls to AI
 * assistants via the Model Context Protocol (MCP).  It supports
 * two transports:
 *
 *   1. HTTP+SSE  - Direct HTTP connections with Server-Sent Events
 *                  (in-process, via McpHttpServerTransport)
 *
 *   2. Unix socket + stdio relay - A Unix domain socket that the
 *      external gowl-mcp binary bridges to stdin/stdout for AI
 *      clients like Claude Code.
 *
 * Both transports run on a dedicated GThread with its own GMainLoop
 * so they don't block the Wayland compositor event loop.  Tool calls
 * are dispatched to the compositor thread via eventfd (see
 * gowl-mcp-dispatch.c).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "gowl-version.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <string.h>

/* default MCP server instructions */
static const gchar *DEFAULT_INSTRUCTIONS =
	"You are connected to a Gowl Wayland compositor via MCP. "
	"You can query window state, manage windows, switch tags, "
	"adjust layouts, send input, take screenshots, and manage "
	"the clipboard. Use list_clients and list_monitors to "
	"understand the current desktop state before taking actions.";

/* --- interface init prototypes --- */
static void mcp_startup_init (GowlStartupHandlerInterface  *iface);
static void mcp_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleMcp, gowl_module_mcp,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		mcp_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER,
		mcp_shutdown_init))

/* ================================================================
 * Configuration
 * ================================================================ */

/**
 * mcp_configure:
 * @mod: the module
 * @config: a GHashTable<string,string> from YAML, or %NULL
 *
 * Parses module configuration from the YAML config file.
 * Keys: transport-http, transport-stdio, http-host, http-port,
 *        http-auth-token, instructions, tools (newline-delimited)
 */
static void
mcp_configure(GowlModule *mod, gpointer config)
{
	GowlModuleMcp *self;
	GHashTable    *settings;
	const gchar   *val;

	self = GOWL_MODULE_MCP(mod);

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	/* transport-http */
	val = (const gchar *)g_hash_table_lookup(settings, "transport-http");
	if (val != NULL)
		self->transport_http = (g_ascii_strcasecmp(val, "true") == 0 ||
		                        g_ascii_strcasecmp(val, "1") == 0);

	/* transport-stdio */
	val = (const gchar *)g_hash_table_lookup(settings, "transport-stdio");
	if (val != NULL)
		self->transport_stdio = (g_ascii_strcasecmp(val, "true") == 0 ||
		                         g_ascii_strcasecmp(val, "1") == 0);

	/* http-host */
	val = (const gchar *)g_hash_table_lookup(settings, "http-host");
	if (val != NULL) {
		g_free(self->http_host);
		self->http_host = g_strdup(val);
	}

	/* http-port */
	val = (const gchar *)g_hash_table_lookup(settings, "http-port");
	if (val != NULL)
		self->http_port = (gint)g_ascii_strtoll(val, NULL, 10);

	/* http-auth-token */
	val = (const gchar *)g_hash_table_lookup(settings, "http-auth-token");
	if (val != NULL) {
		g_free(self->http_auth_token);
		self->http_auth_token = g_strdup(val);
	}

	/* instructions */
	val = (const gchar *)g_hash_table_lookup(settings, "instructions");
	if (val != NULL) {
		g_free(self->instructions);
		self->instructions = g_strdup(val);
	}

	/* tools (newline-delimited list from YAML sequence) */
	val = (const gchar *)g_hash_table_lookup(settings, "tools");
	if (val != NULL && strlen(val) > 0) {
		gchar **tools;
		gint    i;

		if (self->allowed_tools != NULL)
			g_hash_table_unref(self->allowed_tools);

		self->allowed_tools = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL
		);

		tools = g_strsplit(val, "\n", -1);
		for (i = 0; tools[i] != NULL; i++) {
			gchar *trimmed;

			trimmed = g_strstrip(g_strdup(tools[i]));
			if (strlen(trimmed) > 0)
				g_hash_table_add(self->allowed_tools, trimmed);
			else
				g_free(trimmed);
		}
		g_strfreev(tools);

		g_debug("gowl-mcp: tool allowlist has %u entries",
		        g_hash_table_size(self->allowed_tools));
	}
}

/* ================================================================
 * Tool allowlist helper
 * ================================================================ */

gboolean
gowl_module_mcp_is_tool_allowed(
	GowlModuleMcp *self,
	const gchar   *tool_name
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(tool_name != NULL, FALSE);

	/* NULL allowlist means all tools are enabled */
	if (self->allowed_tools == NULL)
		return TRUE;

	return g_hash_table_contains(self->allowed_tools, tool_name);
}

/* ================================================================
 * Server setup (shared between HTTP and socket servers)
 * ================================================================ */

void
gowl_module_mcp_setup_server(
	GowlModuleMcp *self,
	McpServer     *server
){
	const gchar *instructions;

	g_return_if_fail(self != NULL);
	g_return_if_fail(server != NULL);

	instructions = (self->instructions != NULL)
		? self->instructions
		: DEFAULT_INSTRUCTIONS;

	mcp_server_set_instructions(server, instructions);

	/* register all allowed tools */
	gowl_mcp_register_all_tools(server, self);
}

/* ================================================================
 * Socket transport: Unix domain socket for stdio relay
 * ================================================================ */

/**
 * on_socket_session_created:
 * @unix_server: the McpUnixSocketServer
 * @server: the per-connection McpServer (not yet started)
 * @user_data: the GowlModuleMcp
 *
 * Called when a new client connects to the Unix domain socket.
 * Registers all allowed tools on the per-connection server before
 * the MCP handshake begins.
 */
static void
on_socket_session_created(
	McpUnixSocketServer *unix_server,
	McpServer           *server,
	gpointer             user_data
){
	(void)unix_server;
	gowl_module_mcp_setup_server(GOWL_MODULE_MCP(user_data), server);
}

/**
 * setup_socket_transport:
 * @self: the MCP module
 *
 * Creates an McpUnixSocketServer listening at
 * $XDG_RUNTIME_DIR/gowl-mcp.sock.
 *
 * Returns: %TRUE on success
 */
static gboolean
setup_socket_transport(GowlModuleMcp *self)
{
	const gchar       *runtime_dir;
	g_autofree gchar  *socket_path = NULL;
	g_autoptr(GError)  error = NULL;
	const gchar       *instructions;

	runtime_dir = g_get_user_runtime_dir();
	socket_path = g_build_filename(runtime_dir, "gowl-mcp.sock", NULL);

	self->socket_server = mcp_unix_socket_server_new(
		"gowl-mcp", GOWL_VERSION_STRING, socket_path);

	/* apply instructions so every session inherits them */
	instructions = (self->instructions != NULL)
		? self->instructions : DEFAULT_INSTRUCTIONS;
	mcp_unix_socket_server_set_instructions(
		self->socket_server, instructions);

	g_signal_connect(self->socket_server, "session-created",
	                 G_CALLBACK(on_socket_session_created), self);

	if (!mcp_unix_socket_server_start(self->socket_server, &error)) {
		g_warning("gowl-mcp: failed to start socket server on %s: %s",
		          socket_path, error->message);
		g_clear_object(&self->socket_server);
		return FALSE;
	}

	g_debug("gowl-mcp: socket transport listening on %s", socket_path);
	return TRUE;
}

/* ================================================================
 * HTTP+SSE transport
 * ================================================================ */

/**
 * on_http_server_started:
 * @source: the McpServer
 * @result: the async result
 * @user_data: the GowlModuleMcp
 *
 * Callback for mcp_server_start_async on the HTTP server.
 */
static void
on_http_server_started(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GowlModuleMcp     *self;
	g_autoptr(GError)  error = NULL;

	self = GOWL_MODULE_MCP(user_data);

	if (!mcp_server_start_finish(MCP_SERVER(source), result, &error)) {
		g_warning("gowl-mcp: HTTP server start failed: %s",
		          error->message);
		g_clear_object(&self->http_server);
		g_clear_object(&self->http_transport);
	} else {
		g_debug("gowl-mcp: HTTP server started on %s:%d",
		        self->http_host, self->http_port);
	}
}

/**
 * setup_http_transport:
 * @self: the MCP module
 *
 * Creates the HTTP+SSE server using McpHttpServerTransport.
 *
 * Returns: %TRUE on success
 */
static gboolean
setup_http_transport(GowlModuleMcp *self)
{
	self->http_transport = mcp_http_server_transport_new_full(
		self->http_host, (guint)self->http_port
	);

	/* configure authentication if token is set */
	if (self->http_auth_token != NULL && strlen(self->http_auth_token) > 0) {
		mcp_http_server_transport_set_require_auth(
			self->http_transport, TRUE);
		mcp_http_server_transport_set_auth_token(
			self->http_transport, self->http_auth_token);
	}

	self->http_server = mcp_server_new("gowl-mcp", GOWL_VERSION_STRING);
	mcp_server_set_transport(self->http_server,
	                         MCP_TRANSPORT(self->http_transport));
	gowl_module_mcp_setup_server(self, self->http_server);

	/* start async -- the server runs on the MCP thread's GMainContext */
	mcp_server_start_async(self->http_server, NULL,
	                       on_http_server_started, self);

	g_debug("gowl-mcp: HTTP transport initialised on %s:%d",
	        self->http_host, self->http_port);
	return TRUE;
}

/* ================================================================
 * MCP thread
 * ================================================================ */

/**
 * mcp_thread_func:
 * @data: the GowlModuleMcp
 *
 * Entry point for the dedicated MCP I/O thread.  Creates a
 * GMainLoop, sets up transports, and runs until quit.
 *
 * Returns: %NULL
 */
static gpointer
mcp_thread_func(gpointer data)
{
	GowlModuleMcp *self;

	self = GOWL_MODULE_MCP(data);

	/* push our context as the thread-default for this thread */
	g_main_context_push_thread_default(self->mcp_context);

	/* set up configured transports */
	if (self->transport_http)
		setup_http_transport(self);

	if (self->transport_stdio)
		setup_socket_transport(self);

	g_debug("gowl-mcp: thread running");

	/* run the main loop until shutdown */
	g_main_loop_run(self->mcp_loop);

	g_debug("gowl-mcp: thread exiting");

	g_main_context_pop_thread_default(self->mcp_context);

	return NULL;
}

/* ================================================================
 * GowlModule virtual methods
 * ================================================================ */

static gboolean
mcp_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
mcp_deactivate(GowlModule *mod)
{
	(void)mod;
	g_debug("gowl-mcp: deactivated");
}

static const gchar *
mcp_get_name(GowlModule *mod)
{
	(void)mod;
	return "mcp";
}

static const gchar *
mcp_get_description(GowlModule *mod)
{
	(void)mod;
	return "MCP (Model Context Protocol) server for AI assistant integration";
}

static const gchar *
mcp_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* ================================================================
 * GowlStartupHandler
 * ================================================================ */

/**
 * mcp_on_startup:
 * @handler: the module cast to GowlStartupHandler
 * @compositor: a gpointer to GowlCompositor
 *
 * Called when the compositor starts.  Initialises the dispatch
 * queue and spawns the MCP I/O thread.
 */
static void
mcp_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleMcp *self;

	self = GOWL_MODULE_MCP(handler);
	self->compositor = GOWL_COMPOSITOR(compositor);

	/* check if any transport is enabled */
	if (!self->transport_http && !self->transport_stdio) {
		g_debug("gowl-mcp: no transports enabled, module idle");
		return;
	}

	/* initialise the compositor dispatch queue (eventfd) */
	if (!gowl_mcp_dispatch_init(self)) {
		g_warning("gowl-mcp: failed to init dispatch, module disabled");
		return;
	}

	/* create MCP thread context and main loop */
	self->mcp_context = g_main_context_new();
	self->mcp_loop    = g_main_loop_new(self->mcp_context, FALSE);

	/* spawn the MCP I/O thread */
	self->mcp_thread = g_thread_new("gowl-mcp", mcp_thread_func, self);

	g_debug("gowl-mcp: startup complete (http=%s, stdio=%s)",
	        self->transport_http  ? "yes" : "no",
	        self->transport_stdio ? "yes" : "no");
}

static void
mcp_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = mcp_on_startup;
}

/* ================================================================
 * GowlShutdownHandler
 * ================================================================ */

/**
 * mcp_on_shutdown:
 * @handler: the module cast to GowlShutdownHandler
 * @compositor: a gpointer to GowlCompositor (unused)
 *
 * Called when the compositor shuts down.  Stops the MCP thread
 * and cleans up all servers, transports, and sessions.
 */
static void
mcp_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleMcp *self;

	(void)compositor;
	self = GOWL_MODULE_MCP(handler);

	/* stop the MCP thread if running */
	if (self->mcp_loop != NULL) {
		g_main_loop_quit(self->mcp_loop);

		if (self->mcp_thread != NULL) {
			g_thread_join(self->mcp_thread);
			self->mcp_thread = NULL;
		}

		g_main_loop_unref(self->mcp_loop);
		self->mcp_loop = NULL;
	}

	if (self->mcp_context != NULL) {
		g_main_context_unref(self->mcp_context);
		self->mcp_context = NULL;
	}

	/* clean up HTTP server */
	if (self->http_server != NULL) {
		mcp_server_stop(self->http_server);
		g_clear_object(&self->http_server);
	}
	g_clear_object(&self->http_transport);

	/* clean up socket server */
	if (self->socket_server != NULL) {
		mcp_unix_socket_server_stop(self->socket_server);
		g_clear_object(&self->socket_server);
	}

	/* shut down dispatch queue */
	gowl_mcp_dispatch_shutdown(self);

	g_debug("gowl-mcp: shutdown complete");
}

static void
mcp_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = mcp_on_shutdown;
}

/* ================================================================
 * GObject lifecycle
 * ================================================================ */

static void
gowl_module_mcp_finalize(GObject *object)
{
	GowlModuleMcp *self;

	self = GOWL_MODULE_MCP(object);

	g_free(self->http_host);
	g_free(self->http_auth_token);
	g_free(self->instructions);

	if (self->allowed_tools != NULL)
		g_hash_table_unref(self->allowed_tools);

	G_OBJECT_CLASS(gowl_module_mcp_parent_class)->finalize(object);
}

static void
gowl_module_mcp_class_init(GowlModuleMcpClass *klass)
{
	GObjectClass    *object_class;
	GowlModuleClass *mod_class;

	object_class = G_OBJECT_CLASS(klass);
	mod_class    = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_mcp_finalize;

	mod_class->activate        = mcp_activate;
	mod_class->deactivate      = mcp_deactivate;
	mod_class->get_name        = mcp_get_name;
	mod_class->get_description = mcp_get_description;
	mod_class->get_version     = mcp_get_version;
	mod_class->configure       = mcp_configure;
}

static void
gowl_module_mcp_init(GowlModuleMcp *self)
{
	self->compositor      = NULL;
	self->mcp_thread      = NULL;
	self->mcp_loop        = NULL;
	self->mcp_context     = NULL;
	self->http_server     = NULL;
	self->http_transport  = NULL;
	self->socket_server   = NULL;
	self->wake_fd         = -1;
	self->wake_source     = NULL;

	/* defaults */
	self->transport_http  = FALSE;
	self->transport_stdio = TRUE;
	self->http_host       = g_strdup("127.0.0.1");
	self->http_port       = 8716;
	self->http_auth_token = NULL;
	self->instructions    = NULL;
	self->allowed_tools   = NULL;  /* NULL = all allowed */
}

/* ================================================================
 * Module entry point
 * ================================================================ */

/**
 * gowl_module_register:
 *
 * Entry point called when the .so is loaded by GowlModuleManager.
 *
 * Returns: the GType of this module
 */
G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_MCP;
}
