#ifndef DWLBG_CAIRO_PIXBUF_H
#define DWLBG_CAIRO_PIXBUF_H

#include <cairo/cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

int dwlbg_cairo_surface_paint_pixbuf(
		cairo_surface_t * surface, const GdkPixbuf * pixbuf);

#endif
