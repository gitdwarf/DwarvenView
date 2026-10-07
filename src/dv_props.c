#include "dv_props.h"

#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* Overall budget for the external tools. After this the built-in backend
 * answers instead, so the dialog never hangs on a stuck tool. */
#ifndef DV_PROPS_TIMEOUT_SECS
#define DV_PROPS_TIMEOUT_SECS 20
#endif

/* ── Model ────────────────────────────────────────────────────────────────── */

static void
row_free (DvPropRow *r)
{
    g_free (r->key);
    g_free (r->value);
    g_free (r);
}

static void
group_free (DvPropGroup *g)
{
    g_free (g->name);
    g_ptr_array_free (g->rows, TRUE);
    g_free (g);
}

static DvProps *
props_new (const char *path, const char *source)
{
    DvProps *p = g_new0 (DvProps, 1);
    p->path   = g_strdup (path);
    p->source = source;
    p->groups = g_ptr_array_new_with_free_func ((GDestroyNotify) group_free);
    return p;
}

void
dv_props_free (DvProps *p)
{
    if (!p) return;
    g_free (p->path);
    g_free (p->hint);
    g_ptr_array_free (p->groups, TRUE);
    g_free (p);
}

/* Find a group by name, creating it (at the end) if it does not exist. */
static DvPropGroup *
group_get (DvProps *p, const char *name)
{
    for (guint i = 0; i < p->groups->len; i++) {
        DvPropGroup *g = g_ptr_array_index (p->groups, i);
        if (strcmp (g->name, name) == 0)
            return g;
    }
    DvPropGroup *g = g_new0 (DvPropGroup, 1);
    g->name = g_strdup (name);
    g->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);
    g_ptr_array_add (p->groups, g);
    return g;
}

/* Append a row. Both strings are copied and trimmed; empty ones are ignored. */
static gboolean
row_add (DvPropGroup *g, const char *key, const char *value)
{
    char *k = g_strstrip (g_strdup (key));
    char *v = g_strstrip (g_strdup (value));
    if (!*k || !*v) {
        g_free (k);
        g_free (v);
        return FALSE;
    }
    DvPropRow *r = g_new0 (DvPropRow, 1);
    r->key   = k;
    r->value = v;
    g_ptr_array_add (g->rows, r);
    return TRUE;
}

/* Drop groups whose rows were all filtered out, so no orphan headings show. */
static void
props_prune_empty (DvProps *p)
{
    for (guint i = p->groups->len; i > 0; i--) {
        DvPropGroup *g = g_ptr_array_index (p->groups, i - 1);
        if (g->rows->len == 0)
            g_ptr_array_remove_index (p->groups, i - 1);
    }
}

static guint
props_row_count (const DvProps *p)
{
    guint n = 0;
    for (guint i = 0; i < p->groups->len; i++)
        n += ((DvPropGroup *) g_ptr_array_index (p->groups, i))->rows->len;
    return n;
}

char *
dv_props_to_text (const DvProps *p)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < p->groups->len; i++) {
        DvPropGroup *g = g_ptr_array_index (p->groups, i);
        if (s->len)
            g_string_append_c (s, '\n');
        g_string_append_printf (s, "[%s]\n", g->name);
        for (guint j = 0; j < g->rows->len; j++) {
            DvPropRow *r = g_ptr_array_index (g->rows, j);
            g_string_append_printf (s, "%s: %s\n", r->key, r->value);
        }
    }
    return g_string_free (s, FALSE);
}

/* ── Parsers ──────────────────────────────────────────────────────────────── */
/*
 * Each parser turns one tool's plain-text output into groups and rows and
 * returns TRUE only if the result is worth showing. Plain text is used on
 * purpose: all three tools have regular text formats, and it avoids a JSON
 * library dependency.
 */

/* Split "key<padding> : value" at the FIRST " : ". Values may contain " : ". */
static gboolean
split_kv (const char *line, char **key, const char **value)
{
    const char *sep = strstr (line, " : ");
    if (!sep)
        return FALSE;
    *key   = g_strndup (line, (gsize) (sep - line));
    *value = sep + 3;
    return TRUE;
}

/*
 * exiftool -G1 -a
 *   [Group]      Tag Name                    : value
 */
static gboolean
parse_exiftool (const char *text, DvProps *out)
{
    gboolean failed = FALSE;
    gchar  **lines  = g_strsplit (text, "\n", -1);

    for (gchar **l = lines; *l; l++) {
        char *line = g_strchomp (*l);
        if (line[0] != '[')
            continue;
        char *close = strchr (line, ']');
        if (!close)
            continue;

        char       *group = g_strndup (line + 1, (gsize) (close - line - 1));
        const char *rest  = close + 1;
        while (*rest == ' ')
            rest++;

        char       *key;
        const char *value;
        if (split_kv (rest, &key, &value)) {
            if (strcmp (group, "ExifTool") == 0) {
                /* The tool's own group: only its diagnostics matter. */
                if (strcmp (g_strstrip (key), "Error") == 0)
                    failed = TRUE;
                else if (strcmp (key, "Warning") == 0)
                    row_add (group_get (out, "Warnings"), key, value);
            } else {
                row_add (group_get (out, group), key, value);
            }
            g_free (key);
        }
        g_free (group);
    }
    g_strfreev (lines);
    return !failed && props_row_count (out) > 0;
}

/* Advance *p past one whitespace-delimited token; return it as a new string. */
static char *
take_token (const char **p)
{
    const char *s = *p;
    while (g_ascii_isspace (*s))
        s++;
    const char *e = s;
    while (*e && !g_ascii_isspace (*e))
        e++;
    *p = e;
    return g_strndup (s, (gsize) (e - s));
}

/* IFD offsets exiv2 lists as tags. They are file internals, not metadata. */
static gboolean
exiv2_is_pointer (const char *key)
{
    static const char *const POINTERS[] = { "Exif.Image.ExifTag", "Exif.Image.GPSTag",
                                            "Exif.Photo.InteroperabilityTag", NULL };
    for (int i = 0; POINTERS[i]; i++)
        if (strcmp (key, POINTERS[i]) == 0)
            return TRUE;
    return FALSE;
}

/*
 * exiv2 -pa
 *   Family.Group.Tag     Type     Count  value
 * Rows are grouped by the first two key segments, e.g. "Exif.Photo".
 */
static gboolean
parse_exiv2 (const char *text, DvProps *out)
{
    gchar **lines = g_strsplit (text, "\n", -1);

    for (gchar **l = lines; *l; l++) {
        const char *cur   = g_strchomp (*l);
        char       *key   = take_token (&cur);
        char       *type  = take_token (&cur);
        char       *count = take_token (&cur);
        const char *dot1  = strchr (key, '.');
        const char *dot2  = dot1 ? strchr (dot1 + 1, '.') : NULL;

        if (dot2 && *type && *count && !exiv2_is_pointer (key)) {
            char *group = g_strndup (key, (gsize) (dot2 - key));
            row_add (group_get (out, group), dot2 + 1, cur);
            g_free (group);
        }
        g_free (key);
        g_free (type);
        g_free (count);
    }
    g_strfreev (lines);
    return props_row_count (out) > 0;
}

/*
 * mediainfo (default text)
 *   General
 *   Complete name          : /path/file
 *   <blank line>
 *   Image
 *   ...
 * Useful only if some section carries a "Format" row: for files it cannot
 * identify, mediainfo prints just the name and size and still exits 0.
 */
static gboolean
parse_mediainfo (const char *text, DvProps *out)
{
    gboolean     has_format = FALSE;
    DvPropGroup *cur        = NULL;
    gchar      **lines      = g_strsplit (text, "\n", -1);

    for (gchar **l = lines; *l; l++) {
        char *line = g_strchomp (*l);
        if (!*line) {
            cur = NULL;                     /* blank line ends a section */
            continue;
        }
        char       *key;
        const char *value;
        if (!split_kv (line, &key, &value)) {
            cur = group_get (out, g_strstrip (line));   /* section title */
            continue;
        }
        if (cur) {
            g_strstrip (key);
            if (row_add (cur, key, value) && strcmp (key, "Format") == 0)
                has_format = TRUE;
        }
        g_free (key);
    }
    g_strfreev (lines);
    return has_format;
}

/*
 * ffprobe -show_format -show_streams
 *   [STREAM] / [FORMAT] blocks of key=value lines.
 * Dropped as noise: "N/A" values and DISPOSITION flags that are 0.
 */
static gboolean
parse_ffprobe (const char *text, DvProps *out)
{
    DvPropGroup *cur   = NULL;
    char        *index = NULL, *ctype = NULL;
    gchar      **lines = g_strsplit (text, "\n", -1);

    for (gchar **l = lines; *l; l++) {
        char *line = g_strstrip (*l);
        if (strcmp (line, "[STREAM]") == 0 || strcmp (line, "[FORMAT]") == 0) {
            gboolean is_stream = (line[1] == 'S');
            /* Distinct temporary names keep group_get() from merging blocks. */
            char *tmp = g_strdup_printf ("%s#%u", is_stream ? "Stream" : "Format",
                                         out->groups->len);
            cur = group_get (out, tmp);
            g_free (tmp);
            g_clear_pointer (&index, g_free);
            g_clear_pointer (&ctype, g_free);
            continue;
        }
        if (line[0] == '[' && line[1] == '/') {          /* block end: final name */
            if (cur) {
                g_free (cur->name);
                if (index && ctype)
                    cur->name = g_strdup_printf ("Stream %s (%s)", index, ctype);
                else if (index)
                    cur->name = g_strdup_printf ("Stream %s", index);
                else
                    cur->name = g_strdup ("Format");
            }
            cur = NULL;
            continue;
        }
        char *eq = strchr (line, '=');
        if (!cur || !eq)
            continue;

        char       *key   = g_strndup (line, (gsize) (eq - line));
        const char *value = eq + 1;
        if (strcmp (key, "index") == 0)      { g_free (index); index = g_strdup (value); }
        if (strcmp (key, "codec_type") == 0) { g_free (ctype); ctype = g_strdup (value); }

        gboolean noise = (strcmp (value, "N/A") == 0) ||
                         (g_str_has_prefix (key, "DISPOSITION:") && strcmp (value, "0") == 0);
        if (!noise) {
            const char *shown = g_str_has_prefix (key, "TAG:") ? key + 4 : key;
            char *pretty = g_strdelimit (g_strdup (shown), "_:", ' ');
            row_add (cur, pretty, value);
            g_free (pretty);
        }
        g_free (key);
    }
    g_strfreev (lines);
    g_free (index);
    g_free (ctype);
    return props_row_count (out) > 0;
}

/* ── Backend table ────────────────────────────────────────────────────────── */
/*
 * Order is preference order. To add a tool, add a row (and a parser).
 * argv[0] is the program, the rest are fixed arguments; PATH_FLAG, if set, is
 * an option that must precede the file name.
 */

typedef struct {
    const char        *name;
    const char *const *argv;
    const char        *path_flag;
    gboolean         (*parse) (const char *text, DvProps *out);
} Backend;

static const char *const EXIFTOOL_ARGV[]  = { "exiftool", "-charset", "filename=utf8",
                                              "-G1", "-a", NULL };
static const char *const EXIV2_ARGV[]    = { "exiv2", "-pa", NULL };
static const char *const MEDIAINFO_ARGV[] = { "mediainfo", NULL };
static const char *const FFPROBE_ARGV[]   = { "ffprobe", "-hide_banner", "-loglevel", "error",
                                              "-show_format", "-show_streams", NULL };

static const Backend BACKENDS[] = {
    { "exiftool",  EXIFTOOL_ARGV,  NULL, parse_exiftool  },
    { "exiv2",     EXIV2_ARGV,     NULL, parse_exiv2     },
    { "mediainfo", MEDIAINFO_ARGV, NULL, parse_mediainfo },
    { "ffprobe",   FFPROBE_ARGV,   "-i", parse_ffprobe   },
};

/* Run one backend. Returns its stdout as valid UTF-8, or NULL if it could not
 * be started, exited non-zero, or was cancelled. No shell is involved and
 * PATH is an absolute path, so it cannot be mistaken for an option. */
static char *
run_backend (const char *bin, const Backend *b, const char *path, GCancellable *cancel)
{
    GPtrArray *argv = g_ptr_array_new ();
    g_ptr_array_add (argv, (gpointer) bin);
    for (int i = 1; b->argv[i]; i++)
        g_ptr_array_add (argv, (gpointer) b->argv[i]);
    if (b->path_flag)
        g_ptr_array_add (argv, (gpointer) b->path_flag);
    g_ptr_array_add (argv, (gpointer) path);
    g_ptr_array_add (argv, NULL);

    GError      *err  = NULL;
    GSubprocess *proc = g_subprocess_newv ((const gchar * const *) argv->pdata,
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    g_ptr_array_free (argv, TRUE);
    if (!proc) {
        g_clear_error (&err);
        return NULL;
    }

    GBytes *out = NULL;
    char   *text = NULL;
    if (g_subprocess_communicate (proc, NULL, cancel, &out, NULL, &err)) {
        if (g_subprocess_get_if_exited (proc) &&
            g_subprocess_get_exit_status (proc) == 0) {
            gsize len;
            const char *data = g_bytes_get_data (out, &len);
            text = g_utf8_make_valid (data ? data : "", (gssize) len);
        }
    } else {
        g_subprocess_force_exit (proc);      /* timed out or cancelled */
        g_clear_error (&err);
    }
    if (out)
        g_bytes_unref (out);
    g_object_unref (proc);
    return text;
}

/* ── Built-in backend (always available) ──────────────────────────────────── */
/* No parsing of our own: GIO supplies the file facts, gdk-pixbuf sniffs the
 * image header. This is the floor that keeps the dialog useful when no
 * metadata tool is installed. */

static DvProps *
props_builtin (const char *path)
{
    DvProps      *p    = props_new (path, "built-in");
    DvPropGroup  *file = group_get (p, "File");
    GFile        *f    = g_file_new_for_path (path);
    GFileInfo    *info = g_file_query_info (f,
        G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE, NULL, NULL);

    char *base = g_path_get_basename (path);
    char *dir  = g_path_get_dirname (path);
    row_add (file, "Name", base);
    row_add (file, "Location", dir);
    g_free (base);
    g_free (dir);

    if (!info) {
        row_add (file, "Error", "Cannot read this file");
    } else {
        goffset size  = g_file_info_get_size (info);
        char   *human = g_format_size ((guint64) size);
        char   *both  = g_strdup_printf ("%s (%" G_GINT64_FORMAT " bytes)", human, (gint64) size);
        row_add (file, "Size", both);
        g_free (human);
        g_free (both);

        GDateTime *mt = g_file_info_get_modification_date_time (info);
        if (mt) {
            GDateTime *local = g_date_time_to_local (mt);
            char *when = g_date_time_format (local, "%Y-%m-%d %H:%M:%S");
            row_add (file, "Modified", when);
            g_free (when);
            g_date_time_unref (local);
            g_date_time_unref (mt);
        }

        const char *ct = g_file_info_get_content_type (info);
        if (ct) {
            char *desc = g_content_type_get_description (ct);
            char *both2 = g_strdup_printf ("%s (%s)", desc ? desc : ct, ct);
            row_add (file, "Type", both2);
            g_free (desc);
            g_free (both2);
        }
        g_object_unref (info);
    }
    g_object_unref (f);

    int w = 0, h = 0;
    GdkPixbufFormat *fmt = gdk_pixbuf_get_file_info (path, &w, &h);
    if (fmt) {
        DvPropGroup *img = group_get (p, "Image");
        char *desc = gdk_pixbuf_format_get_description (fmt);
        char *dims = g_strdup_printf ("%d x %d pixels", w, h);
        row_add (img, "Format", desc);
        row_add (img, "Dimensions", dims);
        g_free (desc);
        g_free (dims);
    }
    return p;
}

/* ── Chain ────────────────────────────────────────────────────────────────── */

static DvProps *
props_build (const char *path, GCancellable *cancel)
{
    gboolean exiftool_found = FALSE;
    gboolean any_found      = FALSE;

    for (guint i = 0; i < G_N_ELEMENTS (BACKENDS); i++) {
        const Backend *b = &BACKENDS[i];
        if (cancel && g_cancellable_is_cancelled (cancel))
            break;

        char *bin = g_find_program_in_path (b->argv[0]);
        if (!bin)
            continue;
        any_found = TRUE;
        if (strcmp (b->name, "exiftool") == 0)
            exiftool_found = TRUE;

        char *text = run_backend (bin, b, path, cancel);
        g_free (bin);
        if (!text)
            continue;

        DvProps *p  = props_new (path, b->name);
        gboolean ok = b->parse (text, p);
        g_free (text);
        if (ok) {
            props_prune_empty (p);
            if (!exiftool_found)
                p->hint = g_strdup ("exiftool is not installed. Install it for "
                                    "full EXIF and XMP detail.");
            return p;
        }
        dv_props_free (p);
    }

    DvProps *p = props_builtin (path);
    p->hint = g_strdup (any_found
        ? "The installed metadata tools could not read this file."
        : "No metadata tool found. Install exiftool for full EXIF and XMP detail.");
    return p;
}

/* Absolute, normalised path: always starts with '/', so it can never be read
 * as an option by the tool it is handed to. */
static char *
absolute_path (const char *path)
{
    return g_canonicalize_filename (path, NULL);
}

DvProps *
dv_props_fetch_sync (const char *path)
{
    char    *abs = absolute_path (path);
    DvProps *p   = props_build (abs, NULL);
    g_free (abs);
    return p;
}

/* ── Async wrapper ────────────────────────────────────────────────────────── */

static void
props_thread (GTask *task, gpointer source, gpointer path, GCancellable *cancel)
{
    (void) source;
    DvProps *p = props_build ((const char *) path, cancel);
    g_task_return_pointer (task, p, (GDestroyNotify) dv_props_free);
}

static gboolean
watchdog_cb (gpointer cancellable)
{
    g_cancellable_cancel (G_CANCELLABLE (cancellable));
    return G_SOURCE_REMOVE;
}

void
dv_props_fetch_async (const char *path, GAsyncReadyCallback cb, gpointer user_data)
{
    GCancellable *cancel = g_cancellable_new ();
    GTask        *task   = g_task_new (NULL, cancel, cb, user_data);

    /* Cancelling only interrupts the tools; the task must still deliver the
     * built-in result, so it must not short-circuit on the cancellable. */
    g_task_set_check_cancellable (task, FALSE);
    g_task_set_task_data (task, absolute_path (path), g_free);

    /* The source owns a ref, so a late fire on a finished task is harmless. */
    g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, DV_PROPS_TIMEOUT_SECS,
                                watchdog_cb, g_object_ref (cancel), g_object_unref);

    g_task_run_in_thread (task, props_thread);
    g_object_unref (task);
    g_object_unref (cancel);
}

DvProps *
dv_props_fetch_finish (GAsyncResult *res)
{
    return g_task_propagate_pointer (G_TASK (res), NULL);
}
