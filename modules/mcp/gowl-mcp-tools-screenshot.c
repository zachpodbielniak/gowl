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
 * gowl-mcp-tools-screenshot.c - Screenshot MCP tools.
 *
 * Implements:
 *   - screenshot_client  : Capture a specific client window
 *   - screenshot_monitor : Capture an entire output
 *   - screenshot_region  : Capture an arbitrary rectangle
 *
 * Screenshots are returned as base64-encoded PNG via the MCP image
 * content type.  When the optional 'path' parameter is provided,
 * the PNG is written to disk and JSON metadata is returned instead.
 * Uses wlr_texture_read_pixels() for pixel readback and libpng
 * for encoding.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"

#include <json-glib/json-glib.h>
#include <drm_fourcc.h>
#include <png.h>
#include <string.h>

/* ========================================================================== */
/* PNG encoding                                                               */
/* ========================================================================== */

/**
 * PngWriteCtx:
 *
 * Context for writing PNG data to a dynamically-growing memory
 * buffer.  Used as the user pointer for png_set_write_fn().
 */
typedef struct {
	guint8   *data;
	gsize     len;
	gsize     alloc;
} PngWriteCtx;

/**
 * png_write_to_mem:
 *
 * libpng write callback that appends data to a PngWriteCtx buffer.
 */
static void
png_write_to_mem(
	png_structp  png,
	png_bytep    buf,
	png_size_t   len
){
	PngWriteCtx *ctx;

	ctx = (PngWriteCtx *)png_get_io_ptr(png);

	/* Grow buffer if needed */
	while (ctx->len + len > ctx->alloc) {
		ctx->alloc = (ctx->alloc == 0) ? 4096 : ctx->alloc * 2;
		ctx->data = (guint8 *)g_realloc(ctx->data, ctx->alloc);
	}

	memcpy(ctx->data + ctx->len, buf, len);
	ctx->len += len;
}

/**
 * png_flush_noop:
 *
 * No-op flush callback for libpng (writing to memory).
 */
static void
png_flush_noop(png_structp png)
{
	(void)png;
}

/**
 * encode_rgba_to_png:
 * @pixels: RGBA pixel data (8 bits per channel, row-major)
 * @width: image width in pixels
 * @height: image height in pixels
 * @stride: row stride in bytes
 * @out_data: (out): allocated PNG data (caller must g_free)
 * @out_len: (out): length of PNG data
 *
 * Encodes raw RGBA pixel data to an in-memory PNG file.
 * The pixel format is expected to be ARGB8888 (little-endian),
 * which is B, G, R, A in memory on x86 -- we convert to PNG's
 * expected RGBA order.
 *
 * Returns: %TRUE on success
 */
static gboolean
encode_rgba_to_png(
	const guint8  *pixels,
	gint           width,
	gint           height,
	gint           stride,
	guint8       **out_data,
	gsize         *out_len
){
	png_structp png;
	png_infop info;
	PngWriteCtx ctx;
	gint y;
	guint8 *row_buf;

	memset(&ctx, 0, sizeof(ctx));

	png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
		NULL, NULL, NULL);
	if (png == NULL)
		return FALSE;

	info = png_create_info_struct(png);
	if (info == NULL) {
		png_destroy_write_struct(&png, NULL);
		return FALSE;
	}

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		g_free(ctx.data);
		return FALSE;
	}

	png_set_write_fn(png, &ctx, png_write_to_mem, png_flush_noop);

	png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height,
		8, PNG_COLOR_TYPE_RGB_ALPHA,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);

	/* Use fastest compression for responsive screenshots */
	png_set_compression_level(png, 1);

	png_write_info(png, info);

	/*
	 * Convert from ARGB8888 (DRM format, little-endian on x86:
	 * memory order B, G, R, A) to PNG's RGBA order.
	 */
	row_buf = (guint8 *)g_malloc((gsize)width * 4);

	for (y = 0; y < height; y++) {
		const guint8 *src;
		gint x;

		src = pixels + (gsize)y * (gsize)stride;
		for (x = 0; x < width; x++) {
			guint8 b, g, r, a;

			/*
			 * DRM_FORMAT_ARGB8888 little-endian byte order:
			 * byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = A
			 */
			b = src[x * 4 + 0];
			g = src[x * 4 + 1];
			r = src[x * 4 + 2];
			a = src[x * 4 + 3];

			row_buf[x * 4 + 0] = r;
			row_buf[x * 4 + 1] = g;
			row_buf[x * 4 + 2] = b;
			row_buf[x * 4 + 3] = a;
		}

		png_write_row(png, row_buf);
	}

	g_free(row_buf);

	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);

	*out_data = ctx.data;
	*out_len = ctx.len;
	return TRUE;
}

/* ========================================================================== */
/* Helper: capture texture pixels                                             */
/* ========================================================================== */

/**
 * capture_texture_pixels:
 * @texture: the wlr_texture to read
 * @x: source X offset (for region crop)
 * @y: source Y offset (for region crop)
 * @width: capture width (0 = full texture width)
 * @height: capture height (0 = full texture height)
 * @out_pixels: (out): allocated pixel data (caller must g_free)
 * @out_width: (out): actual capture width
 * @out_height: (out): actual capture height
 * @out_stride: (out): row stride in bytes
 *
 * Reads pixels from a wlr_texture in ARGB8888 format.
 *
 * Returns: %TRUE on success
 */
static gboolean
capture_texture_pixels(
	struct wlr_texture *texture,
	gint                x,
	gint                y,
	gint                width,
	gint                height,
	guint8            **out_pixels,
	gint               *out_width,
	gint               *out_height,
	gint               *out_stride
){
	gint w, h, stride;
	guint8 *pixels;
	gint tex_w, tex_h;

	tex_w = (gint)texture->width;
	tex_h = (gint)texture->height;

	w = (width > 0) ? width : tex_w;
	h = (height > 0) ? height : tex_h;

	/* Clamp to texture bounds */
	if (x + w > tex_w)
		w = tex_w - x;
	if (y + h > tex_h)
		h = tex_h - y;
	if (w <= 0 || h <= 0)
		return FALSE;

	stride = w * 4; /* 4 bytes per pixel (ARGB8888) */
	pixels = (guint8 *)g_malloc0((gsize)stride * (gsize)h);

	/*
	 * Initialize the options struct in a single expression so
	 * the const src_box field can be set via designated init.
	 */
	{
		struct wlr_texture_read_pixels_options opts = {
			.data = pixels,
			.format = DRM_FORMAT_ARGB8888,
			.stride = (uint32_t)stride,
			.src_box = {
				.x = x, .y = y,
				.width = w, .height = h
			},
		};

		if (!wlr_texture_read_pixels(texture, &opts)) {
			g_free(pixels);
			return FALSE;
		}
	}

	*out_pixels = pixels;
	*out_width = w;
	*out_height = h;
	*out_stride = stride;
	return TRUE;
}

/**
 * pixels_to_image_result:
 * @pixels: ARGB8888 pixel data
 * @width: image width
 * @height: image height
 * @stride: row stride in bytes
 *
 * Encodes pixel data to PNG and returns an MCP tool result
 * containing the base64-encoded image.
 *
 * Returns: (transfer full): a new #McpToolResult
 */
static McpToolResult *
pixels_to_image_result(
	const guint8 *pixels,
	gint          width,
	gint          height,
	gint          stride
){
	McpToolResult *result;
	guint8 *png_data;
	gsize png_len;
	g_autofree gchar *b64;

	png_data = NULL;
	png_len = 0;

	if (!encode_rgba_to_png(pixels, width, height, stride,
	                        &png_data, &png_len))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Failed to encode PNG");
		return result;
	}

	b64 = g_base64_encode(png_data, png_len);
	g_free(png_data);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_image(result, b64, "image/png");
	return result;
}

/**
 * save_png_to_file:
 * @pixels: ARGB8888 pixel data
 * @width: image width
 * @height: image height
 * @stride: row stride in bytes
 * @path: destination file path
 *
 * Encodes pixels to PNG and writes to @path via g_file_set_contents().
 * Returns an McpToolResult -- either success JSON (path, bytes) or error.
 *
 * Returns: (transfer full): a new #McpToolResult
 */
static McpToolResult *
save_png_to_file(
	const guint8 *pixels,
	gint          width,
	gint          height,
	gint          stride,
	const gchar  *path
){
	McpToolResult *result;
	guint8 *png_data;
	gsize png_len;
	GError *error;

	png_data = NULL;
	png_len = 0;

	if (!encode_rgba_to_png(pixels, width, height, stride,
	                        &png_data, &png_len))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Failed to encode PNG");
		return result;
	}

	error = NULL;
	if (!g_file_set_contents(path, (const gchar *)png_data,
	                         (gssize)png_len, &error))
	{
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf("Failed to write %s: %s",
			path, error->message);
		g_error_free(error);
		g_free(png_data);

		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result, msg);
		return result;
	}

	g_free(png_data);

	/* Return JSON metadata */
	{
		g_autofree gchar *json = NULL;

		json = g_strdup_printf(
			"{\"success\": true, \"path\": \"%s\", \"bytes\": %"
			G_GSIZE_FORMAT "}", path, png_len);

		result = mcp_tool_result_new(FALSE);
		mcp_tool_result_add_text(result, json);
		return result;
	}
}

/* ========================================================================== */
/* Helper: find client by ID                                                  */
/* ========================================================================== */

static GowlClient *
find_client_by_id(
	GowlCompositor *compositor,
	guint           id
){
	GList *clients;
	GList *iter;

	clients = gowl_compositor_get_clients(compositor);
	for (iter = clients; iter != NULL; iter = iter->next) {
		GowlClient *c;

		c = GOWL_CLIENT(iter->data);
		if (gowl_client_get_id(c) == id)
			return c;
	}
	return NULL;
}

/* ========================================================================== */
/* Helper: find monitor by name                                               */
/* ========================================================================== */

static GowlMonitor *
resolve_monitor(
	GowlCompositor *compositor,
	const gchar    *name
){
	GList *monitors;
	GList *iter;

	monitors = gowl_compositor_get_monitors(compositor);
	if (name == NULL || *name == '\0') {
		/* Default to first monitor */
		return (monitors != NULL) ?
			GOWL_MONITOR(monitors->data) : NULL;
	}

	for (iter = monitors; iter != NULL; iter = iter->next) {
		GowlMonitor *mon;
		const gchar *mon_name;

		mon = GOWL_MONITOR(iter->data);
		mon_name = gowl_monitor_get_name(mon);
		if (mon_name != NULL && g_strcmp0(mon_name, name) == 0)
			return mon;
	}
	return NULL;
}

/* ========================================================================== */
/* Tool: screenshot_client                                                    */
/* ========================================================================== */

/**
 * tool_screenshot_client:
 *
 * Captures the current surface of a specific client window and
 * returns it as a base64-encoded PNG image.
 */
static McpToolResult *
tool_screenshot_client(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlClient *client;
	struct wlr_surface *surface;
	struct wlr_texture *texture;
	guint id;
	guint8 *pixels;
	gint w, h, stride;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "id"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameter: id");
		return r;
	}

	id = (guint)json_object_get_int_member(arguments, "id");
	client = find_client_by_id(module->compositor, id);
	if (client == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Client not found");
		return r;
	}

	/* Get the surface texture */
	surface = gowl_client_get_wlr_surface(client);
	if (surface == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Client has no surface (unmapped?)");
		return r;
	}

	texture = wlr_surface_get_texture(surface);
	if (texture == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Client surface has no texture");
		return r;
	}

	pixels = NULL;
	if (!capture_texture_pixels(texture, 0, 0, 0, 0,
	                            &pixels, &w, &h, &stride))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to read surface pixels");
		return r;
	}

	/* Check for optional save path */
	if (arguments != NULL &&
	    json_object_has_member(arguments, "path"))
	{
		const gchar *path;
		McpToolResult *r;

		path = json_object_get_string_member(arguments, "path");
		r = save_png_to_file(pixels, w, h, stride, path);
		g_free(pixels);
		return r;
	}

	/* Default: return base64 image */
	{
		McpToolResult *r;

		r = pixels_to_image_result(pixels, w, h, stride);
		g_free(pixels);
		return r;
	}
}

static McpToolResult *
handle_screenshot_client(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_screenshot_client, arguments, NULL);
}

/* ========================================================================== */
/* Tool: screenshot_monitor                                                   */
/* ========================================================================== */

/**
 * tool_screenshot_monitor:
 *
 * Captures the entire composited output of a monitor by building
 * an output state from the scene graph and reading back the
 * resulting buffer as pixels.
 */
static McpToolResult *
tool_screenshot_monitor(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlMonitor *mon;
	const gchar *mon_name;
	struct wlr_output_state state;
	struct wlr_texture *texture;
	struct wlr_renderer *renderer;
	guint8 *pixels;
	gint w, h, stride;

	mon_name = NULL;
	if (arguments != NULL &&
	    json_object_has_member(arguments, "monitor"))
	{
		mon_name = json_object_get_string_member(
			arguments, "monitor");
	}

	mon = resolve_monitor(module->compositor, mon_name);
	if (mon == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Monitor not found");
		return r;
	}

	renderer = gowl_compositor_get_wlr_renderer(module->compositor);

	/*
	 * Build the scene output state.  This renders the full scene
	 * for this output into a wlr_buffer in the output state.
	 */
	wlr_output_state_init(&state);
	if (!wlr_scene_output_build_state(mon->scene_output,
	                                  &state, NULL))
	{
		McpToolResult *r;

		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to render scene for output");
		return r;
	}

	if (state.buffer == NULL) {
		McpToolResult *r;

		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Scene render produced no buffer");
		return r;
	}

	/*
	 * Create a texture from the rendered buffer so we can
	 * read pixels from it.
	 */
	texture = wlr_texture_from_buffer(renderer, state.buffer);
	if (texture == NULL) {
		McpToolResult *r;

		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to create texture from output buffer");
		return r;
	}

	pixels = NULL;
	if (!capture_texture_pixels(texture, 0, 0, 0, 0,
	                            &pixels, &w, &h, &stride))
	{
		McpToolResult *r;

		wlr_texture_destroy(texture);
		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to read output pixels");
		return r;
	}

	wlr_texture_destroy(texture);
	wlr_output_state_finish(&state);

	/* Check for optional save path */
	if (arguments != NULL &&
	    json_object_has_member(arguments, "path"))
	{
		const gchar *path;
		McpToolResult *r;

		path = json_object_get_string_member(arguments, "path");
		r = save_png_to_file(pixels, w, h, stride, path);
		g_free(pixels);
		return r;
	}

	/* Default: return base64 image */
	{
		McpToolResult *r;

		r = pixels_to_image_result(pixels, w, h, stride);
		g_free(pixels);
		return r;
	}
}

static McpToolResult *
handle_screenshot_monitor(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_screenshot_monitor, arguments, NULL);
}

/* ========================================================================== */
/* Tool: screenshot_region                                                    */
/* ========================================================================== */

/**
 * tool_screenshot_region:
 *
 * Captures an arbitrary rectangular region from a monitor.
 * Renders the full scene, then crops to the requested region.
 */
static McpToolResult *
tool_screenshot_region(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	GowlMonitor *mon;
	const gchar *mon_name;
	struct wlr_output_state state;
	struct wlr_texture *texture;
	struct wlr_renderer *renderer;
	gint x, y, w, h;
	guint8 *pixels;
	gint out_w, out_h, out_stride;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "x") ||
	    !json_object_has_member(arguments, "y") ||
	    !json_object_has_member(arguments, "width") ||
	    !json_object_has_member(arguments, "height"))
	{
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Missing required parameters: "
			"x, y, width, height");
		return r;
	}

	x = (gint)json_object_get_int_member(arguments, "x");
	y = (gint)json_object_get_int_member(arguments, "y");
	w = (gint)json_object_get_int_member(arguments, "width");
	h = (gint)json_object_get_int_member(arguments, "height");

	if (w <= 0 || h <= 0) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Width and height must be positive");
		return r;
	}

	mon_name = NULL;
	if (json_object_has_member(arguments, "monitor"))
		mon_name = json_object_get_string_member(
			arguments, "monitor");

	mon = resolve_monitor(module->compositor, mon_name);
	if (mon == NULL) {
		McpToolResult *r;

		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r, "Monitor not found");
		return r;
	}

	renderer = gowl_compositor_get_wlr_renderer(module->compositor);

	/* Render the full scene for this output */
	wlr_output_state_init(&state);
	if (!wlr_scene_output_build_state(mon->scene_output,
	                                  &state, NULL))
	{
		McpToolResult *r;

		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to render scene for output");
		return r;
	}

	if (state.buffer == NULL) {
		McpToolResult *r;

		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Scene render produced no buffer");
		return r;
	}

	texture = wlr_texture_from_buffer(renderer, state.buffer);
	if (texture == NULL) {
		McpToolResult *r;

		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to create texture from output buffer");
		return r;
	}

	/* Capture only the requested region */
	pixels = NULL;
	if (!capture_texture_pixels(texture, x, y, w, h,
	                            &pixels, &out_w, &out_h, &out_stride))
	{
		McpToolResult *r;

		wlr_texture_destroy(texture);
		wlr_output_state_finish(&state);
		r = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(r,
			"Failed to read region pixels "
			"(region may be out of bounds)");
		return r;
	}

	wlr_texture_destroy(texture);
	wlr_output_state_finish(&state);

	/* Check for optional save path */
	if (arguments != NULL &&
	    json_object_has_member(arguments, "path"))
	{
		const gchar *path;
		McpToolResult *r;

		path = json_object_get_string_member(arguments, "path");
		r = save_png_to_file(pixels, out_w, out_h, out_stride, path);
		g_free(pixels);
		return r;
	}

	/* Default: return base64 image */
	{
		McpToolResult *r;

		r = pixels_to_image_result(pixels, out_w, out_h, out_stride);
		g_free(pixels);
		return r;
	}
}

static McpToolResult *
handle_screenshot_region(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_screenshot_region, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

void
gowl_mcp_register_screenshot_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* screenshot_client */
	if (gowl_module_mcp_is_tool_allowed(module,
	    "screenshot_client"))
	{
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("screenshot_client",
			"Capture a screenshot of a specific client window. "
			"Returns a base64-encoded PNG image. If 'path' is "
			"provided, saves to file instead.");
		mcp_tool_set_read_only_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "id");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Client window ID (from list_clients)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "path");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"File path to write the PNG to. "
			"If omitted, returns base64 image.");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "id");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool,
		                    handle_screenshot_client,
		                    module, NULL);
	}

	/* screenshot_monitor */
	if (gowl_module_mcp_is_tool_allowed(module,
	    "screenshot_monitor"))
	{
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("screenshot_monitor",
			"Capture a screenshot of an entire monitor/output. "
			"Returns a base64-encoded PNG image. If 'path' is "
			"provided, saves to file instead.");
		mcp_tool_set_read_only_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "monitor");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Output name (e.g. 'eDP-1', 'HDMI-A-1'). "
			"Default: first monitor.");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "path");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"File path to write the PNG to. "
			"If omitted, returns base64 image.");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool,
		                    handle_screenshot_monitor,
		                    module, NULL);
	}

	/* screenshot_region */
	if (gowl_module_mcp_is_tool_allowed(module,
	    "screenshot_region"))
	{
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("screenshot_region",
			"Capture a rectangular region from a monitor. "
			"Returns a base64-encoded PNG image. If 'path' is "
			"provided, saves to file instead.");
		mcp_tool_set_read_only_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "x");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"X offset of the capture region (pixels)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "y");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Y offset of the capture region (pixels)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "width");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Width of the capture region (pixels)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "height");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "integer");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Height of the capture region (pixels)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "monitor");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Output name (default: first monitor)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "path");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"File path to write the PNG to. "
			"If omitted, returns base64 image.");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "x");
		json_builder_add_string_value(b, "y");
		json_builder_add_string_value(b, "width");
		json_builder_add_string_value(b, "height");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool,
		                    handle_screenshot_region,
		                    module, NULL);
	}

	g_debug("screenshot tools registered");
}
