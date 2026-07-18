#ifndef DWLBG_BUFFERS_H
#define DWLBG_BUFFERS_H

#define CAIRO_FMT CAIRO_FORMAT_ARGB32

#include <cairo.h>
#include <wayland-client.h>

#include "output.h"

struct dwlbg_buffer {
	struct wl_list link;  // dwlbg_output::buffer_ring;

	bool busy;

	struct wl_buffer * backing;
	cairo_t * cairo;
	cairo_surface_t * cairo_surface;

	void * data;
	size_t size;
};

struct dwlbg_buffer * dwlbg_allocate_buffer(struct dwlbg_output * output);
bool dwlbg_allocate_buffers(struct dwlbg_output * output, unsigned int count);
struct dwlbg_buffer * dwlbg_next_buffer(struct dwlbg_output * output);
void dwlbg_buffer_destroy(struct dwlbg_buffer * buffer);

#endif
