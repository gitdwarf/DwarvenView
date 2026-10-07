#pragma once

/* DwarvenView embedded SVG icon.
 * Registered with GtkIconTheme at startup.
 * define DWARVENVIEW_ICON_IMPL in one .c file before including. */

static const char DWARVENVIEW_ICON_SVG[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\" width=\"64\" height=\"64\">\n"
    "  <!-- Outer frame -->\n"
    "  <rect x=\"4\" y=\"10\" width=\"56\" height=\"44\" rx=\"5\" ry=\"5\"\n"
    "        fill=\"#2a2a3a\" stroke=\"#7a7aaa\" stroke-width=\"2\"/>\n"
    "  <!-- Image area background -->\n"
    "  <rect x=\"8\" y=\"14\" width=\"48\" height=\"36\" rx=\"3\" ry=\"3\" fill=\"#1a1a2a\"/>\n"
    "  <!-- Mountain / landscape -->\n"
    "  <polygon points=\"14,46 28,24 36,34 42,28 52,46\"\n"
    "           fill=\"#4a6a9a\" opacity=\"0.9\"/>\n"
    "  <!-- Sun -->\n"
    "  <circle cx=\"44\" cy=\"22\" r=\"6\" fill=\"#f0c040\" opacity=\"0.95\"/>\n"
    "  <!-- Lens ring (viewfinder feel) -->\n"
    "  <circle cx=\"32\" cy=\"32\" r=\"14\" fill=\"none\"\n"
    "          stroke=\"#7a7aaa\" stroke-width=\"1.5\" opacity=\"0.5\"/>\n"
    "  <circle cx=\"32\" cy=\"32\" r=\"10\" fill=\"none\"\n"
    "          stroke=\"#9a9acc\" stroke-width=\"1\" opacity=\"0.3\"/>\n"
    "  <!-- Corner brackets -->\n"
    "  <path d=\"M10,18 L10,14 L14,14\" fill=\"none\" stroke=\"#aaaacc\" stroke-width=\"1.5\"/>\n"
    "  <path d=\"M54,18 L54,14 L50,14\" fill=\"none\" stroke=\"#aaaacc\" stroke-width=\"1.5\"/>\n"
    "  <path d=\"M10,46 L10,50 L14,50\" fill=\"none\" stroke=\"#aaaacc\" stroke-width=\"1.5\"/>\n"
    "  <path d=\"M54,46 L54,50 L50,50\" fill=\"none\" stroke=\"#aaaacc\" stroke-width=\"1.5\"/>\n"
    "</svg>\n"
    "";

#ifdef DWARVENVIEW_ICON_IMPL
#include <gtk/gtk.h>
#include <glib.h>

static void
dv_install_icon (void)
{
    /* Write icon to XDG cache so GTK can find it by name */
    const char *cache = g_get_user_cache_dir ();
    char *icon_dir = g_build_filename (cache, "dwarvenview", "icons",
                                       "hicolor", "scalable", "apps", NULL);
    g_mkdir_with_parents (icon_dir, 0755);

    char *icon_path = g_build_filename (icon_dir, "dwarvenview.svg", NULL);
    if (!g_file_test (icon_path, G_FILE_TEST_EXISTS)) {
        GError *err = NULL;
        g_file_set_contents (icon_path, DWARVENVIEW_ICON_SVG,
                             -1, &err);
        g_clear_error (&err);
    }

    /* Add cache dir to icon search path */
    char *icon_base = g_build_filename (cache, "dwarvenview", "icons", NULL);
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (
        gdk_display_get_default ());
    gtk_icon_theme_add_search_path (theme, icon_base);
    g_free (icon_base);
    g_free (icon_path);
    g_free (icon_dir);
}
#endif /* DWARVENVIEW_ICON_IMPL */
