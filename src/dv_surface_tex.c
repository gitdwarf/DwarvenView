#include "dv_surface_tex.h"

GdkTexture *
dv_texture_from_surface (cairo_surface_t *surf)
{
    cairo_surface_flush (surf);
    int     h      = cairo_image_surface_get_height (surf);
    int     stride = cairo_image_surface_get_stride (surf);
    GBytes *bytes  = g_bytes_new (cairo_image_surface_get_data (surf), (gsize) h * stride);
    GdkTexture *tex = gdk_memory_texture_new (cairo_image_surface_get_width (surf), h,
                                              GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, (gsize) stride);
    g_bytes_unref (bytes);
    return tex;
}
