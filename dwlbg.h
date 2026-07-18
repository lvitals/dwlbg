#ifndef DWLBG_H
#define DWLBG_H

#include <poll.h>
#include <sys/un.h>
#include <stdbool.h>
#include <wayland-client.h>

// Maximum number of events we can keep track of, including both the reserved
// slots below and any active animations.
#define DWLBG_EVENT_COUNT 25

// These are used to reserve a few pollfd slots for static stuff.
enum dwlbg_events {
	DWLBG_SIGNAL_EVENT,
	DWLBG_WAYLAND_EVENT,
	DWLBG_IPC_CONNECT_EVENT,
	DWLBG_IPC_CLIENT_EVENT,
	DWLBG_FIRST_ANIM_EVENT,  // last
};

struct dwlbg_state {
	bool run;
	struct pollfd events[DWLBG_EVENT_COUNT];

	struct wl_display * display;
	struct wl_registry * registry;
	struct wl_compositor * compositor;
	struct wl_shm * shm;

	struct zwlr_layer_shell_v1 * layer_shell;
	struct zxdg_output_manager_v1 * output_manager;

	struct sockaddr_un ipc_sock;

	struct wl_list output_configs;  // dwlbg_output_config::link
	struct wl_list idle_outputs;  // dwlbg_output::link
	struct wl_list animations;  // dwlbg_animation::link
};

void dwlbg_reconfigure(struct dwlbg_state * dwlbg);

#endif
