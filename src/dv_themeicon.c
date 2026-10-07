#include "dv_themeicon.h"
#include "dv_surface_tex.h"
#include "dv_debug.h"

#include <gtk/gtk.h>

GdkTexture *
dv_theme_icon_texture (const char *icon_name, int size)
{
    GdkDisplay *display = gdk_display_get_default ();
    if (!display) return NULL;

    GtkIconTheme *theme = gtk_icon_theme_get_for_display (display);
    if (!gtk_icon_theme_has_icon (theme, icon_name)) {
        dv_debug_log ("themeicon: theme has no icon '%s'", icon_name);
        return NULL;
    }

    GtkIconPaintable *icon = gtk_icon_theme_lookup_icon (theme, icon_name, NULL, size, 1,
                                                         GTK_TEXT_DIR_NONE, 0);
    GtkSnapshot *snap = gtk_snapshot_new ();
    gdk_paintable_snapshot (GDK_PAINTABLE (icon), snap, size, size);
    GskRenderNode *node = gtk_snapshot_free_to_node (snap);
    g_object_unref (icon);
    if (!node) return NULL;

    /* Software draw: needs no realized window or renderer. */
    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create (surf);
    gsk_render_node_draw (node, cr);
    cairo_destroy (cr);
    gsk_render_node_unref (node);

    GdkTexture *tex = dv_texture_from_surface (surf);
    cairo_surface_destroy (surf);
    return tex;
}
