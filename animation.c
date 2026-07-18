#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include "dwlbg.h"
#include "buffers.h"
#include "output.h"
#include "animation.h"

static bool set_timer_milliseconds(int timer_fd, unsigned int delay) {
	struct itimerspec spec = {
		.it_value = (struct timespec) {
			.tv_sec = delay / 1000,
			.tv_nsec = (delay % 1000) * (long)1000000,
		},
	};
	int ret = timerfd_settime(timer_fd, 0, &spec, NULL);
	if (ret < 0) {
		fprintf(stderr, "Timer error (fd %d): %s\n", timer_fd, strerror(errno));
		return false;
	}

	return true;
}

static void scale_image_onto(
		cairo_t * cairo,
		cairo_surface_t * source,
		struct dwlbg_output * output) {
	// TODO: Store scaled width/height on buffer so we only need to pass config
	int32_t buffer_width = output->width * output->scale;
	int32_t buffer_height = output->height * output->scale;
	int anchor = output->config->anchor;
	cairo_filter_t filter = output->config->filter;

	double width = cairo_image_surface_get_width(source);
	double height = cairo_image_surface_get_height(source);

	double window_ratio = (double)buffer_width / buffer_height;
	double bg_ratio = width / height;

	cairo_save(cairo);

	cairo_matrix_t matrix;
	cairo_matrix_init_identity(&matrix);
	cairo_pattern_t * pattern = cairo_pattern_create_for_surface(source);

	double scale_x = 0.0;
	double scale_y = 0.0;
	double offset_x = 0.0;
	double offset_y = 0.0;

	switch (output->config->scaling_mode) {
	case SCALING_MODE_FILL:
		if (window_ratio > bg_ratio) {
			scale_x = scale_y = (double)buffer_width / width;

			if (anchor & ANCHOR_TOP) {
				offset_y = 0.0;
			}
			else if (anchor & ANCHOR_BOTTOM) {
				offset_y = ((double)buffer_height / scale_y) - height;
			}
			else {  // ANCHOR_CENTER
				offset_y = ((double)buffer_height / 2 / scale_y) - (height / 2);
			}
		} else {
			scale_x = scale_y = (double)buffer_height / height;

			if (anchor & ANCHOR_LEFT) {
				offset_x = 0.0;
			}
			else if (anchor & ANCHOR_RIGHT) {
				offset_x = ((double)buffer_width / scale_x) - width;
			}
			else {  // ANCHOR_CENTER
				offset_x = ((double)buffer_width / 2 / scale_x) - (width / 2);
			}
		}
		break;
	case SCALING_MODE_STRETCH:
		scale_x = (double)buffer_width / width;
		scale_y = (double)buffer_height / height;
		break;
	case SCALING_MODE_TILE:
		scale_x = scale_y = (double)output->scale;
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

		if (anchor & ANCHOR_LEFT) {
			offset_x = 0.0;
		}
		else if (anchor & ANCHOR_RIGHT) {
			offset_x = ((double)buffer_width / scale_x) - width;
		}
		else {  // ANCHOR_CENTER
			offset_x = ((double)buffer_width / 2 / scale_x) - (width / 2);
		}

		if (anchor & ANCHOR_TOP) {
			offset_y = 0.0;
		}
		else if (anchor & ANCHOR_BOTTOM) {
			offset_y = ((double)buffer_height / scale_y) - height;
		}
		else {  // ANCHOR_CENTER
			offset_y = ((double)buffer_height / 2 / scale_y) - (height / 2);
		}

		break;
	}

	cairo_matrix_translate(&matrix, -offset_x, -offset_y);
	cairo_matrix_scale(&matrix, 1 / scale_x, 1 / scale_y);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_pattern_set_filter(pattern, filter);
	cairo_set_source(cairo, pattern);
	cairo_paint(cairo);
	cairo_pattern_destroy(pattern);

	cairo_restore(cairo);
}

int dwlbg_render_frame(struct dwlbg_animation * anim) {
	bool advanced = gdk_pixbuf_animation_iter_advance(anim->frame_iter, NULL);
	GdkPixbuf * image = gdk_pixbuf_animation_iter_get_pixbuf(anim->frame_iter);

	// If we've got another frame to display, update our timer. Note that while
	// it isn't documented, the various implementations of this function take
	// into account the elapsed time of the current frame. This means we can
	// safely schedule frames early to redraw when new outputs appear without
	// worrying about the timing of the next frame being wrong.
	int delay = gdk_pixbuf_animation_iter_get_delay_time(anim->frame_iter);
	dwlbg_animation_schedule_frame(anim, delay);

	// Only count a frame if we actually advanced. If a new output was added,
	// we may have been called early to render the previous frame again.
	if (advanced && anim->first_cycle) {
		++anim->frame_count;
	}

	// It's important to set this _after_ we increment the frame_count for the
	// final time.
	bool last_frame = gdk_pixbuf_animation_iter_on_currently_loading_frame(
			anim->frame_iter);
	if (last_frame) {
		anim->first_cycle = false;
	}

	struct dwlbg_output * output;
	wl_list_for_each(output, &anim->outputs, link) {
		struct dwlbg_buffer * buffer = dwlbg_next_buffer(output);

		if (output->cached_frames < anim->frame_count) {
			if (!anim->first_cycle) {
				// When we're past the first cycle, we want to have as many
				// buffers as the animation has frames, because then we can
				// keep each resized frame in a buffer instead of re-scaling it
				// each time. However, we don't know which frame we started on,
				// so just attempt to resize every frame until we've cached
				// them all.
				if (!dwlbg_allocate_buffers(output, anim->frame_count)) {
					// TODO: This will freeze us at the current frame, probably
					// should quit instead.
					fprintf(stderr, "Unable to allocate %d frame buffers\n",
							anim->frame_count);
					return -1;
				}
			}

			// Draw the frame into our source surface, at its native size.
			dwlbg_cairo_surface_paint_pixbuf(anim->source_surface, image);

			// Then scale it into the buffer.
			scale_image_onto(buffer->cairo, anim->source_surface, output);

			wl_surface_set_buffer_scale(output->surface, output->scale);

			if (!anim->first_cycle) {
				// We count the number of frames we've cached to know when
				// we've cached them all, even if we didn't start at the first
				// frame of the animation. On the first cycle, however, we
				// don't want to cache because we don't know how many there
				// will be (or if the animation is even finite, technically).
				++output->cached_frames;
			}
		}

		// TODO: This should mark the buffer as busy, but we're not actually
		// checking for that anyway.
		wl_surface_attach(output->surface, buffer->backing, 0, 0);
		wl_surface_damage(output->surface, 0, 0, output->width, output->height);
		wl_surface_commit(output->surface);
	}

	return delay;
}

bool dwlbg_animation_schedule_frame(
		struct dwlbg_animation * anim, unsigned int delay) {
	if (delay > 0) {
		return set_timer_milliseconds(anim->timerfd, (unsigned int)delay);
	}
	else {
		return true;
	}
}

struct dwlbg_animation * dwlbg_animation_create(
		struct dwlbg_state * dwlbg, char * image_path) {
	int event_index = -1;
	for (size_t i = DWLBG_FIRST_ANIM_EVENT; i < DWLBG_EVENT_COUNT; ++i) {
		if (dwlbg->events[i].fd == -1) {
			event_index = i;
			break;
		}
	}

	if (event_index == -1) {
		fprintf(stderr, "No event slots available, too many animations!\n");
		return NULL;
	}

	GError * error = NULL;
	GdkPixbufAnimation * image = gdk_pixbuf_animation_new_from_file(
			image_path, &error);

	if (error || !image) {
		fprintf(stderr, "Could not open image '%s': %s\n", image_path,
				error ? error->message : "unknown error");
		if (image) {
			g_object_unref(image);
		}
		if (error) {
			g_error_free(error);
		}
		return NULL;
	}

	struct dwlbg_animation * anim = calloc(1, sizeof(struct dwlbg_animation));
	if (!anim) {
		fprintf(stderr, "Failed to allocate animation\n");
		g_object_unref(image);
		return NULL;
	}
	wl_list_init(&anim->outputs);

	anim->dwlbg = dwlbg;
	anim->path = strdup(image_path);
	if (!anim->path) {
		fprintf(stderr, "Failed to allocate animation path\n");
		g_object_unref(image);
		free(anim);
		return NULL;
	}
	anim->image = image;
	anim->frame_iter = gdk_pixbuf_animation_get_iter(image, NULL);
	if (!anim->frame_iter) {
		fprintf(stderr, "Failed to create animation iterator\n");
		g_object_unref(image);
		free(anim->path);
		free(anim);
		return NULL;
	}

	// There's no way to directly ask for the number of frames in an animation,
	// because gdk-pixbuf is designed to work with possibly streaming sources.
	// However, when loading from a file, the final frame is always marked as
	// the "loading frame". We use this, in combination with the following
	// flag, to count the frames ourselves. Once we know how many there are, we
	// can start caching the scaled buffers instead of rescaling each time we
	// draw. We have to track the total length so we don't allocate an infinite
	// number of buffers.
	anim->first_cycle = true;

	// The first frame drawn does not advance, so we can't count it. Instead,
	// just start at 1.
	anim->frame_count = 1;

	// We're going to make the wild assumption that every frame in the
	// animation has the same number of channels.
	GdkPixbuf * first = gdk_pixbuf_animation_iter_get_pixbuf(anim->frame_iter);
	int channel_count = gdk_pixbuf_get_n_channels(first);

	// We need a cairo surface of the image's size to draw each frame into
	// while scaling them up. This is as good a place for it as any.
	anim->source_surface = cairo_image_surface_create(
			(channel_count == 3) ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32,
			gdk_pixbuf_animation_get_width(image),
			gdk_pixbuf_animation_get_height(image));
	if (cairo_surface_status(anim->source_surface) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "Failed to create animation source surface\n");
		cairo_surface_destroy(anim->source_surface);
		g_object_unref(anim->frame_iter);
		g_object_unref(image);
		free(anim->path);
		free(anim);
		return NULL;
	}

	anim->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (anim->timerfd < 0) {
		fprintf(stderr, "Failed to create animation timer: %s\n", strerror(errno));
		cairo_surface_destroy(anim->source_surface);
		g_object_unref(anim->frame_iter);
		g_object_unref(image);
		free(anim->path);
		free(anim);
		return NULL;
	}

	dwlbg->events[event_index] = (struct pollfd) {
		.fd = anim->timerfd,
		.events = POLLIN,
	};
	anim->event_index = event_index;

	if (!dwlbg_animation_schedule_frame(anim, 1)) {  // Show first frame ASAP.
		fprintf(stderr, "Unable to schedule first timer\n");
	}

	wl_list_insert(dwlbg->animations.prev, &anim->link);
	return anim;
}

void dwlbg_animation_destroy(struct dwlbg_animation * anim) {
	wl_list_remove(&anim->link);

	// Disable the pollfd entry so that another animation can reuse it later.
	close(anim->timerfd);
	anim->dwlbg->events[anim->event_index] = (struct pollfd) {
		.fd = -1,
		.events = 0,  // Not strictly necessary, but eases debugging.
	};

	cairo_surface_destroy(anim->source_surface);
	g_object_unref(anim->image);
	g_object_unref(anim->frame_iter);
	free(anim->path);

	// Put all of the associated outputs back into the idle list, in case we
	// want to reassign them to a new animation later. Destroying them doesn't
	// happen until they are removed from the display, or we are told to exit.
	wl_list_insert_list(&anim->dwlbg->idle_outputs, &anim->outputs);

	anim->dwlbg = NULL;
	free(anim);
}
