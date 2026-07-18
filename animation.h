#ifndef DWLBG_ANIMATION_H
#define DWLBG_ANIMATION_H

#include <poll.h>
#include <cairo.h>
#include <wayland-client.h>

#include "cairo-pixbuf.h"
#include "config.h"

struct dwlbg_state;

struct dwlbg_animation {
	struct dwlbg_state * dwlbg;
	struct wl_list link;

	// In order to update our timer for each frame, we keep a reference to the
	// timerfd that we placed into dwlbg::events.
	int timerfd;
	int event_index;

	char * path;
	GdkPixbufAnimation * image;
	GdkPixbufAnimationIter * frame_iter;
	cairo_surface_t * source_surface;

	bool first_cycle;
	unsigned int frame_count;

	struct wl_list outputs;  // dwlbg_output::link
};

int dwlbg_render_frame(struct dwlbg_animation * anim);
bool dwlbg_animation_schedule_frame(
		struct dwlbg_animation * anim, unsigned int delay);
struct dwlbg_animation * dwlbg_animation_create(
		struct dwlbg_state * dwlbg, char * image_path);
void dwlbg_animation_destroy(struct dwlbg_animation * anim);

#endif
