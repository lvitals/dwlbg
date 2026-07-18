project(
	'dwlbg',
	'c',
	version: '1.0.0',
	license: 'MIT',
	meson_version: '>=0.46.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

cairo = dependency('cairo')
gdk_pixbuf = dependency('gdk-pixbuf-2.0')
wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols', version: '>=1.14')

# gdk-pixbuf's animation API is deprecated upstream, but it is still the API
# used here to keep animated GIF support.
add_project_arguments('-Wno-deprecated-declarations', language: 'c')

subdir('protocols')

c = meson.get_compiler('c')

executable(
	'dwlbg',
	files([
		'animation.c',
		'buffers.c',
		'cairo-pixbuf.c',
		'config.c',
		'dwlbg.c',
		'output.c',
	]),
	dependencies: [
		cairo,
		gdk_pixbuf,
		wayland_client,
		client_protos,
		c.find_library('rt'),  # For shm_open
	],
	install: true,
)

executable(
	'dwlbgctl',
	files([
		'dwlbgctl.c',
	]),
	install: true,
)
