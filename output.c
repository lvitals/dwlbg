#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "dwlbg.h"
#include "animation.h"
#include "output.h"
#include "buffers.h"

static void noop() {}  // For unused listener members.

//
// Wayland outputs
//

static void dwlbg_recreate_buffers(struct dwlbg_output * output) {
	if (!output->width || !output->height) {
		// Probably in the middle of reconfiguring, will try again later.
		return;
	}

	output->cached_frames = 0;

	struct dwlbg_buffer * buffer, * tmp;
	wl_list_for_each_safe(buffer, tmp, &output->buffer_ring, link) {
		dwlbg_buffer_destroy(buffer);
	}
	wl_list_init(&output->buffer_ring);

	// Keep the existing ring size across output changes. New outputs start with
	// two buffers; static wallpapers are shrunk to one by the render path.

	unsigned int count = (output->buffer_count) ? output->buffer_count : 2;
	output->buffer_count = 0;

	if (!dwlbg_allocate_buffers(output, count)) {
		fprintf(stderr, "Could not allocate buffers!\n");
		output->dwlbg->run = false;
	}
}

static void handle_output_scale(
		void * data,
		struct wl_output * wl_output __attribute__((unused)),
		int32_t factor) {
	struct dwlbg_output * output = data;
	output->scale = factor;
}

static void handle_output_done(
		void * data,
		struct wl_output * wl_output __attribute__((unused))) {
	struct dwlbg_output * output = data;
	dwlbg_recreate_buffers(output);
}

struct wl_output_listener output_listener = {
	.scale = handle_output_scale,
	.geometry = noop,
	.mode = noop,
	.done = handle_output_done,
};

//
// wlroots layer surface
//

static void layer_surface_configure(
		void * data,
		struct zwlr_layer_surface_v1 * layer_surface,
		uint32_t serial,
		uint32_t width,
		uint32_t height) {
	struct dwlbg_output * output = data;

	output->width = width;
	output->height = height;

	// The entire surface can be marked opaque, as it should be the lowest
	// z-index on the display.
	struct wl_region * opaque = wl_compositor_create_region(
			output->dwlbg->compositor);
	wl_region_add(opaque, 0, 0, output->width, output->height);
	wl_surface_set_opaque_region(output->surface, opaque);
	wl_region_destroy(opaque);

	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	dwlbg_recreate_buffers(output);
}

static void layer_surface_closed(
		void * data,
		struct zwlr_layer_surface_v1 * layer_surface __attribute__((unused))) {
	struct dwlbg_output * output = data;
	dwlbg_output_destroy(output);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

//
// XDG Output Manager
//

static void handle_xdg_output_name(
		void * data,
		struct zxdg_output_v1 * xdg_output __attribute__((unused)),
		const char *name) {
	struct dwlbg_output * output = (struct dwlbg_output *)data;
	free(output->name);
	output->name = strdup(name);
}

static void handle_xdg_output_done(
		void * data,
		struct zxdg_output_v1 * xdg_output) {
	struct dwlbg_output * output = (struct dwlbg_output *)data;

	// We have no further use for this object.
	zxdg_output_v1_destroy(xdg_output);
	output->xdg_output = NULL;
}

struct zxdg_output_v1_listener xdg_output_listener = {
	.name = handle_xdg_output_name,
	.done = handle_xdg_output_done,
	.logical_position = noop,
	.logical_size = noop,
	.description = noop,
};

//
// Outputs
//

struct dwlbg_output * dwlbg_output_create(
		struct dwlbg_state * dwlbg, struct wl_output * wl_output) {
	struct dwlbg_output * output = calloc(1, sizeof(struct dwlbg_output));
	if (!output) {
		fprintf(stderr, "Failed to allocate output\n");
		wl_output_destroy(wl_output);
		return NULL;
	}
	output->dwlbg = dwlbg;
	wl_list_init(&output->link);
	wl_list_init(&output->buffer_ring);

	output->output = wl_output;
	wl_output_add_listener(wl_output, &output_listener, output);

	output->surface = wl_compositor_create_surface(dwlbg->compositor);

	if (!output->surface) {
		fprintf(stderr, "Couldn't create surface for output!");
		dwlbg_output_destroy(output);
		return NULL;
	}

	struct wl_region * input_region = wl_compositor_create_region(
			dwlbg->compositor);

	if (!input_region) {
		fprintf(stderr, "Couldn't create input region for output!");
		dwlbg_output_destroy(output);
		return NULL;
	}

	wl_surface_set_input_region(output->surface, input_region);
	wl_region_destroy(input_region);

	output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			dwlbg->layer_shell,
			output->surface,
			output->output,
			ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
			"dwlbg");

	if (!output->layer_surface) {
		fprintf(stderr, "Couldn't create layer surface for output!");
		dwlbg_output_destroy(output);
		return NULL;
	}

	// xdg-output support is optional, so we need to check for it.
	if (dwlbg->output_manager) {
		output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
				dwlbg->output_manager, output->output);
		zxdg_output_v1_add_listener(
				output->xdg_output, &xdg_output_listener, output);
	}
	else {
		// TODO: Need to assign name as str of index and manually associate.
	}

	zwlr_layer_surface_v1_set_size(output->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(output->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(output->layer_surface,
			&layer_surface_listener, output);
	wl_surface_commit(output->surface);

	// Get the rest of the output properties. We need the name to associate
	// this output with an dwlbg_output_config, and dwlbg_reconfigure assumes
	// it is already present.
	wl_display_roundtrip(dwlbg->display);

	wl_list_insert(dwlbg->idle_outputs.prev, &output->link);
	dwlbg_reconfigure(dwlbg);

	return output;
}

void dwlbg_output_destroy(struct dwlbg_output * output) {
	wl_list_remove(&output->link);

	free(output->name);

	if (output->xdg_output) {
		zxdg_output_v1_destroy(output->xdg_output);
	}
	if (output->layer_surface) {
		zwlr_layer_surface_v1_destroy(output->layer_surface);
	}
	if (output->surface) {
		wl_surface_destroy(output->surface);
	}

	struct dwlbg_buffer * buffer, * tmp;
	wl_list_for_each_safe(buffer, tmp, &output->buffer_ring, link) {
		dwlbg_buffer_destroy(buffer);
	}

	wl_output_destroy(output->output);
	free(output);
}
