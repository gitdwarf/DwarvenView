#pragma once
/* dv_themeicon -- a theme icon (the current GTK icon theme's) as a GdkTexture. */
#include <gdk/gdk.h>

/* The named icon at size x size pixels, or NULL if the theme has none or there is no display.
 * Main thread only (it uses the icon theme). */
GdkTexture *dv_theme_icon_texture (const char *icon_name, int size);
