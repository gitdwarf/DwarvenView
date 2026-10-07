#include "dv_export.h"
#include <math.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <librsvg/rsvg.h>
#include <cairo.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <gio/gio.h>

/* ── Format table ────────────────────────────────────────────────────────── */

const DvExportFormat DV_EXPORT_FORMATS[] = {
    /* ext        label           animated  still_only  svg_only */
    { "png",  "PNG",              TRUE,     FALSE },
    { "jpg",  "JPEG",             FALSE,    TRUE  },
    { "webp", "WebP",             TRUE,     FALSE },
    { "gif",  "GIF",              TRUE,     FALSE },
    { "jxl",  "JPEG XL",         TRUE,     FALSE },
    { "avif", "AVIF",             TRUE,     FALSE },
    { "tiff", "TIFF",             FALSE,    TRUE  },
    { "bmp",  "BMP",              FALSE,    TRUE  },
    { "svg",  "SVG (vector)",     FALSE,    FALSE },  /* SVG->SVG: viewBox crop only */
};
const int DV_EXPORT_NFORMATS = (int)(sizeof DV_EXPORT_FORMATS /
                                     sizeof DV_EXPORT_FORMATS[0]);

/* ── Result helpers ──────────────────────────────────────────────────────── */

void
dv_export_result_free (DvExportResult *r)
{
    if (!r) return;
    g_strfreev (r->output_paths);
    g_free (r->error_msg);
    g_free (r->warning_msg);
    g_free (r);
}

static DvExportResult *
result_fail (const char *msg)
{
    DvExportResult *r = g_new0 (DvExportResult, 1);
    r->ok        = FALSE;
    r->error_msg = g_strdup (msg);
    return r;
}

static DvExportResult *
result_ok (GPtrArray *paths)
{
    g_ptr_array_add (paths, NULL);
    DvExportResult *r   = g_new0 (DvExportResult, 1);
    r->ok           = TRUE;
    r->output_paths = (char **) g_ptr_array_free (paths, FALSE);
    return r;
}

/* ── ffmpeg invocation (no shell) ────────────────────────────────────────── */

/*
 * Every ffmpeg call goes through these helpers. Arguments are handed to the
 * child as an argv array, never through a shell, so a file name can contain
 * any character (quotes, spaces, $, ;, backticks) without being interpreted.
 * DwarvenView opens arbitrary dropped files, so paths are untrusted input.
 */

/* New argv seeded with "ffmpeg -y". Elements are owned copies. */
static GPtrArray *
ffmpeg_argv_new (void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (a, g_strdup ("ffmpeg"));
    g_ptr_array_add (a, g_strdup ("-y"));
    return a;
}

/* Append NULL-terminated const char * arguments, each copied verbatim. */
static void
ffmpeg_argv_add (GPtrArray *a, ...)
{
    va_list     ap;
    const char *s;

    va_start (ap, a);
    while ((s = va_arg (ap, const char *)))
        g_ptr_array_add (a, g_strdup (s));
    va_end (ap);
}

/* "-i SRC", plus "-vf VF" when VF is set. */
static void
ffmpeg_argv_add_input (GPtrArray *a, const char *src, const char *vf)
{
    ffmpeg_argv_add (a, "-i", src, NULL);
    if (vf)
        ffmpeg_argv_add (a, "-vf", vf, NULL);
}

/*
 * Run the argv built above and free it. If STDIN_DATA is set it is written
 * to ffmpeg's stdin (otherwise stdin is /dev/null). Returns TRUE only when
 * ffmpeg started, took all the input, and exited with status 0.
 */
static gboolean
ffmpeg_run (GPtrArray *a, const guchar *stdin_data, gsize stdin_len)
{
    g_ptr_array_add (a, NULL);

    GPid     pid    = 0;
    gint     in_fd  = -1;
    GError  *err    = NULL;
    gboolean spawned = g_spawn_async_with_pipes (
        NULL, (char **) a->pdata, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
        G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, &pid,
        stdin_data ? &in_fd : NULL, NULL, NULL, &err);
    g_ptr_array_free (a, TRUE);
    if (!spawned) {
        g_clear_error (&err);
        return FALSE;
    }

    gboolean fed = TRUE;
    if (stdin_data) {
        /* ffmpeg may exit early; a write to its dead pipe must not SIGPIPE us */
        void (*old_pipe) (int) = signal (SIGPIPE, SIG_IGN);
        gsize done = 0;
        while (done < stdin_len) {
            ssize_t n = write (in_fd, stdin_data + done, stdin_len - done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) { fed = FALSE; break; }
            done += (gsize) n;
        }
        signal (SIGPIPE, old_pipe);
        close (in_fd);
    }

    int   status = 0;
    pid_t w;
    do w = waitpid (pid, &status, 0);
    while (w < 0 && errno == EINTR);
    g_spawn_close_pid (pid);

    return fed && w == pid && g_spawn_check_wait_status (status, NULL);
}

/* Move a file; across filesystems fall back to copy then delete. */
static gboolean
move_file (const char *src, const char *dst)
{
    if (g_rename (src, dst) == 0)
        return TRUE;

    GFile   *s  = g_file_new_for_path (src);
    GFile   *d  = g_file_new_for_path (dst);
    gboolean ok = g_file_copy (s, d, G_FILE_COPY_OVERWRITE,
                               NULL, NULL, NULL, NULL);
    g_object_unref (s);
    g_object_unref (d);
    g_unlink (src);
    return ok;
}

/* Delete a temp directory and any files ffmpeg left in it (also on failure). */
static void
remove_tmp_dir (const char *dir)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name (d))) {
            char *path = g_build_filename (dir, name, NULL);
            g_unlink (path);
            g_free (path);
        }
        g_dir_close (d);
    }
    g_rmdir (dir);
}

/* ── Path helpers ────────────────────────────────────────────────────────── */

/*
 * Build stem (no extension) from source basename.
 * For extensionless cursor files: just use the name as-is.
 */
/*
 * Build output path for a single-file conversion.
 */
static char *
single_output (const char    *source_path,
               const char    *ext,
               const DvPrefs *prefs)
{
    return dv_prefs_resolve_output (source_path, ext, prefs);
}

/*
 * Build output path for frame N (1-based) of a multi-frame export.
 * e.g. /out/dir/filename_001.tiff
 */
/* ── SVG export ──────────────────────────────────────────────────────────── */

/*
 * Detect the maximum safe square dimension for Cairo ARGB32 surfaces.
 * Tests by actually attempting to create surfaces until one fails.
 * Also enforces libavcodec's int32 buffer limit: dim*dim*4 <= INT32_MAX.
 * Result is cached after first call.
 */
static int
cairo_max_safe_dim (void)
{
    static int cached = 0;
    if (cached > 0) return cached;

    /* libavcodec reads PNG with 64-byte aligned strides.
     * stride = align(dim*4, 64); total = stride*dim must fit in INT32_MAX.
     * Binary search for the exact limit. */
    int av_limit = 1024;
    for (int dim = 32768; dim >= 1024; dim--) {
        int stride = ((dim * 4 + 63) / 64) * 64;
        if ((gint64) stride * dim <= G_MAXINT32) {
            av_limit = dim;
            break;
        }
    }

    /* Binary search for Cairo surface creation limit */
    int lo = 1024, hi = av_limit;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        cairo_surface_t *s = cairo_image_surface_create (
            CAIRO_FORMAT_ARGB32, mid, mid);
        gboolean ok = (cairo_surface_status (s) == CAIRO_STATUS_SUCCESS);
        cairo_surface_destroy (s);
        if (ok) lo = mid;
        else    hi = mid - 1;
    }

    cached = lo;
    return cached;
}

/*
 * Clamp SVG render dimensions to Cairo limits.
 * Returns TRUE if clamping was needed; sets *clamped_w, *clamped_h.
 * Also sets *actual_zoom to the zoom level that fits.
 */
static gboolean
clamp_svg_render_size (int src_w, int src_h, double zoom,
                       int *clamped_w, int *clamped_h, double *actual_zoom)
{
    int want_w = (int)(src_w * zoom);
    int want_h = (int)(src_h * zoom);

    if (want_w <= cairo_max_safe_dim () && want_h > 0 &&
        want_h <= cairo_max_safe_dim () && want_w > 0) {
        *clamped_w  = want_w;
        *clamped_h  = want_h;
        *actual_zoom = zoom;
        return FALSE;   /* no clamp needed */
    }

    /* Scale down proportionally to fit within Cairo limits */
    double scale = 1.0;
    if (want_w > 0 && want_h > 0) {
        double sx = (double) cairo_max_safe_dim () / want_w;
        double sy = (double) cairo_max_safe_dim () / want_h;
        scale = (sx < sy) ? sx : sy;
    }

    *clamped_w  = (int)(want_w * scale);
    *clamped_h  = (int)(want_h * scale);
    *actual_zoom = zoom * scale;
    if (*clamped_w < 1) *clamped_w = 1;
    if (*clamped_h < 1) *clamped_h = 1;
    return TRUE;   /* was clamped */
}

/* cairo_write_func_t: collect encoded PNG bytes in memory */
static cairo_status_t
png_append (void *closure, const unsigned char *data, unsigned int len)
{
    g_byte_array_append ((GByteArray *) closure, data, len);
    return CAIRO_STATUS_SUCCESS;
}

static DvExportResult *
export_svg (const char           *source_path,
            const DvExportFormat  *fmt,
            const DvViewport      *vp,
            const DvPrefs         *prefs)
{
    GError     *err  = NULL;
    RsvgHandle *svg  = rsvg_handle_new_from_file (source_path, &err);
    if (!svg) {
        DvExportResult *r = result_fail (err ? err->message : "SVG load failed");
        g_clear_error (&err);
        return r;
    }

    /* Native SVG dimensions */
    gdouble native_w = 0, native_h = 0;
    rsvg_handle_get_intrinsic_size_in_pixels (svg, &native_w, &native_h);
    if (native_w <= 0) native_w = 512;
    if (native_h <= 0) native_h = 512;

    /* Export resolution: if hard zoom, scale by zoom factor */
    double zoom  = dv_viewport_is_hard (vp) ? vp->zoom : 1.0;
    int    out_w = (int)(native_w * zoom);
    int    out_h = (int)(native_h * zoom);

    /* Clamp to Cairo surface limits -- prevents InvalidSize crash on extreme zoom */
    char   *clamp_warning = NULL;
    {
        int    clamped_w, clamped_h;
        double actual_zoom;
        if (clamp_svg_render_size (native_w, native_h, zoom,
                                   &clamped_w, &clamped_h, &actual_zoom)) {
            clamp_warning = g_strdup_printf (
                "Requested export resolution exceeds renderer limits.\n"
                "Maximum supported size: %d x %d (%.4g%%).",
                clamped_w, clamped_h, actual_zoom * 100.0);
            out_w = clamped_w;
            out_h = clamped_h;
        }
    }

    /* Render via Cairo to PNG temp file, then re-encode with ffmpeg if needed */
    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                        out_w, out_h);
    cairo_t *cr = cairo_create (surf);
    RsvgRectangle viewport = { 0, 0, (double)out_w, (double)out_h };
    rsvg_handle_render_document (svg, cr, &viewport, NULL);
    cairo_destroy (cr);
    /* Encode the PNG in memory. No temp file, so there is nothing to race on. */
    GByteArray *png = g_byte_array_new ();
    cairo_status_t st = cairo_surface_write_to_png_stream (surf, png_append, png);
    cairo_surface_destroy (surf);
    g_object_unref (svg);
    if (st != CAIRO_STATUS_SUCCESS) {
        g_byte_array_free (png, TRUE);
        return result_fail ("PNG encoding failed");
    }

    char *out_path = single_output (source_path, fmt->ext, prefs);
    if (!out_path) {
        g_byte_array_free (png, TRUE);
        return result_fail ("Could not determine output path");
    }

    gboolean ok;
    if (strcmp (fmt->ext, "png") == 0) {
        /* Already PNG -- write it out (atomic replace, never follows a symlink) */
        ok = g_file_set_contents (out_path, (const char *) png->data,
                                  (gssize) png->len, NULL);
    } else {
        /* Re-encode via ffmpeg, PNG fed on stdin */
        GPtrArray *a = ffmpeg_argv_new ();
        ffmpeg_argv_add (a, "-f", "png_pipe", "-i", "pipe:0", out_path, NULL);
        ok = ffmpeg_run (a, png->data, png->len);
    }
    g_byte_array_free (png, TRUE);

    if (!ok) {
        g_free (clamp_warning);
        g_free (out_path);
        return result_fail ("ffmpeg conversion failed");
    }

    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (paths, out_path);
    DvExportResult *res = result_ok (paths);
    res->warning_msg = clamp_warning;   /* NULL or clamping notice */
    return res;
}

/* ── Raster export ───────────────────────────────────────────────────────── */

/*
 * Build the ffmpeg scale filter string for the current viewport.
 * Returns NULL if no scaling needed (native res export).
 */
/* Build ffmpeg filter string for the current viewport transform.
 * Exports exactly what is visible in the viewport -- crop region + pan.
 * Includes rotation and flip. Returns NULL if passthrough only. */
static char *
build_transform_filter (const DvViewport *vp, int native_w, int native_h)
{
    GString *f = g_string_new ("");

    /* 1. Compute the visible source rectangle (accounts for crop + pan + zoom) */
    if (dv_viewport_is_hard (vp)) {
        double vx, vy, vw, vh;
        dv_viewport_get_export_rect (vp, native_w, native_h,
                                     &vx, &vy, &vw, &vh);

        int ix = (int)(vx + 0.5);
        int iy = (int)(vy + 0.5);
        int iw = (int)(vw + 0.5);
        int ih = (int)(vh + 0.5);

        /* Clamp to image bounds */
        if (ix < 0) { iw += ix; ix = 0; }
        if (iy < 0) { ih += iy; iy = 0; }
        if (ix + iw > native_w) iw = native_w - ix;
        if (iy + ih > native_h) ih = native_h - iy;

        /* Crop to visible region if not the full image */
        if (iw > 0 && ih > 0 &&
            (ix > 0 || iy > 0 || iw < native_w || ih < native_h)) {
            g_string_append_printf (f, "crop=%d:%d:%d:%d", iw, ih, ix, iy);
        }

        /* Scale -- output at the widget display size for pixel-perfect export */
        int out_w = vp->widget_w;
        int out_h = vp->widget_h;
        if (out_w > 0 && out_h > 0 && (iw != out_w || ih != out_h)) {
            if (f->len > 0) g_string_append_c (f, ',');
            /* zscale with spline36 -- highest quality resampling.
             * unsharp restores detail lost in upscaling. */
            g_string_append_printf (f,
                "zscale=%d:%d:filter=spline36:dither=error_diffusion,"
                "unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=0.8",
                out_w, out_h);
        }
    }

    /* 2. Rotation */
    if (vp->rotation == 90) {
        if (f->len > 0) g_string_append_c (f, ',');
        g_string_append (f, "transpose=1");
    } else if (vp->rotation == 180) {
        if (f->len > 0) g_string_append_c (f, ',');
        g_string_append (f, "hflip,vflip");
    } else if (vp->rotation == 270) {
        if (f->len > 0) g_string_append_c (f, ',');
        g_string_append (f, "transpose=2");
    }

    /* 3. Flip */
    if (vp->flip_h) {
        if (f->len > 0) g_string_append_c (f, ',');
        g_string_append (f, "hflip");
    }
    if (vp->flip_v) {
        if (f->len > 0) g_string_append_c (f, ',');
        g_string_append (f, "vflip");
    }

    if (f->len == 0) {
        g_string_free (f, TRUE);
        return NULL;
    }

    return g_string_free (f, FALSE);
}

/* Legacy wrapper for callsites that only care about scale/crop */
static char *
build_scale_filter (const DvViewport *vp, int native_w, int native_h)
{
    return build_transform_filter (vp, native_w, native_h);
}

static DvExportResult *
export_raster_static (const char           *source_path,
                      const DvExportFormat  *fmt,
                      const DvViewport      *vp,
                      int                   native_w,
                      int                   native_h,
                      const DvPrefs         *prefs)
{
    char *out_path = single_output (source_path, fmt->ext, prefs);
    if (!out_path)
        return result_fail ("Could not determine output path");

    char *scale = build_scale_filter (vp, native_w, native_h);
    GPtrArray *a = ffmpeg_argv_new ();
    ffmpeg_argv_add_input (a, source_path, scale);
    ffmpeg_argv_add (a, out_path, NULL);
    g_free (scale);
    gboolean ok = ffmpeg_run (a, NULL, 0);

    if (!ok) {
        g_free (out_path);
        return result_fail ("ffmpeg conversion failed");
    }

    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (paths, out_path);
    return result_ok (paths);
}

static DvExportResult *
export_raster_animated_to_animated (const char           *source_path,
                                    const DvExportFormat  *fmt,
                                    const DvViewport      *vp,
                                    int                   native_w,
                                    int                   native_h,
                                    const DvPrefs         *prefs)
{
    char *out_path = single_output (source_path, fmt->ext, prefs);
    if (!out_path)
        return result_fail ("Could not determine output path");

    char *scale = build_scale_filter (vp, native_w, native_h);

    GPtrArray *a = ffmpeg_argv_new ();
    if (strcmp (fmt->ext, "gif") == 0) {
        /* GIF carries its own filter graph; the scale filter is embedded in it.
         * No -map: the graph's single free output is selected automatically
         * (mapping [s1] fails, that label is already consumed by paletteuse). */
        char *pre = scale ? g_strdup_printf ("%s,", scale) : g_strdup ("");
        char *fc  = g_strdup_printf (
            "[0:v]%ssplit[s0][s1];[s0]palettegen=stats_mode=diff[p];"
            "[s1][p]paletteuse=dither=bayer", pre);
        ffmpeg_argv_add_input (a, source_path, NULL);
        ffmpeg_argv_add (a, "-filter_complex", fc, "-an", NULL);
        g_free (pre);
        g_free (fc);
    } else {
        ffmpeg_argv_add_input (a, source_path, scale);
        if (strcmp (fmt->ext, "png") == 0)
            ffmpeg_argv_add (a, "-f", "apng", NULL);
        if (strcmp (fmt->ext, "webp") == 0)
            ffmpeg_argv_add (a, "-lossless", "0", "-quality", "90", NULL);
    }
    ffmpeg_argv_add (a, out_path, NULL);
    g_free (scale);
    gboolean ok = ffmpeg_run (a, NULL, 0);

    if (!ok) {
        g_free (out_path);
        return result_fail ("ffmpeg animated conversion failed");
    }

    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (paths, out_path);
    return result_ok (paths);
}

static DvExportResult *
export_raster_animated_per_frame (const char           *source_path,
                                  const DvExportFormat  *fmt,
                                  const DvViewport      *vp,
                                  int                   native_w,
                                  int                   native_h,
                                  const DvPrefs         *prefs)
{
    /* Don't probe -- just extract and count actual output files */
    int total = 1000;   /* generous upper bound; loop breaks on first missing file */

    /* Build ffmpeg output pattern using a temp dir then rename */
    char *tmp_dir = g_dir_make_tmp ("dv_frames_XXXXXX", NULL);
    if (!tmp_dir)
        return result_fail ("Could not create temp directory");

    char *scale = build_scale_filter (vp, native_w, native_h);
    char *pattern = g_strdup_printf ("%s/%%06d.%s", tmp_dir, fmt->ext);
    GPtrArray *a = ffmpeg_argv_new ();
    ffmpeg_argv_add_input (a, source_path, scale);
    ffmpeg_argv_add (a, pattern, NULL);
    g_free (scale);
    g_free (pattern);
    gboolean ok = ffmpeg_run (a, NULL, 0);

    if (!ok) {
        remove_tmp_dir (tmp_dir);
        g_free (tmp_dir);
        return result_fail ("ffmpeg frame extraction failed");
    }

    /* Resolve output directory once -- not per-frame */
    char *ref     = dv_prefs_resolve_output (source_path, fmt->ext, prefs);
    char *out_dir = ref ? g_path_get_dirname (ref) : g_path_get_dirname (source_path);
    g_free (ref);

    char *stem  = g_path_get_basename (source_path);
    char *dot   = strrchr (stem, '.');
    if (dot) *dot = '\0';

    int digits = 3;   /* adjusted after counting frames */
    (void) total;

    /* Rename temp files to final paths */
    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);

    for (int fi = 1; fi <= total + 10; fi++) {
        char *tmp_frame = g_strdup_printf ("%s/%06d.%s", tmp_dir, fi, fmt->ext);
        if (!g_file_test (tmp_frame, G_FILE_TEST_EXISTS)) {
            g_free (tmp_frame);
            break;
        }
        char *fname = g_strdup_printf ("%s_%0*d.%s", stem, digits, fi, fmt->ext);
        char *out   = g_build_filename (out_dir, fname, NULL);
        g_free (fname);
        gboolean moved = move_file (tmp_frame, out);
        g_free (tmp_frame);
        if (!moved) {
            g_free (out);
            continue;
        }
        g_ptr_array_add (paths, out);
    }

    g_free (stem);
    g_free (out_dir);

    remove_tmp_dir (tmp_dir);
    g_free (tmp_dir);

    if (paths->len == 0) {
        g_ptr_array_free (paths, TRUE);
        return result_fail ("No frames extracted");
    }

    return result_ok (paths);
}

/* ── Public entry point ──────────────────────────────────────────────────── */

/* ── Xcursor export ──────────────────────────────────────────────────────── */

static DvExportResult *
export_xcursor (const char           *source_path,
                const DvExportFormat  *fmt,
                DvEngine              *engine,
                const DvViewport      *vp,
                const DvPrefs         *prefs)
{
    (void) vp;   /* Xcursor: no viewport scaling, always native res */
    int native_w = 0, native_h = 0;
    dv_engine_get_size (engine, &native_w, &native_h);
    if (native_w <= 0 || native_h <= 0)
        return result_fail ("Xcursor: invalid dimensions");

    /* Get the pixel data from the texture via Cairo */
    GdkTexture *tex = dv_engine_get_current_texture (engine);
    if (!tex)
        return result_fail ("Xcursor: no texture available");

    /* Download texture to raw RGBA bytes */
    gsize   stride    = (gsize)(native_w * 4);
    gsize   data_size = stride * (gsize)native_h;
    guchar *pixels    = g_malloc (data_size);
    gdk_texture_download (tex, pixels, stride);

    /* Pipe RGBA pixels to ffmpeg via stdin */
    char *out_path = single_output (source_path, fmt->ext, prefs);
    if (!out_path) {
        g_free (pixels);
        return result_fail ("Could not determine output path");
    }

    char *size = g_strdup_printf ("%dx%d", native_w, native_h);
    GPtrArray *a = ffmpeg_argv_new ();
    ffmpeg_argv_add (a, "-f", "rawvideo", "-pixel_format", "rgba",
                     "-video_size", size, "-framerate", "1",
                     "-i", "pipe:0", out_path, NULL);
    g_free (size);
    gboolean ok = ffmpeg_run (a, pixels, data_size);
    g_free (pixels);

    if (!ok) {
        g_free (out_path);
        return result_fail ("Xcursor: ffmpeg pipe failed");
    }

    GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (paths, out_path);
    return result_ok (paths);
}




/* ── SVG -> SVG viewBox crop export ────────────────────────────────────── */

static DvExportResult *
export_svg_viewbox (const char *source_path, const DvViewport *vp,
                    const DvPrefs *prefs)
{
    char *src_content = NULL;
    if (!g_file_get_contents (source_path, &src_content, NULL, NULL)) {
        DvExportResult *r = g_new0 (DvExportResult, 1);
        r->error_msg = g_strdup ("Could not read SVG source");
        return r;
    }

    /* Determine viewBox -- crop region or full image */
    double vx, vy, vw, vh;
    if (vp->has_crop) {
        vx = vp->crop_x; vy = vp->crop_y;
        vw = vp->crop_w; vh = vp->crop_h;
    } else {
        vx = 0; vy = 0;
        vw = vp->image_w > 0 ? vp->image_w : 100;
        vh = vp->image_h > 0 ? vp->image_h : 100;
    }

    /* Skip XML declaration and opening <svg...> tag to get just the children.
     * This prevents nested <svg> in the output. */
    const char *body_start = src_content;

    /* Skip <?xml ...?> declaration if present */
    if (g_str_has_prefix (body_start, "<?")) {
        const char *end_pi = strstr (body_start, "?>");
        if (end_pi) body_start = end_pi + 2;
    }

    /* Skip whitespace and comments until we hit <svg */
    const char *svg_open = strstr (body_start, "<svg");
    if (svg_open) {
        /* Skip to the end of the opening <svg...> tag */
        const char *svg_end = strchr (svg_open, '>');
        if (svg_end) body_start = svg_end + 1;
    }

    /* Find end -- last </svg> closes the original root element */
    const char *body_end = g_strrstr (src_content, "</svg>");
    if (!body_end) body_end = src_content + strlen (src_content);

    char *inner   = g_strndup (body_start, (gsize)(body_end - body_start));
    char *out_svg = g_strdup_printf (
        "<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
        "     viewBox=\"%.6g %.6g %.6g %.6g\">\n"
        "%s\n"
        "</svg>\n",
        vx, vy, vw, vh, inner);
    g_free (inner);
    g_free (src_content);

    char *out_path = dv_prefs_resolve_output (source_path, "svg", prefs);
    GError *err = NULL;
    g_file_set_contents (out_path, out_svg, -1, &err);
    g_free (out_svg);

    DvExportResult *result = g_new0 (DvExportResult, 1);
    if (err) {
        result->error_msg = g_strdup (err->message);
        g_error_free (err);
        g_free (out_path);
    } else {
        result->ok           = TRUE;
        result->output_paths = g_new0 (char *, 2);
        result->output_paths[0] = out_path;
    }
    return result;
}

DvExportResult *
dv_export (const char           *source_path,
           const DvExportFormat  *fmt,
           DvEngine              *engine,
           const DvViewport      *vp,
           const DvPrefs         *prefs)
{
    /* SVG -> SVG: viewBox envelope export -- no rasterisation */
    if (strcmp (fmt->ext, "svg") == 0)
        return export_svg_viewbox (source_path, vp, prefs);

    int native_w = 0, native_h = 0;
    dv_engine_get_size (engine, &native_w, &native_h);
    if (native_w <= 0) native_w = 1;
    if (native_h <= 0) native_h = 1;

    /* Xcursor: detect by magic bytes -- ffmpeg cannot read this format */
    {
        guint8 magic[4] = {0};
        FILE *mf = fopen (source_path, "rb");
        if (mf) { fread (magic, 1, 4, mf); fclose (mf); }
        if (memcmp (magic, "Xcur", 4) == 0)
            return export_xcursor (source_path, fmt, engine, vp, prefs);
    }

    /* SVG: use librsvg renderer */
    if (dv_engine_is_svg (engine))
        return export_svg (source_path, fmt, vp, prefs);

    /* Animated source */
    if (dv_engine_is_animated (engine)) {
        if (fmt->still_only)
            return export_raster_animated_per_frame (
                source_path, fmt, vp, native_w, native_h, prefs);
        else
            return export_raster_animated_to_animated (
                source_path, fmt, vp, native_w, native_h, prefs);
    }

    /* Static raster */
    return export_raster_static (source_path, fmt, vp, native_w, native_h, prefs);
}
