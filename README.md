# dwlbg
### Animated and static wallpaper daemon for dwl

	>> dwlbg -h
	Usage: dwlbg [-c <config-path>]

	  -c  Path to the configuration file to use.
		  (default: $XDG_CONFIG_HOME/dwlbg/config)
	  -h  Show this text.

## Features

- Displays animated GIF wallpapers on dwl.
- Displays static image wallpapers supported by gdk-pixbuf.
- Supports per-output configuration for multi-monitor dwl setups.
- Shares one animation timer across outputs using the same image, keeping
  matching wallpapers in sync.

## Configuration

The configuration file is ini-style. Here's an example:

	[output LVDS-1]
	image=$XDG_CONFIG_HOME/wallpaper
	filter=nearest
	scaling-mode=fill
	anchor=center

See `config.example` for a wildcard setup and per-monitor overrides.

Outputs displaying the same image share the animation timer, and are therefore
always in sync.

The output name `*` will match any output not specified elsewhere in the file.
For dwl, output names are provided through xdg-output. You can inspect them
with tools such as `wlr-randr`.

### Output options

- `image`: Path to the image on disk, environment variables and ~ are expanded.
- `scaling-mode`: How to scale the image to fit on the output:
	- `fill` (default)
	- `tile`
	- `stretch`
- `anchor`: Some combination of `top`, `bottom`, `left`, `right`, and `center`.
	Can be combined with dashes, such as `center-left`.
- `filter`: Scaling filter to use. Supported values:
	- `fast`: Quickest
	- `good`: Balance of speed and quality
	- `best`: Looks really good
	- `nearest`: Nearest neighbor, good for pixel art
	- `bilinear`: Linear interpolation

These are provided by [cairo](https://cairographics.org/manual/cairo-cairo-pattern-t.html#cairo-filter-t).

## Build + run

	meson build
	ninja -C build
	build/dwlbg

For an up-to-date dependency list, check out meson.build.

dwl must be built with support for these protocols:

- wlr-layer-shell-unstable-v1
- xdg-output-unstable-v1

The bundled `wlr-layer-shell-unstable-v1.xml` is copied from this repository's
`dwl/protocols` tree, which targets the wlroots-0.20 based dwl build here.

## Resource usage

CPU consumption will vary depending on the framerate of the chosen image, as
dwlbg must wake up for every frame. However, with a reasonable (but still
visually interesting) image, I have seen it idling as low as 0.3%. It will be
noticably higher immediately after startup (or after reconfiguration), until it
can cache all of the scaled frames per output.

Memory consumption is a factor of the number of frames in each configured
image, the number of outputs displaying each image, and the resolution of each
display. It will remain constant once frames are cached.
