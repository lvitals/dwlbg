// dwlbg - animated and static wallpapers for dwl.
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cairo.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "dwlbg.h"
#include "animation.h"
#include "config.h"
#include "output.h"

//
// Signal handler
//

static int signal_pipe[2];
void signal_handler(int number) {
	if (write(signal_pipe[1], &number, sizeof(number)) < 0) {
		// Ignoring the error, there's nothing to be done about it in here.
		// Need to look at it to placate gcc, though.
		// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66425
	}
}

//
// Wayland registry
//

static void noop() {}  // For unused listener members.

static void handle_registry(
		void * data,
		struct wl_registry * registry,
		uint32_t name,
		const char * interface,
		uint32_t version __attribute__((unused))) {
	struct dwlbg_state * dwlbg = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		dwlbg->compositor = wl_registry_bind(
				registry, name, &wl_compositor_interface, 3);
	}
	else if (strcmp(interface, wl_shm_interface.name) == 0) {
		dwlbg->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	}
	else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output * output = wl_registry_bind(
				registry, name, &wl_output_interface, 3);
		dwlbg_output_create(dwlbg, output);
	}
	else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		dwlbg->output_manager = wl_registry_bind(
				registry, name, &zxdg_output_manager_v1_interface, 2);
	}
	else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		dwlbg->layer_shell = wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, 1);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_registry,
	.global_remove = noop,
};

//
// IPC
//

static int dwlbg_ipc_create(struct dwlbg_state * dwlbg) {
	int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd == -1) {
		perror("Unable to create IPC socket, IPC is disabled");
		return -1;
	}
	if (fcntl(sock_fd, F_SETFL, O_NONBLOCK) == -1) {
		perror("Unable to set nonblocking, IPC is disabled");
		close(sock_fd);
		return -1;
	}

	dwlbg->ipc_sock.sun_family = AF_UNIX;
	int path_size = sizeof(dwlbg->ipc_sock.sun_path);

	// TODO: Configurable ipc path
	const char * runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) {
		runtime = "/tmp";
	}
	if (path_size <= snprintf(
				dwlbg->ipc_sock.sun_path, path_size, "%s/dwlbg", runtime)) {
		fprintf(stderr, "Socket path is too long, IPC is disabled\n");
		close(sock_fd);
		return -1;
	}

	unlink(dwlbg->ipc_sock.sun_path);
	if (bind(sock_fd, (struct sockaddr *)&dwlbg->ipc_sock,
				sizeof(dwlbg->ipc_sock)) == -1) {
		perror("Unable to bind IPC socket, IPC is disabled");
		close(sock_fd);
		return -1;
	}

	if (listen(sock_fd, 1) == -1) {
		perror("Unable to listen on IPC socket, IPC is disabled");
		close(sock_fd);
		return -1;
	}

	return sock_fd;
}

static void dwlbg_ipc_destroy(struct dwlbg_state * dwlbg) {
	if (dwlbg->events[DWLBG_IPC_CONNECT_EVENT].fd >= 0) {
		close(dwlbg->events[DWLBG_IPC_CONNECT_EVENT].fd);
	}
	unlink(dwlbg->ipc_sock.sun_path);
	// TODO: Gracefully disconnect a client if one exists.
}

static void dwlbg_ipc_handle_command(
		struct dwlbg_state * dwlbg, const int client) {
	// Duplicate the IPC descriptor before attempting to load config from it
	// because load_config closes the descriptor when it's done.
	int config_fd = dup(client);
	if (config_fd < 0) {
		perror("Could not duplicate IPC file descriptor");
		if (write(client, "Unable to read config from IPC\n", 32) < 0) {
			fprintf(stderr, "Error replying to ipc command\n");
		}
		close(client);
		return;
	}

	FILE * ipc_config = fdopen(config_fd, "r");
	if (!ipc_config) {
		perror("Could not open IPC config stream");
		close(config_fd);
		close(client);
		return;
	}
	int loaded = load_config(dwlbg, ipc_config, "ipc");
	if (loaded == -1) {
		// TODO: Expose the error messages instead of writing them to dwlbg's
		// stderr, where they will go nowhere.
		if (write(client, "Invalid configuration\n", 23) < 0) {
			fprintf(stderr, "Error replying to ipc command\n");
		}
	}

	// TODO: If there was an error reading the config, we might have partially
	// applied it. We're going to reconfig so that nothing gets out of sync
	// internally, but this should be fixed in the config handlers.
	dwlbg_reconfigure(dwlbg);

	close(client);
	dwlbg->events[DWLBG_IPC_CLIENT_EVENT].fd = -1;
}

//
// Reconfiguration
//

// dwlbg_reconfigure is called after configuration changes in such a way that
// requires outputs to potentially be assigned to different animations. All
// outputs are returned to the idle_outputs list, and then one-by-one matched
// to the correct animation again. The animations will continue on their merry
// way, so outputs which end up back on the same animation will continue from
// the frame they were on. Finally, any animations which no longer have any
// outputs assigned will be cleaned up.
//
// This is not particularly efficient, but it's extremely simple which makes it
// unlikely to introduce bugs. We also don't have that many outputs.
void dwlbg_reconfigure(struct dwlbg_state * dwlbg) {
	// Return all outputs to the idle list.
	struct dwlbg_animation * anim;
	wl_list_for_each(anim, &dwlbg->animations, link) {
		wl_list_insert_list(&dwlbg->idle_outputs, &anim->outputs);
		wl_list_init(&anim->outputs);
	}

	// Now attempt to associate each output with its config, and therefore
	// animation. This might create new animations as needed.
	struct dwlbg_output * output, * tmp;
	wl_list_for_each_safe(output, tmp, &dwlbg->idle_outputs, link) {
		output->config = NULL;
		output->cached_frames = 0;

		struct dwlbg_output_config * opc, * wildcard_opc = NULL;
		wl_list_for_each(opc, &output->dwlbg->output_configs, link) {
			if (strcmp(opc->name, output->name) == 0) {
				output->config = opc;
				break;
			}
			if (strcmp(opc->name, "*") == 0) {
				// Collect this as we pass by if it exists, so we can apply it if
				// there's no exact match.
				wildcard_opc = opc;
			}
		}

		if (!output->config) {
			output->config = wildcard_opc;
		}

		struct dwlbg_animation * found_anim = NULL;
		if (output->config) {
			wl_list_for_each(anim, &output->dwlbg->animations, link) {
				if (strcmp(anim->path, output->config->image_path) == 0) {
					found_anim = anim;
					break;
				}
			}

			if (!found_anim) {
				// No animation exists, so make one. Note that this may still
				// fail, in which case this output will become idle.
				// TODO: It would be better to get any possible failures out of
				// the way at config time. The primary one is the image not
				// existing, which could be easily checked without creating an
				// animation.
				found_anim = dwlbg_animation_create(dwlbg,
						output->config->image_path);
			}
		}

		if (found_anim) {
			wl_list_remove(&output->link);
			wl_list_insert(found_anim->outputs.prev, &output->link);

			// Force a render to ensure there's a frame displayed on the output
			// even if the configured image is static.
			dwlbg_animation_schedule_frame(found_anim, 1);
		}
	}

	// Finally, clean up any animations which no longer have any outputs.
	struct dwlbg_animation * anim_tmp;
	wl_list_for_each_safe(anim, anim_tmp, &dwlbg->animations, link) {
		if (wl_list_empty(&anim->outputs)) {
			dwlbg_animation_destroy(anim);
		}
	}
}

//
// Main
//

static const char usage[] =
	"Usage: dwlbg [-c <config-path>]\n"
	"\n"
	"  -c  Path to the configuration file to use.\n"
	"      (default: $XDG_CONFIG_HOME/dwlbg/config)\n"
	"  -h  Show this text.\n"
	"\n"
	"To control dwlbg while it is running, use `dwlbgctl`.\n";

int main(int argc, char * argv[]) {
	struct dwlbg_state dwlbg = {0};
	dwlbg.cache_budget_bytes = DWLBG_DEFAULT_CACHE_BUDGET_MB * 1024UL * 1024UL;
	wl_list_init(&dwlbg.output_configs);
	wl_list_init(&dwlbg.idle_outputs);
	wl_list_init(&dwlbg.animations);

	for (size_t i = 0; i < DWLBG_EVENT_COUNT; ++i) {
		dwlbg.events[i] = (struct pollfd) {
			.fd = -1,
		};
	}

	char * config_path = NULL;
	int ret = 1;

	for (int argi = 1; argi < argc; ++argi) {
		if (strcmp(argv[argi], "-c") == 0) {
			if (++argi >= argc) {
				fprintf(stderr, usage);
				return 1;
			}
			free(config_path);
			config_path = strdup(argv[argi]);
		}
		else if (strcmp(argv[argi], "-h") == 0) {
			printf(usage);
			free(config_path);
			return 0;
		}
		else {
			fprintf(stderr, usage);
			free(config_path);
			return 1;
		}
	}

	if (!config_path) {
		config_path = strdup("$XDG_CONFIG_HOME/dwlbg/config");
	}
	if (!config_path) {
		fprintf(stderr, "Unable to allocate config path\n");
		return 1;
	}

	if (pipe(signal_pipe) == -1) {
		perror("Unable to create pipe for signal handler");
		return 1;
	}
	dwlbg.events[DWLBG_SIGNAL_EVENT] = (struct pollfd) {
		.fd = signal_pipe[0],
		.events = POLLIN,
	};
	struct sigaction act = {0};
	act.sa_handler = &signal_handler;
	int error = sigaction(SIGINT, &act, NULL);
	error += sigaction(SIGTERM, &act, NULL);
	error += sigaction(SIGQUIT, &act, NULL);
	if (error < 0) {
		perror("Unable to install signal handler");
		return 1;
	}

	// Ignore SIGPIPE, may occur on the IPC socket.
	signal(SIGPIPE, SIG_IGN);

	dwlbg.display = wl_display_connect(NULL);
	if (!dwlbg.display) {
		fprintf(stderr, "Unable to connect to Wayland display\n");
		free(config_path);
		close(signal_pipe[0]);
		close(signal_pipe[1]);
		return 1;
	}

	dwlbg.events[DWLBG_WAYLAND_EVENT] = (struct pollfd) {
		.fd = wl_display_get_fd(dwlbg.display),
		.events = POLLIN,
	};
	dwlbg.events[DWLBG_IPC_CONNECT_EVENT] = (struct pollfd) {
		.fd = dwlbg_ipc_create(&dwlbg),
		.events = POLLIN,
	};
	dwlbg.events[DWLBG_IPC_CLIENT_EVENT] = (struct pollfd) {
		.fd = -1,  // This event is idle when no client is connected.
		.events = POLLIN,
	};

	dwlbg.registry = wl_display_get_registry(dwlbg.display);
	wl_registry_add_listener(dwlbg.registry, &registry_listener, &dwlbg);
	wl_display_roundtrip(dwlbg.display);
	if (!dwlbg.compositor || !dwlbg.layer_shell || !dwlbg.shm) {
		fprintf(stderr, "Compositor is missing required Wayland protocols\n");
		free(config_path);
		goto cleanup;
	}

	int load_status = load_config_file(&dwlbg, config_path);
	free(config_path);
	if (load_status < 0) {
		goto cleanup;
	}

	dwlbg_reconfigure(&dwlbg);

	dwlbg.run = true;
	while (dwlbg.run) {
		while (wl_display_prepare_read(dwlbg.display) != 0) {
			wl_display_dispatch_pending(dwlbg.display);
		}
		wl_display_flush(dwlbg.display);

		int polled = poll(dwlbg.events, DWLBG_EVENT_COUNT, -1);
		if (polled < 0) {
			wl_display_cancel_read(dwlbg.display);
			continue;
		}

		if (dwlbg.events[DWLBG_SIGNAL_EVENT].revents & POLLIN) {
			int signal_number;
			const ssize_t size = sizeof(signal_number);
			if (read(signal_pipe[0], &signal_number, size) < size) {
				// Do nothing, I guess?
			}
			else {
				if (signal_number == SIGINT ||
						signal_number == SIGTERM ||
						signal_number == SIGQUIT) {
					dwlbg.run = false;
				}
			}
		}

		// Read wayland events first so we can handle any resizing, etc, before
		// attempting to draw again.
		if (dwlbg.events[DWLBG_WAYLAND_EVENT].revents & POLLIN) {
			if (wl_display_read_events(dwlbg.display) != 0) {
				if (errno == 104) {
					// Compositor disconnected us, exit quietly.
				}
				else {
					fprintf(stderr, "Failed to read Wayland events: %s\n",
							strerror(errno));
				}
				dwlbg.run = false;
			}
		}
		else {
			wl_display_cancel_read(dwlbg.display);
		}

		// At this point, we may have been shut down. Might as well not waste
		// time drawing.
		if (!dwlbg.run) {
			break;
		}

		// Check for a new IPC connection.
		if (dwlbg.events[DWLBG_IPC_CONNECT_EVENT].revents & POLLIN) {
			// If accept fails for some reason, it will return -1. We just let
			// that happen, because it will simply disable the client pollfd.
			int client = accept(dwlbg.events[DWLBG_IPC_CONNECT_EVENT].fd,
					NULL, NULL);

			if (client != -1) {
				int flags;
				if ((flags = fcntl(client, F_GETFL)) == -1 ||
						fcntl(client, F_SETFL, flags|O_NONBLOCK) == -1) {
					perror("Unable to set nonblocking on IPC client socket");
					close(client);
					client = -1;
				}
			}

			dwlbg.events[DWLBG_IPC_CLIENT_EVENT].fd = client;
		}

		if (dwlbg.events[DWLBG_IPC_CLIENT_EVENT].revents & POLLIN) {
			int client = dwlbg.events[DWLBG_IPC_CLIENT_EVENT].fd;
			dwlbg_ipc_handle_command(&dwlbg, client);
		}

		// Now see if we need to draw any frames.
		struct dwlbg_animation * anim;
		wl_list_for_each(anim, &dwlbg.animations, link) {
			if (dwlbg.events[anim->event_index].revents & POLLIN) {
				uint64_t expirations;
				ssize_t n = read(
						dwlbg.events[anim->event_index].fd,
						&expirations,
						sizeof(expirations));

				if (n < 0) {
					fprintf(stderr, "Failed to read timer events\n");
					break;
				}

				// This will update the animation's timerfd automatically if
				// neccessary. (Spooky!)
				dwlbg_render_frame(anim);
			}
		}
	}

	ret = 0;

cleanup:
	;
	struct dwlbg_animation * anim, * anim_tmp;
	wl_list_for_each_safe(anim, anim_tmp, &dwlbg.animations, link) {
		dwlbg_animation_destroy(anim);
	}

	// At this point, because we've destroyed all of the animations, all
	// outputs should be idle again and will be cleaned up here.
	struct dwlbg_output * output, * output_tmp;
	wl_list_for_each_safe(output, output_tmp, &dwlbg.idle_outputs, link) {
		dwlbg_output_destroy(output);
	}

	struct dwlbg_output_config * opc, * opc_tmp;
	wl_list_for_each_safe(opc, opc_tmp, &dwlbg.output_configs, link) {
	    dwlbg_output_config_destroy(opc);
	}

	dwlbg_ipc_destroy(&dwlbg);

	if (dwlbg.output_manager) {
		zxdg_output_manager_v1_destroy(dwlbg.output_manager);
	}
	if (dwlbg.layer_shell) {
		zwlr_layer_shell_v1_destroy(dwlbg.layer_shell);
	}

	if (dwlbg.compositor) {
		wl_compositor_destroy(dwlbg.compositor);
	}
	if (dwlbg.shm) {
		wl_shm_destroy(dwlbg.shm);
	}
	if (dwlbg.registry) {
		wl_registry_destroy(dwlbg.registry);
	}
	if (dwlbg.display) {
		wl_display_disconnect(dwlbg.display);
	}
	if (dwlbg.events[DWLBG_SIGNAL_EVENT].fd >= 0) {
		close(dwlbg.events[DWLBG_SIGNAL_EVENT].fd);
	}
	close(signal_pipe[1]);

	return ret;
}
