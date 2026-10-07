#pragma once
/* dv_surface_tex -- turn a finished cairo ARGB32 image surface into a GdkTexture. */
#include <gdk/gdk.h>
#include <cairo.h>

/* Copies the pixels; the surface stays the caller's. */
GdkTexture *dv_texture_from_surface (cairo_surface_t *surf);
