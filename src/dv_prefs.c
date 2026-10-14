#include "dv_prefs.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#define CONF_GROUP   "dwarvenview"
#define CONF_SUBDIR  "dwarvenview"
#define CONF_FILE    "dwarvenview.conf"

/* ── Config path ─────────────────────────────────────────────────────────── */

static char *
config_path (void)
{
    return g_build_filename (g_get_user_config_dir (),
                             CONF_SUBDIR, CONF_FILE, NULL);
}

/* ── Load / save ─────────────────────────────────────────────────────────── */

DvPrefs *
dv_prefs_load (void)
{
    DvPrefs *p = g_new0 (DvPrefs, 1);
    /* Defaults */
    p->pan.invert_arrow_lr  = FALSE;
    p->pan.invert_arrow_ud  = FALSE;
    p->pan.invert_wheel_ud  = FALSE;
    p->pan.invert_wheel_lr  = FALSE;
    p->fallback_save_dir    = NULL;
    p->ctx_dynamic          = FALSE;   /* matches historical behavior -- was
                                         * never persisted, so g_new0's zero-
                                         * fill (FALSE) has always been the
                                         * de facto default every session */
    p->default_view_mode    = DV_VIEW_MODE_FIT;
    p->show_loading_splash  = TRUE;    /* preserves current always-on behavior */
    p->loading_splash_delay = 0.0;     /* preserves current immediate-display behavior */
    p->fullscreen_hide_ui   = FALSE;   /* fullscreen keeps the controls unless asked */
    p->browse_media         = FALSE;   /* images only unless asked */

    char *path = config_path ();
    GKeyFile *kf = g_key_file_new ();
    GError   *err = NULL;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_clear_error (&err);
        g_key_file_free (kf);
        g_free (path);
        return p;
    }

    p->pan.invert_arrow_lr = g_key_file_get_boolean (kf, CONF_GROUP,
        "invert_arrow_lr", NULL);
    p->pan.invert_arrow_ud = g_key_file_get_boolean (kf, CONF_GROUP,
        "invert_arrow_ud", NULL);
    p->pan.invert_wheel_ud = g_key_file_get_boolean (kf, CONF_GROUP,
        "invert_wheel_ud", NULL);
    p->pan.invert_wheel_lr = g_key_file_get_boolean (kf, CONF_GROUP,
        "invert_wheel_lr", NULL);

    if (g_key_file_has_key (kf, CONF_GROUP, "ctx_dynamic", NULL))
        p->ctx_dynamic = g_key_file_get_boolean (kf, CONF_GROUP,
            "ctx_dynamic", NULL);

    if (g_key_file_has_key (kf, CONF_GROUP, "default_view_mode", NULL)) {
        gint v = g_key_file_get_integer (kf, CONF_GROUP,
            "default_view_mode", NULL);
        p->default_view_mode = (v == DV_VIEW_MODE_ACTUAL)
            ? DV_VIEW_MODE_ACTUAL : DV_VIEW_MODE_FIT;
    }

    if (g_key_file_has_key (kf, CONF_GROUP, "show_loading_splash", NULL))
        p->show_loading_splash = g_key_file_get_boolean (kf, CONF_GROUP,
            "show_loading_splash", NULL);

    if (g_key_file_has_key (kf, CONF_GROUP, "fullscreen_hide_ui", NULL))
        p->fullscreen_hide_ui = g_key_file_get_boolean (kf, CONF_GROUP,
            "fullscreen_hide_ui", NULL);

    if (g_key_file_has_key (kf, CONF_GROUP, "browse_media", NULL))
        p->browse_media = g_key_file_get_boolean (kf, CONF_GROUP,
            "browse_media", NULL);

    if (g_key_file_has_key (kf, CONF_GROUP, "loading_splash_delay", NULL)) {
        double d = g_key_file_get_double (kf, CONF_GROUP,
            "loading_splash_delay", NULL);
        if (d < 0.0) d = 0.0;
        if (d > 1.0) d = 1.0;
        p->loading_splash_delay = d;
    }

    char *fbd = g_key_file_get_string (kf, CONF_GROUP,
        "fallback_save_dir", NULL);

    char *ssi = g_key_file_get_string (kf, CONF_GROUP,
        "slideshow_interval", NULL);
    if (ssi && *ssi)
        p->slideshow_interval = ssi;
    else
        g_free (ssi);
    if (fbd && *fbd)
        p->fallback_save_dir = fbd;
    else
        g_free (fbd);

    g_key_file_free (kf);
    g_free (path);
    return p;
}

void
dv_prefs_save (const DvPrefs *p)
{
    GKeyFile *kf = g_key_file_new ();

    g_key_file_set_boolean (kf, CONF_GROUP, "invert_arrow_lr",
        p->pan.invert_arrow_lr);
    g_key_file_set_boolean (kf, CONF_GROUP, "invert_arrow_ud",
        p->pan.invert_arrow_ud);
    g_key_file_set_boolean (kf, CONF_GROUP, "invert_wheel_ud",
        p->pan.invert_wheel_ud);
    g_key_file_set_boolean (kf, CONF_GROUP, "invert_wheel_lr",
        p->pan.invert_wheel_lr);

    g_key_file_set_boolean (kf, CONF_GROUP, "ctx_dynamic",
        p->ctx_dynamic);
    g_key_file_set_integer (kf, CONF_GROUP, "default_view_mode",
        (gint) p->default_view_mode);
    g_key_file_set_boolean (kf, CONF_GROUP, "show_loading_splash",
        p->show_loading_splash);
    g_key_file_set_double (kf, CONF_GROUP, "loading_splash_delay",
        p->loading_splash_delay);
    g_key_file_set_boolean (kf, CONF_GROUP, "fullscreen_hide_ui",
        p->fullscreen_hide_ui);
    g_key_file_set_boolean (kf, CONF_GROUP, "browse_media",
        p->browse_media);

    g_key_file_set_string (kf, CONF_GROUP, "fallback_save_dir",
        p->fallback_save_dir ? p->fallback_save_dir : "");
    g_key_file_set_string (kf, CONF_GROUP, "slideshow_interval",
        p->slideshow_interval ? p->slideshow_interval : "3");

    char *path = config_path ();
    char *dir  = g_path_get_dirname (path);
    g_mkdir_with_parents (dir, 0755);
    g_free (dir);

    GError *err = NULL;
    g_key_file_save_to_file (kf, path, &err);
    if (err) {
        g_warning ("Could not save prefs: %s", err->message);
        g_error_free (err);
    }

    g_key_file_free (kf);
    g_free (path);
}

void
dv_prefs_free (DvPrefs *p)
{
    if (!p) return;
    g_free (p->fallback_save_dir);
    g_free (p->slideshow_interval);
    g_free (p);
}

/* ── Output path resolution ──────────────────────────────────────────────── */

/* Return the real home dir for the current user (handles root correctly) */
static const char *
real_home (void)
{
    struct passwd *pw = getpwuid (getuid ());
    return pw ? pw->pw_dir : g_get_home_dir ();
}

/* True if path is under the user's home (or /root for root) */
static gboolean
is_user_path (const char *path)
{
    const char *home = real_home ();
    return g_str_has_prefix (path, home);
}

/* True if path equals or starts with $TMPDIR or /tmp */
static gboolean
is_tmp_path (const char *path)
{
    const char *tmpdir = g_get_tmp_dir ();
    if (g_str_has_prefix (path, tmpdir)) return TRUE;
    if (g_str_has_prefix (path, "/tmp"))  return TRUE;
    return FALSE;
}

/* True if we can write a file in dir */
static gboolean
dir_writable (const char *dir)
{
    if (!dir || !g_file_test (dir, G_FILE_TEST_IS_DIR))
        return FALSE;
    return access (dir, W_OK) == 0;
}

/* Try to use dir as output location: writable, user path, not tmp */
static gboolean
dir_suitable (const char *dir)
{
    if (!dir) return FALSE;
    if (is_tmp_path (dir)) return FALSE;
    if (!is_user_path (dir)) return FALSE;
    if (!dir_writable (dir)) return FALSE;
    return TRUE;
}

/* Build output filename stem, appending _DV_Conversion<ISO datetime>
 * if the plain name would collide with an existing file. */
static char *
make_output_fname (const char *source_path, const char *ext, const char *dir)
{
    char *base  = g_path_get_basename (source_path);
    char *noext = g_strdup (base);
    g_free (base);
    char *dot = strrchr (noext, '.');
    if (dot) *dot = '\0';

    /* Try plain name first */
    char *plain = g_strdup_printf ("%s.%s", noext, ext);
    char *probe = g_build_filename (dir, plain, NULL);
    if (!g_file_test (probe, G_FILE_TEST_EXISTS)) {
        g_free (probe);
        g_free (noext);
        return plain;   /* no collision */
    }
    g_free (probe);
    g_free (plain);

    /* Collision -- append ISO datetime to the second */
    GDateTime *now = g_date_time_new_now_local ();
    char *stamp = g_date_time_format (now, "%Y%m%dT%H%M%S");
    g_date_time_unref (now);

    char *fname = g_strdup_printf ("%s_DV_Conversion%s.%s", noext, stamp, ext);
    g_free (stamp);
    g_free (noext);
    return fname;
}

char *
dv_prefs_resolve_output (const char    *source_path,
                         const char    *ext,
                         const DvPrefs *prefs)
{
    /* Resolve directory first, then build filename with collision check */
    /* We do a two-pass approach: find the dir, then make the filename */


    /* Find the output directory */
    const char *chosen_dir = NULL;
    char *src_dir = g_path_get_dirname (source_path);

    if (dir_suitable (src_dir))
        chosen_dir = src_dir;
    else if (prefs && prefs->fallback_save_dir &&
             dir_suitable (prefs->fallback_save_dir))
        chosen_dir = prefs->fallback_save_dir;
    else {
        const char *pics = g_get_user_special_dir (G_USER_DIRECTORY_PICTURES);
        if (pics && dir_suitable (pics))
            chosen_dir = pics;
        else
            chosen_dir = real_home ();
    }

    /* Build filename with collision check against chosen dir */
    char *out_fname = make_output_fname (source_path, ext, chosen_dir);
    char *result    = g_build_filename (chosen_dir, out_fname, NULL);
    g_free (out_fname);
    g_free (src_dir);
    return result;
}

/* ── Last accessed directory ─────────────────────────────────────────────── */

static char *
last_dir_path (void)
{
    return g_build_filename (g_get_user_cache_dir (),
                             "dwarvenview", "last_dir", NULL);
}

void
dv_prefs_save_last_dir (const char *dir)
{
    if (!dir) return;
    char *path = last_dir_path ();
    char *parent = g_path_get_dirname (path);
    g_mkdir_with_parents (parent, 0755);
    g_free (parent);
    GError *err = NULL;
    char *content = g_strdup_printf ("%s\n", dir);
    g_file_set_contents (path, content, -1, &err);
    g_free (content);
    g_clear_error (&err);
    g_free (path);
}

char *
dv_prefs_load_last_dir (void)
{
    char *path = last_dir_path ();
    char *content = NULL;
    g_file_get_contents (path, &content, NULL, NULL);
    g_free (path);
    if (!content) return NULL;
    /* Strip trailing newline */
    g_strchomp (content);
    if (!*content || !g_file_test (content, G_FILE_TEST_IS_DIR)) {
        g_free (content);
        return NULL;
    }
    return content;
}
