#include "dv_debug.h"

#include <glib/gstdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

/* Own tiny store: group "debug", key "enabled" in the same config file
 * dv_prefs.c uses. Deliberately independent of DvPrefs -- this is a
 * diagnostic switch, not a per-view preference, and keeping it out of the
 * DvPrefs struct means it needs no wiring through dv_widget_set_prefs()
 * or any of DvPrefs's copies. */
#define CONF_SUBDIR "dwarvenview"
#define CONF_FILE   "dwarvenview.conf"
#define CONF_GROUP  "debug"
#define CONF_KEY    "enabled"

/* Read once per process and cached: dv_debug_log() is called from the
 * decode thread on every load, so it must not touch disk when logging is
 * off. The cache is refreshed on every dv_debug_set_logging_enabled(). */
static gboolean cached_enabled = FALSE;
static gboolean cache_loaded   = FALSE;

static char *
conf_path (void)
{
    return g_build_filename (g_get_user_config_dir (), CONF_SUBDIR, CONF_FILE, NULL);
}

char *
dv_debug_log_path (void)
{
    return g_build_filename (g_get_user_cache_dir (), "dwarvenview", "debug.log", NULL);
}

static gboolean
read_enabled_from_disk (void)
{
    char     *path = conf_path ();
    GKeyFile *kf   = g_key_file_new ();
    gboolean  v    = g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL) &&
                     g_key_file_get_boolean (kf, CONF_GROUP, CONF_KEY, NULL);
    g_key_file_free (kf);
    g_free (path);
    return v;
}

gboolean
dv_debug_logging_enabled (void)
{
    if (!cache_loaded) {
        cached_enabled = read_enabled_from_disk ();
        cache_loaded   = TRUE;
    }
    return cached_enabled;
}

void
dv_debug_set_logging_enabled (gboolean enabled)
{
    enabled = enabled != FALSE;

    char     *path = conf_path ();
    GKeyFile *kf   = g_key_file_new ();
    g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL);   /* keep other groups */
    g_key_file_set_boolean (kf, CONF_GROUP, CONF_KEY, enabled);

    char *dir = g_path_get_dirname (path);
    g_mkdir_with_parents (dir, 0755);
    g_free (dir);
    g_key_file_save_to_file (kf, path, NULL);
    g_key_file_free (kf);
    g_free (path);

    cached_enabled = enabled;
    cache_loaded   = TRUE;

    /* David's own rule for DT4GTK's identical switch: turning it off
     * removes any existing log immediately, so a stale log from a
     * previous enabled session never lingers once the feature is off. */
    if (!enabled) {
        char *log = dv_debug_log_path ();
        g_unlink (log);
        g_free (log);
    }
}

void
dv_debug_log (const char *fmt, ...)
{
    if (!dv_debug_logging_enabled ())
        return;

    struct timeval tv;
    gettimeofday (&tv, NULL);
    struct tm tmv;
    localtime_r (&tv.tv_sec, &tmv);
    char stamp[32];
    strftime (stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;
    va_start (ap, fmt);
    char *body = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    char *line = g_strdup_printf ("[%s.%03d] %s\n", stamp,
                                  (int) (tv.tv_usec / 1000), body);
    g_free (body);

    char *path = dv_debug_log_path ();
    char *dir  = g_path_get_dirname (path);
    g_mkdir_with_parents (dir, 0755);
    g_free (dir);

    /* Full open-write-flush-fsync-close on every line: the write is
     * physically durable before this call returns, before whatever comes
     * next (a raw libav or librsvg call on an untrusted file) has any
     * chance to bring the process down. A crash after this point still
     * leaves the line on disk; a crash before it never reached here. */
    FILE *f = fopen (path, "a");
    if (f) {
        fputs (line, f);
        fflush (f);
        fsync (fileno (f));
        fclose (f);
    }
    g_free (path);

    /* Also stderr: free when a terminal is attached, harmless when not. */
    fputs (line, stderr);
    fflush (stderr);
    g_free (line);
}
