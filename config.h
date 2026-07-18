#ifndef DWLBG_CONFIG_H
#define DWLBG_CONFIG_H

#include <stdbool.h>
#include <stdio.h>
#include <cairo.h>

#include "dwlbg.h"

struct dwlbg_output_config {
	struct wl_list link;  // dwlbg_state::output_configs

	char * name;
	char * image_path;

	cairo_filter_t filter;

	enum {
		SCALING_MODE_FILL,
		SCALING_MODE_STRETCH,
		SCALING_MODE_TILE,
	} scaling_mode;

	enum {  // Bitmask, top/bottom and left/right are mutually exclusive.
		ANCHOR_CENTER = 0,
		ANCHOR_TOP = 1,
		ANCHOR_LEFT = 2,
		ANCHOR_BOTTOM = 4,
		ANCHOR_RIGHT = 8,
	} anchor;
};


struct dwlbg_output_config * dwlbg_output_config_create(
		struct dwlbg_state * dwlbg, const char * output_name);
void dwlbg_output_config_destroy(struct dwlbg_output_config * opc);

typedef bool dwlbg_configurator_t(struct dwlbg_state *, char *, char *, char *);
dwlbg_configurator_t * configurator_from_string(const char * name);

int load_config_file(struct dwlbg_state * dwlbg, const char * path);
int load_config(struct dwlbg_state * dwlbg, FILE * config_file,
		const char * filename);

#endif
