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
 * gowl-mcp-dispatch.c - Thread-safe compositor dispatch implementation.
 *
 * Provides the eventfd + request queue mechanism that allows MCP
 * tool handlers (running on the MCP thread) to safely execute code
 * on the compositor thread (Wayland event loop).
 *
 * Flow:
 *   1. MCP thread creates a GowlMcpRequest, enqueues it, writes to eventfd
 *   2. Compositor thread wakes on eventfd readable, dequeues request
 *   3. Compositor thread calls the tool function with full state access
 *   4. Compositor thread stores result, signals GCond
 *   5. MCP thread wakes from g_cond_wait, returns the result
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>
#include <wayland-server-core.h>

/**
 * dispatch_wake_handler:
 * @fd: the eventfd file descriptor
 * @mask: the event mask (WL_EVENT_READABLE)
 * @data: user data (GowlModuleMcp*)
 *
 * Called on the compositor thread when the eventfd is readable.
 * Drains the eventfd and processes all pending requests in the queue.
 *
 * Returns: 0 to keep the event source active
 */
static int
dispatch_wake_handler(
	int      fd,
	uint32_t mask,
	void    *data
){
	GowlModuleMcp  *module;
	guint64          val;
	ssize_t          ret;
	GList           *batch;
	GList           *l;

	module = (GowlModuleMcp *)data;

	/* drain the eventfd */
	ret = read(fd, &val, sizeof(val));
	(void)ret;

	/* grab all pending requests under the lock */
	g_mutex_lock(&module->queue_mutex);
	batch = module->pending_requests.head;
	module->pending_requests.head = NULL;
	module->pending_requests.tail = NULL;
	module->pending_requests.length = 0;
	g_mutex_unlock(&module->queue_mutex);

	/* process each request on the compositor thread */
	for (l = batch; l != NULL; l = l->next) {
		GowlMcpRequest *req;

		req = (GowlMcpRequest *)l->data;

		/* execute the tool function with compositor access */
		req->result = req->func(module, req->arguments, req->user_data);

		/* signal the waiting MCP thread */
		g_mutex_lock(&req->mutex);
		req->done = TRUE;
		g_cond_signal(&req->cond);
		g_mutex_unlock(&req->mutex);
	}

	g_list_free(batch);

	return 0;
}

gboolean
gowl_mcp_dispatch_init(GowlModuleMcp *module)
{
	struct wl_event_loop *event_loop;

	g_return_val_if_fail(module != NULL, FALSE);
	g_return_val_if_fail(module->compositor != NULL, FALSE);

	g_mutex_init(&module->queue_mutex);
	g_queue_init(&module->pending_requests);

	/* create eventfd for cross-thread wakeup */
	module->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (module->wake_fd < 0) {
		g_warning("gowl-mcp: failed to create eventfd: %s",
		          g_strerror(errno));
		return FALSE;
	}

	/* register the eventfd with the Wayland event loop */
	event_loop = gowl_compositor_get_event_loop(module->compositor);

	module->wake_source = wl_event_loop_add_fd(
		event_loop,
		module->wake_fd,
		WL_EVENT_READABLE,
		dispatch_wake_handler,
		module
	);

	if (module->wake_source == NULL) {
		g_warning("gowl-mcp: failed to add eventfd to wl_event_loop");
		close(module->wake_fd);
		module->wake_fd = -1;
		return FALSE;
	}

	g_debug("gowl-mcp: dispatch queue initialised (eventfd=%d)",
	        module->wake_fd);
	return TRUE;
}

void
gowl_mcp_dispatch_shutdown(GowlModuleMcp *module)
{
	g_return_if_fail(module != NULL);

	/* remove from wayland event loop */
	if (module->wake_source != NULL) {
		wl_event_source_remove(module->wake_source);
		module->wake_source = NULL;
	}

	/* drain any remaining requests with an error */
	g_mutex_lock(&module->queue_mutex);
	while (!g_queue_is_empty(&module->pending_requests)) {
		GowlMcpRequest *req;

		req = (GowlMcpRequest *)g_queue_pop_head(&module->pending_requests);

		req->result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(req->result, "MCP module is shutting down");

		g_mutex_lock(&req->mutex);
		req->done = TRUE;
		g_cond_signal(&req->cond);
		g_mutex_unlock(&req->mutex);
	}
	g_mutex_unlock(&module->queue_mutex);

	/* close the eventfd */
	if (module->wake_fd >= 0) {
		close(module->wake_fd);
		module->wake_fd = -1;
	}

	g_mutex_clear(&module->queue_mutex);

	g_debug("gowl-mcp: dispatch queue shut down");
}

McpToolResult *
gowl_mcp_dispatch_call(
	GowlModuleMcp   *module,
	GowlMcpToolFunc  func,
	JsonObject       *arguments,
	gpointer          user_data
){
	GowlMcpRequest req;
	guint64        val;
	ssize_t        ret;

	g_return_val_if_fail(module != NULL, NULL);
	g_return_val_if_fail(func != NULL, NULL);

	/* initialise the request on the stack (MCP thread blocks until done) */
	g_mutex_init(&req.mutex);
	g_cond_init(&req.cond);
	req.done      = FALSE;
	req.func      = func;
	req.arguments = arguments;
	req.user_data = user_data;
	req.result    = NULL;

	/* enqueue and wake the compositor thread */
	g_mutex_lock(&module->queue_mutex);
	g_queue_push_tail(&module->pending_requests, &req);
	g_mutex_unlock(&module->queue_mutex);

	val = 1;
	ret = write(module->wake_fd, &val, sizeof(val));
	(void)ret;

	/* block until the compositor thread processes our request */
	g_mutex_lock(&req.mutex);
	while (!req.done)
		g_cond_wait(&req.cond, &req.mutex);
	g_mutex_unlock(&req.mutex);

	g_mutex_clear(&req.mutex);
	g_cond_clear(&req.cond);

	return req.result;
}
