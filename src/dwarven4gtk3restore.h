/*
 * Dwarven4GTK3Restore.h
 * Version: 1.1.0
 *
 * Single-header library that restores GTK3 behaviours removed or deprecated
 * in GTK4, by reading the [X-DwarvenSuite] section of the standard GTK4
 * settings file (~/.config/gtk-4.0/settings.ini).
 *
 * USAGE
 * -----
 * 1. Drop this header into your project.
 * 2. In exactly ONE .c file, before including this header:
 *
 *      #define DWARVEN4GTK3RESTORE_IMPLEMENTATION
 *      #include "dwarven4gtk3restore.h"
 *
 * 3. In all other files that need the API, just:
 *
 *      #include "dwarven4gtk3restore.h"
 *
 * 4. After gtk_application_run() returns (or in your activate handler):
 *
 *      dwarven4gtk3restore_init ();
 *
 * 5. For live updates when the user changes their theme:
 *
 *      dwarven4gtk3restore_watch ();   // call once after init
 *
 * SUPPORTED KEYS (in [X-DwarvenSuite] section)
 * ---------------------------------------------
 * gtk-toolbar-style      integer   0=icons+text 1=text 2=icons 3=both 4=icons-only
 * gtk-toolbar-icon-size  integer   1=small 2=medium 3=large
 * gtk-button-images      boolean   true/false -- show icons in buttons
 * gtk-menu-images        boolean   true/false -- show icons in menu items
 * gtk-theme-name         string    widget theme name (set by DwarvenThemer4GTK)
 * dark-mode              boolean   true/false -- dark theme preference
 *
 * STANDARDS COMPLIANCE
 * --------------------
 * Reads ~/.config/gtk-4.0/settings.ini (the standard GTK4 settings file).
 * The [X-DwarvenSuite] section is namespaced and ignored by GTK4 itself.
 * If the section is absent the library does nothing -- the app behaves as
 * any standard GTK4 app.
 *
 * THIRD PARTY USE
 * ---------------
 * Any GTK4 app can use this library. It has no dependency on DwarvenSuite
 * or DwarvenThemer4GTK. Users who have the [X-DwarvenSuite] section in
 * their settings.ini get the extended behaviour; users who do not are
 * unaffected.
 *
 * LICENSE: GPL-2.0-or-later
 * AUTHOR:  thedwarf / gitdwarf
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <glib.h>
#include <gtk/gtk.h>

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * Read [X-DwarvenSuite] from ~/.config/gtk-4.0/settings.ini and apply
 * any recognised keys. Safe to call multiple times (re-reads and re-applies).
 * Returns TRUE if the section was found and at least one key applied.
 */
gboolean dwarven4gtk3restore_init (void);

/*
 * Watch ~/.config/gtk-4.0/settings.ini for changes and automatically
 * re-apply when the file is modified. Call once after init.
 * The monitor is attached to the default GLib main context.
 */
void dwarven4gtk3restore_watch (void);


/* ── Implementation ─────────────────────────────────────────────────────── */

#ifdef DWARVEN4GTK3RESTORE_IMPLEMENTATION

#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>

#define D4G_SECTION     "X-DwarvenSuite"
#define D4G_SETTINGS    "gtk-4.0/settings.ini"

/* Toolbar style constants (matching GTK2/3 GtkToolbarStyle) */
#define D4G_TOOLBAR_BOTH       0
#define D4G_TOOLBAR_TEXT       1
#define D4G_TOOLBAR_ICONS      2
#define D4G_TOOLBAR_BOTH_HORIZ 3
#define D4G_TOOLBAR_ICONS_ONLY 4

/* Icon size constants */
#define D4G_ICON_SMALL         1
#define D4G_ICON_MEDIUM        2
#define D4G_ICON_LARGE         3

static char *
d4g_settings_path (void)
{
    return g_build_filename (g_get_user_config_dir (), D4G_SETTINGS, NULL);
}

/*
 * Apply CSS to honour gtk-button-images and gtk-menu-images.
 * In GTK4 these are deprecated -- we restore them via CSS.
 */
static void
d4g_apply_button_images (gboolean show)
{
    static GtkCssProvider *prov = NULL;
    if (prov) {
        gtk_style_context_remove_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (prov));
        g_clear_object (&prov);
    }

    if (!show) return;

    prov = gtk_css_provider_new ();
    /* Restore icon visibility in buttons -- GTK4 hides them by default */
    gtk_css_provider_load_from_string (prov,
        "button image { -gtk-icon-size: 16px; }\n"
        ".image-button image { min-width: 16px; min-height: 16px; }\n");
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (),
        GTK_STYLE_PROVIDER (prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void
d4g_apply_menu_images (gboolean show)
{
    static GtkCssProvider *prov = NULL;
    if (prov) {
        gtk_style_context_remove_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (prov));
        g_clear_object (&prov);
    }

    if (!show) return;

    prov = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (prov,
        "menuitem image { min-width: 16px; min-height: 16px; }\n"
        "popover modelbutton image { min-width: 16px; min-height: 16px; }\n");
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (),
        GTK_STYLE_PROVIDER (prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/*
 * Apply toolbar icon size via CSS on the GtkToolbar / GtkBox toolbars.
 */
static void
d4g_apply_icon_size (int size)
{
    static GtkCssProvider *prov = NULL;
    if (prov) {
        gtk_style_context_remove_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (prov));
        g_clear_object (&prov);
    }

    int px;
    switch (size) {
    case D4G_ICON_SMALL:  px = 16; break;
    case D4G_ICON_LARGE:  px = 32; break;
    default:              px = 24; break;   /* medium */
    }

    prov = gtk_css_provider_new ();
    char *css = g_strdup_printf (
        ".toolbar button image { -gtk-icon-size: %dpx; "
        "min-width: %dpx; min-height: %dpx; }\n",
        px, px, px);
    gtk_css_provider_load_from_string (prov, css);
    g_free (css);
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (),
        GTK_STYLE_PROVIDER (prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/*
 * Apply toolbar style.
 * GTK4 doesn't have GtkToolbar -- we target GtkBox toolbars and
 * GtkButton children. Text visibility is controlled via CSS.
 */
static void
d4g_apply_toolbar_style (int style)
{
    static GtkCssProvider *prov = NULL;
    if (prov) {
        gtk_style_context_remove_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (prov));
        g_clear_object (&prov);
    }

    const char *css = NULL;
    switch (style) {
    case D4G_TOOLBAR_TEXT:
        /* Text only -- hide images in toolbar buttons */
        css = ".toolbar button image { min-width: 0; min-height: 0; "
              "opacity: 0; }\n"
              ".toolbar button label { margin: 0; }\n";
        break;
    case D4G_TOOLBAR_ICONS:
    case D4G_TOOLBAR_ICONS_ONLY:
        /* Icons only -- hide labels in toolbar buttons */
        css = ".toolbar button label { min-width: 0; min-height: 0; "
              "opacity: 0; margin: 0; }\n";
        break;
    case D4G_TOOLBAR_BOTH:
    case D4G_TOOLBAR_BOTH_HORIZ:
    default:
        /* Both -- no override needed */
        return;
    }

    prov = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (prov, css);
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (),
        GTK_STYLE_PROVIDER (prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ── Core read + apply ──────────────────────────────────────────────────── */

gboolean
dwarven4gtk3restore_init (void)
{
    char     *path = d4g_settings_path ();
    GKeyFile *kf   = g_key_file_new ();
    GError   *err  = NULL;
    gboolean  found = FALSE;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error (&err);
        g_key_file_free (kf);
        g_free (path);
        return FALSE;
    }
    g_free (path);

    /* Apply baseline menubar/popover spacing regardless of section presence.
     * GtkPopoverMenuBar in GTK4 strips all GTK2/3 menu spacing by default. */
    {
        static GtkCssProvider *base_prov = NULL;
        if (!base_prov) {
            base_prov = gtk_css_provider_new ();
            gtk_css_provider_load_from_string (base_prov,
                "menubar > item { padding: 2px 8px; }\n"
                "menubar > item:hover { padding: 2px 8px; }\n"
                ".menubar > button { padding: 2px 8px; min-height: 0; }\n"
                "popover.menu contents { padding: 4px 2px; }\n"
                "popover.menu modelbutton { padding: 4px 16px; min-height: 0; }\n"
                "popover.menu modelbutton label { padding: 0 4px; }\n"
                "popover.menu separator { margin: 2px 8px; }\n");
            gtk_style_context_add_provider_for_display (
                gdk_display_get_default (),
                GTK_STYLE_PROVIDER (base_prov),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    }

    if (!g_key_file_has_group (kf, D4G_SECTION)) {
        g_key_file_free (kf);
        return FALSE;
    }

    found = TRUE;

    /* gtk-toolbar-style */
    if (g_key_file_has_key (kf, D4G_SECTION, "gtk-toolbar-style", NULL)) {
        int v = (int) g_key_file_get_integer (kf, D4G_SECTION,
                    "gtk-toolbar-style", NULL);
        d4g_apply_toolbar_style (v);
    }

    /* gtk-toolbar-icon-size */
    if (g_key_file_has_key (kf, D4G_SECTION, "gtk-toolbar-icon-size", NULL)) {
        int v = (int) g_key_file_get_integer (kf, D4G_SECTION,
                    "gtk-toolbar-icon-size", NULL);
        d4g_apply_icon_size (v);
    }

    /* gtk-button-images */
    if (g_key_file_has_key (kf, D4G_SECTION, "gtk-button-images", NULL)) {
        gboolean v = g_key_file_get_boolean (kf, D4G_SECTION,
                         "gtk-button-images", NULL);
        d4g_apply_button_images (v);
    }

    /* gtk-menu-images */
    if (g_key_file_has_key (kf, D4G_SECTION, "gtk-menu-images", NULL)) {
        gboolean v = g_key_file_get_boolean (kf, D4G_SECTION,
                         "gtk-menu-images", NULL);
        d4g_apply_menu_images (v);
    }

    /* gtk-theme-name -- written by DwarvenThemer4GTK for live theme switching */
    if (g_key_file_has_key (kf, D4G_SECTION, "gtk-theme-name", NULL)) {
        char *theme = g_key_file_get_string (kf, D4G_SECTION,
                          "gtk-theme-name", NULL);
        if (theme && *theme) {
            GtkSettings *settings = gtk_settings_get_default ();
            if (settings)
                g_object_set (settings, "gtk-theme-name", theme, NULL);
        }
        g_free (theme);
    }

    /* dark-mode -- gtk-application-prefer-dark-theme */
    if (g_key_file_has_key (kf, D4G_SECTION, "dark-mode", NULL)) {
        char *dm = g_key_file_get_string (kf, D4G_SECTION, "dark-mode", NULL);
        gboolean dark = dm && (g_strcmp0 (dm, "true") == 0);
        GtkSettings *settings = gtk_settings_get_default ();
        if (settings)
            g_object_set (settings, "gtk-application-prefer-dark-theme",
                          dark, NULL);
        g_free (dm);
    }

    g_key_file_free (kf);
    return found;
}

/* ── File monitor for live updates ─────────────────────────────────────── */

static void
d4g_on_file_changed (GFileMonitor      *monitor,
                     GFile             *file,
                     GFile             *other,
                     GFileMonitorEvent  event,
                     gpointer           user_data)
{
    (void) monitor; (void) file; (void) other; (void) user_data;
    if (event == G_FILE_MONITOR_EVENT_CHANGED ||
        event == G_FILE_MONITOR_EVENT_CREATED) {
        dwarven4gtk3restore_init ();
    }
}

void
dwarven4gtk3restore_watch (void)
{
    static GFileMonitor *mon = NULL;

    /* Already watching -- prevent double monitors if called twice */
    if (mon) return;

    char  *path = d4g_settings_path ();
    GFile *f    = g_file_new_for_path (path);
    g_free (path);

    GError *err = NULL;
    mon = g_file_monitor_file (f, G_FILE_MONITOR_NONE, NULL, &err);
    g_object_unref (f);

    if (!mon) {
        g_warning ("Dwarven4GTK3Restore: cannot watch settings file: %s",
                   err ? err->message : "unknown error");
        g_clear_error (&err);
        return;
    }

    g_signal_connect (mon, "changed", G_CALLBACK (d4g_on_file_changed), NULL);
    /* Static pointer keeps monitor alive for process lifetime.
     * g_file_monitor_file returns a full reference -- no ref_sink needed. */
}

#endif /* DWARVEN4GTK3RESTORE_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif
