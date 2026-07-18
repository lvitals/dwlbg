#ifndef DWLBG_OUTPUT_H
#define DWLBG_OUTPUT_H

#include <wayland-client.h>
#include <cairo.h>

#include "config.h"

struct dwlbg_state;

struct dwlbg_output {
	struct dwlbg_state * dwlbg;
	struct dwlbg_output_config * config;
	struct wl_list link;  // dwlbg_state::outputs

	char * name;
	struct wl_output * output;

	struct wl_surface * surface;
	struct zwlr_layer_surface_v1 * layer_surface;
	struct zxdg_output_v1 * xdg_output;

	uint32_t width;
	uint32_t height;
	int32_t scale;

	unsigned int cached_frames;
	struct wl_list buffer_ring;  // dwlbg_buffer::link
	unsigned int buffer_count;
};

struct dwlbg_output * dwlbg_output_create(
		struct dwlbg_state * dwlbg, struct wl_output * wl_output);
void dwlbg_output_destroy(struct dwlbg_output * output);

#endif
