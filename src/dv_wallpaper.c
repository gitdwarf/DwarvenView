#include "dv_wallpaper.h"
#include "dv_export.h"

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

/* ── Internal helpers ───────────────────────────────────────────────────── */

/*
 * Returns $XDG_DATA_HOME/wallpapers, creating it if needed.
 * Caller must g_free().
 */
char *
dv_wallpaper_dir (void)
{
    const char *data_home = g_get_user_data_dir ();   /* XDG_DATA_HOME */
    char *dir = g_build_filename (data_home, "wallpapers", NULL);
    g_mkdir_with_parents (dir, 0755);
    return dir;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

gboolean
dv_wallpaper_set (const char *image_path, char **error_out)
{
    if (!image_path || !*image_path) {
        if (error_out) *error_out = g_strdup ("No image path provided");
        return FALSE;
    }

    char *dir      = dv_wallpaper_dir ();
    char *basename = g_path_get_basename (image_path);
    char *dest     = g_build_filename (dir, basename, NULL);
    char *link_path = g_build_filename (dir, ".current", NULL);

    gboolean ok = TRUE;

    /* Copy file into wallpaper dir unless it's already there */
    if (g_strcmp0 (image_path, dest) != 0) {
        GFile  *src_f  = g_file_new_for_path (image_path);
        GFile  *dst_f  = g_file_new_for_path (dest);
        GError *err    = NULL;

        g_file_copy (src_f, dst_f,
                     G_FILE_COPY_OVERWRITE | G_FILE_COPY_ALL_METADATA,
                     NULL, NULL, NULL, &err);

        if (err) {
            if (error_out)
                *error_out = g_strdup_printf ("Failed to copy wallpaper: %s",
                                              err->message);
            g_error_free (err);
            g_object_unref (src_f);
            g_object_unref (dst_f);
            ok = FALSE;
            goto cleanup;
        }

        g_object_unref (src_f);
        g_object_unref (dst_f);
    }

    /* Update .current symlink atomically via GLib -- no POSIX visibility issues */
    {
        char *tmp_link = g_strdup_printf ("%s.tmp", link_path);
        GFile *tmp_f   = g_file_new_for_path (tmp_link);
        GFile *lnk_f   = g_file_new_for_path (link_path);
        GError *lerr   = NULL;

        /* Remove stale tmp if present */
        g_file_delete (tmp_f, NULL, NULL);

        /* Create symlink: tmp_link -> dest */
        if (!g_file_make_symbolic_link (tmp_f, dest, NULL, &lerr)) {
            if (error_out)
                *error_out = g_strdup_printf ("Failed to create symlink: %s",
                                              lerr->message);
            g_error_free (lerr);
            g_object_unref (tmp_f);
            g_object_unref (lnk_f);
            g_free (tmp_link);
            ok = FALSE;
            goto cleanup;
        }

        /* Atomically rename tmp -> .current */
        if (!g_file_move (tmp_f, lnk_f,
                          G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &lerr)) {
            if (error_out)
                *error_out = g_strdup_printf ("Failed to update .current: %s",
                                              lerr->message);
            g_error_free (lerr);
            g_file_delete (tmp_f, NULL, NULL);
            g_object_unref (tmp_f);
            g_object_unref (lnk_f);
            g_free (tmp_link);
            ok = FALSE;
            goto cleanup;
        }

        g_object_unref (tmp_f);
        g_object_unref (lnk_f);
        g_free (tmp_link);
    }

cleanup:
    g_free (dir);
    g_free (basename);
    g_free (dest);
    g_free (link_path);
    return ok;
}


/* ── Smart wallpaper export ─────────────────────────────────────────────── */

/* True if `ffmpeg -encoders` lists a line containing NAME. No shell: argv only. */
static gboolean
ffmpeg_lists_encoder (const char *name)
{
    char    *argv[] = { "ffmpeg", "-hide_banner", "-encoders", NULL };
    char    *out    = NULL;
    gint     status = 0;
    gboolean found  = FALSE;

    if (g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH |
                      G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      &out, NULL, &status, NULL)
        && out && g_spawn_check_wait_status (status, NULL))
        found = strstr (out, name) != NULL;

    g_free (out);
    return found;
}

/* Probe once whether ffmpeg was built with libjxl */
static int
jxl_available (void)
{
    static int cached = -1;
    if (cached < 0)
        cached = ffmpeg_lists_encoder ("libjxl") ? 1 : 0;
    return cached;
}

gboolean
dv_wallpaper_set_smart (const char  *source_path,
                         DvEngine    *engine,
                         DvViewport  *viewport,
                         char       **error_out)
{
    if (!source_path || !*source_path) {
        if (error_out) *error_out = g_strdup ("No source path");
        return FALSE;
    }

    char *dir = dv_wallpaper_dir ();
    g_mkdir_with_parents (dir, 0755);

    gboolean has_transform = dv_viewport_is_hard (viewport) || viewport->has_crop ||
                             viewport->rotation || viewport->flip_h || viewport->flip_v;

    char   *dest     = NULL;
    gboolean ok      = TRUE;

    if (!has_transform) {
        /* No viewport change -- copy source file as-is, any format */
        char *base = g_path_get_basename (source_path);
        dest = g_build_filename (dir, base, NULL);
        g_free (base);

        GFile  *sf  = g_file_new_for_path (source_path);
        GFile  *df  = g_file_new_for_path (dest);
        GError *err = NULL;
        g_file_copy (sf, df, G_FILE_COPY_OVERWRITE | G_FILE_COPY_ALL_METADATA,
                     NULL, NULL, NULL, &err);
        g_object_unref (sf); g_object_unref (df);
        if (err) {
            if (error_out) *error_out = g_strdup (err->message);
            g_error_free (err);
            ok = FALSE;
        }
    } else if (dv_engine_is_svg (engine)) {
        /* SVG + transform: wrap with new viewBox via dv_export */
        const DvExportFormat *svg_fmt = NULL;
        for (int i = 0; i < DV_EXPORT_NFORMATS; i++) {
            if (strcmp (DV_EXPORT_FORMATS[i].ext, "svg") == 0) {
                svg_fmt = &DV_EXPORT_FORMATS[i]; break;
            }
        }
        if (svg_fmt) {
            DvExportResult *result = dv_export (source_path, svg_fmt, engine, viewport, NULL);
            if (result && result->ok && result->output_paths && result->output_paths[0]) {
                char *base = g_path_get_basename (result->output_paths[0]);
                dest = g_build_filename (dir, base, NULL);
                g_free (base);
                GFile *sf = g_file_new_for_path (result->output_paths[0]);
                GFile *df = g_file_new_for_path (dest);
                GError *err = NULL;
                g_file_move (sf, df, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err);
                g_object_unref (sf); g_object_unref (df);
                if (err) { if (error_out) *error_out = g_strdup (err->message); g_error_free (err); ok = FALSE; }
            } else {
                if (error_out) *error_out = g_strdup (result ? result->error_msg : "SVG export failed");
                ok = FALSE;
            }
            if (result) dv_export_result_free (result);
        }
    } else {
        /* Raster + transform: export to PNG (or JXL if HDR and available) */
        const char *ext = (dv_engine_is_hdr (engine) && jxl_available ()) ? "jxl" : "png";
        char *stem = g_path_get_basename (source_path);
        char *dot  = strrchr (stem, '.');
        if (dot) *dot = '\0';
        char *fname = g_strdup_printf ("%s.%s", stem, ext);
        dest = g_build_filename (dir, fname, NULL);
        g_free (stem); g_free (fname);

        /* Use dv_export to apply viewport transform and write to temp path */
        const DvExportFormat *fmt = NULL;
        for (int i = 0; i < DV_EXPORT_NFORMATS; i++) {
            if (strcmp (DV_EXPORT_FORMATS[i].ext, ext) == 0) {
                fmt = &DV_EXPORT_FORMATS[i]; break;
            }
        }
        if (!fmt) {
            if (error_out) *error_out = g_strdup_printf ("Unknown format: %s", ext);
            ok = FALSE;
        } else {
            DvExportResult *result = dv_export (source_path, fmt, engine, viewport, NULL);
            if (!result || !result->ok) {
                if (error_out)
                    *error_out = g_strdup (result ? result->error_msg : "Export failed");
                ok = FALSE;
            } else if (result->output_paths && result->output_paths[0]) {
                /* Move exported file into wallpaper dir */
                GFile  *sf  = g_file_new_for_path (result->output_paths[0]);
                GFile  *df  = g_file_new_for_path (dest);
                GError *err = NULL;
                g_file_move (sf, df, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err);
                g_object_unref (sf); g_object_unref (df);
                if (err) {
                    if (error_out) *error_out = g_strdup (err->message);
                    g_error_free (err); ok = FALSE;
                }
            }
            if (result) dv_export_result_free (result);
        }
    }

    /* Update .current symlink */
    if (ok && dest)
        ok = dv_wallpaper_set (dest, error_out);

    g_free (dest);
    g_free (dir);
    return ok;
}

char *
dv_wallpaper_get_current (void)
{
    char *dir       = dv_wallpaper_dir ();
    char *link_path = g_build_filename (dir, ".current", NULL);
    char *target    = g_file_read_link (link_path, NULL);
    g_free (dir);
    g_free (link_path);
    return target;   /* caller g_free()s */
}
