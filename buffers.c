//
// Shared memory buffers
//
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dwlbg.h"
#include "buffers.h"

static int pid_shm_open(const char * prefix, int oflag, mode_t mode) {
	static const char format[] = "%s-%d";

	pid_t pid = getpid();
	int length = snprintf(NULL, 0, format, prefix, pid);
	if (length < 0) {
		return -1;
	}
	char * name = calloc(length + 1, sizeof(char));
	if (!name) {
		return -1;
	}
	length = snprintf(name, length + 1, format, prefix, pid);
	if (length < 0) {
		free(name);
		return -1;
	}

	int fd = shm_open(name, oflag, mode);
	if (fd < 0) {
		free(name);
		return -1;
	}

	shm_unlink(name);
	free(name);
	return fd;
}

static void buffer_handle_release(
		void *data,
		struct wl_buffer *wl_buffer __attribute__((unused))) {
	((struct dwlbg_buffer *) data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

struct dwlbg_buffer * dwlbg_allocate_buffer(struct dwlbg_output * output) {
	uint32_t stride = cairo_format_stride_for_width(
			CAIRO_FMT,
			output->width * output->scale);
	size_t size = stride * output->height * output->scale;
	if (size < 1) {
		fprintf(stderr, "Tiny buffer\n");
		return NULL;
	}

	// O_EXCL shouldn't be necessary here, but I would rather have it fail if
	// something weird happens.
	errno = 0;
	int fd = pid_shm_open("/dwlbg-buffer", O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		fprintf(stderr, "Failed to create buffer backing memory: %s\n",
				strerror(errno));
		return NULL;
	}
	shm_unlink("/dwlbg-buffer");

	errno = 0;
	if (ftruncate(fd, size) < 0) {
		fprintf(stderr, "Failed to resize buffer memory: %s\n",
				strerror(errno));
		close(fd);
		return NULL;
	}

	errno = 0;
	void * data = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "Failed to map backing memory: %s\n", strerror(errno));
		close(fd);
		return NULL;
	}

	struct dwlbg_buffer * buffer = calloc(1, sizeof(struct dwlbg_buffer));
	if (!buffer) {
		fprintf(stderr, "Failed to allocate buffer metadata\n");
		munmap(data, size);
		close(fd);
		return NULL;
	}
	wl_list_init(&buffer->link);

	struct wl_shm_pool * pool = wl_shm_create_pool(
			output->dwlbg->shm, fd, size);
	if (!pool) {
		fprintf(stderr, "Failed to create shm pool\n");
		munmap(data, size);
		close(fd);
		free(buffer);
		return NULL;
	}
	buffer->backing = wl_shm_pool_create_buffer(
			pool, 0,
			output->width * output->scale,
			output->height * output->scale,
			stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (!buffer->backing) {
		fprintf(stderr, "Failed to create Wayland buffer\n");
		munmap(data, size);
		close(fd);
		free(buffer);
		return NULL;
	}
	wl_buffer_add_listener(buffer->backing, &buffer_listener, buffer);

	close(fd);

	buffer->data = data;
	buffer->size = size;
	buffer->cairo_surface = cairo_image_surface_create_for_data(
			data,
			CAIRO_FMT,
			output->width * output->scale,
			output->height * output->scale,
			stride);
	if (cairo_surface_status(buffer->cairo_surface) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "Failed to create cairo surface\n");
		wl_buffer_destroy(buffer->backing);
		munmap(buffer->data, buffer->size);
		free(buffer);
		return NULL;
	}
	buffer->cairo = cairo_create(buffer->cairo_surface);
	if (cairo_status(buffer->cairo) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "Failed to create cairo context\n");
		cairo_destroy(buffer->cairo);
		cairo_surface_destroy(buffer->cairo_surface);
		wl_buffer_destroy(buffer->backing);
		munmap(buffer->data, buffer->size);
		free(buffer);
		return NULL;
	}

	wl_list_insert(output->buffer_ring.prev, &buffer->link);
	return buffer;
}

bool dwlbg_allocate_buffers(struct dwlbg_output * output, unsigned int count) {
	struct dwlbg_buffer * buffer;

	// If we have too many buffers, shrink the pool instead to recover memory
	// and prevent getting the animation out of sync.
	if (output->buffer_count >= count) {
		for (; output->buffer_count > count; --output->buffer_count) {
			buffer = wl_container_of(output->buffer_ring.prev, buffer, link);
			dwlbg_buffer_destroy(buffer);
		}
	}
	else {
		for (; output->buffer_count < count; ++output->buffer_count) {
			buffer = dwlbg_allocate_buffer(output);
			if (!buffer) {
				return false;
			}
		}
	}
	return true;
}

struct dwlbg_buffer * dwlbg_next_buffer(struct dwlbg_output * output) {
	struct dwlbg_buffer * current = wl_container_of(
			output->buffer_ring.next, current, link);
	wl_list_remove(&current->link);
	wl_list_insert(output->buffer_ring.prev, &current->link);

	return wl_container_of(output->buffer_ring.next, current, link);
}

void dwlbg_buffer_destroy(struct dwlbg_buffer * buffer) {
	wl_list_remove(&buffer->link);

	cairo_destroy(buffer->cairo);
	cairo_surface_destroy(buffer->cairo_surface);
	wl_buffer_destroy(buffer->backing);

	munmap(buffer->data, buffer->size);
	free(buffer);
}
