#include "dv_engine.h"
#include "dv_placeholder.h"
#include "dv_debug.h"
#include "dv_surface_tex.h"
#include "dv_webp_anim.h"
#include "dv_viewable.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gdk/gdk.h>
#include <librsvg/rsvg.h>
#include <cairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libavcodec / libavformat / libswscale */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* Check if this load is still current -- used in load thread to abort stale work */
#define LOAD_IS_STALE(eng, gen) \
    (g_atomic_int_get (&(eng)->load_gen) != (gen))

/* ── Fast dimension reader ───────────────────────────────────────────────── */

/*
 * Read image dimensions directly from file headers, bypassing libavcodec.
 * Returns TRUE if dimensions were read, FALSE if format not recognised.
 * Supports PNG (IHDR at fixed offset) and JPEG (SOF marker scan).
 */
static gboolean
read_image_dims_fast (const char *path, int *out_w, int *out_h)
{
    dv_debug_log ("read_image_dims_fast: opening %s", path);
    FILE *f = fopen (path, "rb");
    if (!f) {
        dv_debug_log ("read_image_dims_fast: fopen failed: %s", path);
        return FALSE;
    }

    guint8 buf[32] = {0};
    size_t n = fread (buf, 1, sizeof (buf), f);
    fclose (f);
    dv_debug_log ("read_image_dims_fast: read %zu bytes", n);
    if (n < 24) return FALSE;

    /* PNG: signature 8 bytes, IHDR chunk: 4 len + 4 "IHDR" + 4 width + 4 height */
    static const guint8 png_sig[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    if (memcmp (buf, png_sig, 8) == 0 && memcmp (buf+12, "IHDR", 4) == 0) {
        *out_w = (int)((buf[16]<<24)|(buf[17]<<16)|(buf[18]<<8)|buf[19]);
        *out_h = (int)((buf[20]<<24)|(buf[21]<<16)|(buf[22]<<8)|buf[23]);
        dv_debug_log ("read_image_dims_fast: PNG header says %dx%d", *out_w, *out_h);
        return (*out_w > 0 && *out_h > 0);
    }

    return FALSE;
}


/* ── ffmpeg CLI fallback for oversized images ───────────────────────────── */

/* Maximum dimension libavcodec can decode (stride-aligned).
 * Images larger than this are decoded via ffmpeg CLI pipe instead. */
static int
av_max_decode_dim (void)
{
    static int cached = 0;
    if (cached > 0) return cached;
    /* Largest dim where align(dim*4,64)*dim <= INT32_MAX */
    for (int dim = 32768; dim >= 1024; dim--) {
        int stride = ((dim * 4 + 63) / 64) * 64;
        if ((gint64) stride * dim <= G_MAXINT32) {
            cached = dim;
            return cached;
        }
    }
    cached = 8192;
    return cached;
}

/*
 * Decode an oversized image using GdkPixbuf (libpng backend).
 * GdkPixbuf loads PNG row-by-row and scales during load, so it never
 * allocates the full uncompressed buffer -- unlike libavcodec which
 * decodes the entire image before sws_scale can reduce it.
 * Thread-safe for file reading.
 */
static GdkTexture *
decode_via_ffmpeg_pipe (const char *path, int out_w, int out_h)
{
    dv_debug_log ("decode_via_ffmpeg_pipe: gdk_pixbuf_new_from_file_at_scale "
                 "%s -> %dx%d, starting", path, out_w, out_h);
    GError    *err    = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale (
                            path, out_w, out_h, TRUE, &err);
    dv_debug_log ("decode_via_ffmpeg_pipe: gdk_pixbuf_new_from_file_at_scale "
                 "returned, pixbuf=%s", pixbuf ? "ok" : "NULL");
    if (!pixbuf) {
        if (err) { dv_debug_log ("decode_via_ffmpeg_pipe: %s", err->message); g_error_free (err); }
        return NULL;
    }

    /* Ensure RGBA -- libpng may give RGB without alpha */
    GdkPixbuf *rgba = pixbuf;
    if (gdk_pixbuf_get_n_channels (pixbuf) != 4 ||
        !gdk_pixbuf_get_has_alpha (pixbuf)) {
        rgba = gdk_pixbuf_add_alpha (pixbuf, FALSE, 0, 0, 0);
        g_object_unref (pixbuf);
    }

    int    w      = gdk_pixbuf_get_width  (rgba);
    int    h      = gdk_pixbuf_get_height (rgba);
    int    stride = gdk_pixbuf_get_rowstride (rgba);
    guchar *pixels = gdk_pixbuf_get_pixels (rgba);

    GBytes     *bytes = g_bytes_new (pixels, (gsize)(h * stride));
    dv_debug_log ("decode_via_ffmpeg_pipe: gdk_memory_texture_new %dx%d stride=%d, starting", w, h, stride);
    GdkTexture *tex   = gdk_memory_texture_new (w, h,
                            GDK_MEMORY_R8G8B8A8, bytes, (gsize) stride);
    dv_debug_log ("decode_via_ffmpeg_pipe: gdk_memory_texture_new returned");
    g_bytes_unref (bytes);
    g_object_unref (rgba);
    return tex;
}


/* ── Resolution ladder ──────────────────────────────────────────────────── */

/* Maximum display decode resolution (longest dimension).
 * Images larger than this are downsampled for display only.
 * Exports always use the original file directly -- no quality loss. */
#define DV_MAX_DISPLAY_PX 4096

const int DV_LADDER[] = {
    /* Small rungs first -- fast preview for large images */
    128, 256, 640, 1080, 1920, 2560, 3840, 5760, 7680, 10240, 12288, 14336, 16384, 0
};

/* ── Internal structures ────────────────────────────────────────────────── */

typedef struct {
    GdkTexture     *texture;
    int             pass_width;
    int             native_w;
    int             native_h;
    gboolean        is_final;
    gboolean        failed;   /* explicit: texture == NULL alone means nothing (SVG sends none) */
    DvEnginePassCb  cb;
    gpointer        user_data;
    DvEngine       *eng;
    gint            gen;   /* generation at dispatch time */
    guint           source_id;   /* the idle source delivering it, so shutdown can cancel it */
} PassResult;

struct _DvEngine {
    /* Current image state */
    GMutex          lock;
    char           *current_path;
    gboolean        is_svg;
    gboolean        is_hdr;
    gboolean        is_animated;

    /* Native dimensions (set after first pass) */
    int             native_w;
    int             native_h;

    /* Animation frames */
    GPtrArray      *frames;       /* GdkTexture* -- owned */
    GArray         *frame_delays; /* guint milliseconds each frame stays up, parallel to frames */
    guint           frame_idx;
    guint           anim_timer;

    /* SVG */
    RsvgHandle     *svg_handle;
    GdkTexture     *svg_texture;
    int             svg_last_w;
    int             svg_last_h;

    /* Progressive load threads. cancel belongs to the CURRENT load only: each load
     * gets a fresh one (never g_cancellable_reset on one a thread may be reading),
     * and the thread holds its own reference. active_loads counts running load
     * threads so dv_engine_free can wait for all of them, not just the newest. */
    GCancellable   *cancel;
    gint            active_loads;
    GCond           loads_done;
    GHashTable     *pending;      /* PassResult* posted to the main loop, not yet delivered */
    gint            load_gen;    /* incremented on each new load; stale threads discard results */
    gboolean        is_placeholder; /* TRUE only while the placeholder is actually being shown */
    gboolean        is_failed;      /* TRUE once every decode stage has given up for this load */
    GdkTexture     *placeholder_tex; /* pre-rendered, created on main thread */
    GdkTexture     *audio_icon_tex;  /* shown for audio with no picture; set from the main thread, guarded by lock */
    guint           placeholder_timer;   /* pending delayed-placeholder timer, 0 if none */
    DvEnginePassCb  pending_cb;          /* cb for the current load, used by the timer */
    gpointer        pending_user_data;   /* user_data for the current load, used by the timer */
};

/* ── Forward declarations ───────────────────────────────────────────────── */

static gpointer load_thread_func (gpointer data);
static gboolean deliver_idle     (gpointer data);
void            dv_engine_start_anim (DvEngine *eng);
static GdkTexture *dv_load_xcursor  (const char *path);
static GdkTexture *decode_pass_av (GCancellable *cancel, AVFormatContext *fmt_ctx,
                                   int video_stream, int target_w,
                                   int native_w, int native_h);

/* The one way a result reaches the main loop. Registered in eng->pending so
 * dv_engine_free can cancel anything still queued when the engine goes. */
static void
post_result (DvEngine *eng, DvEnginePassCb cb, gpointer user_data, gint gen,
             GdkTexture *texture, int pass_width, int native_w, int native_h,
             gboolean is_final, gboolean failed)
{
    PassResult *r = g_new0 (PassResult, 1);
    r->texture    = texture;
    r->pass_width = pass_width;
    r->native_w   = native_w;
    r->native_h   = native_h;
    r->is_final   = is_final;
    r->failed     = failed;
    r->cb         = cb;
    r->user_data  = user_data;
    r->eng        = eng;
    r->gen        = gen;
    g_mutex_lock (&eng->lock);
    r->source_id = g_idle_add (deliver_idle, r);
    g_hash_table_add (eng->pending, r);
    g_mutex_unlock (&eng->lock);
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

DvEngine *
dv_engine_new (void)
{
    DvEngine *eng = g_new0 (DvEngine, 1);
    g_mutex_init (&eng->lock);
    g_cond_init (&eng->loads_done);
    eng->pending         = g_hash_table_new (NULL, NULL);
    eng->cancel          = g_cancellable_new ();
    eng->placeholder_tex = NULL;   /* built on first use via dv_engine_init_placeholder */
    eng->frames  = g_ptr_array_new_with_free_func (g_object_unref);
    eng->frame_delays = g_array_new (FALSE, FALSE, sizeof (guint));
    return eng;
}

void
dv_engine_free (DvEngine *eng)
{
    /* Stop any pending timers first -- their callbacks reference eng,
     * and it's about to be freed below. anim_timer had no cleanup here
     * before this change; that was a pre-existing dangling-source risk
     * for the same reason placeholder_timer would have had one if this
     * weren't added now. */
    if (eng->anim_timer) {
        g_source_remove (eng->anim_timer);
        eng->anim_timer = 0;
    }
    if (eng->placeholder_timer) {
        g_source_remove (eng->placeholder_timer);
        eng->placeholder_timer = 0;
    }
    dv_engine_cancel (eng);
    g_mutex_lock (&eng->lock);
    while (eng->active_loads > 0)
        g_cond_wait (&eng->loads_done, &eng->lock);
    /* No thread can post any more. Cancel what is still queued for the main loop:
     * those results point at eng, which is about to go. */
    GHashTableIter it;
    gpointer       key;
    g_hash_table_iter_init (&it, eng->pending);
    while (g_hash_table_iter_next (&it, &key, NULL)) {
        PassResult *r = key;
        g_source_remove (r->source_id);
        if (r->texture) g_object_unref (r->texture);
        g_free (r);
    }
    g_hash_table_destroy (eng->pending);
    g_mutex_unlock (&eng->lock);
    g_object_unref (eng->cancel);
    g_ptr_array_free (eng->frames, TRUE);
    g_array_free (eng->frame_delays, TRUE);
    g_clear_object (&eng->placeholder_tex);
    g_clear_object (&eng->audio_icon_tex);
    g_clear_object (&eng->svg_handle);
    g_clear_object (&eng->svg_texture);
    g_free (eng->current_path);
    g_cond_clear (&eng->loads_done);
    g_mutex_clear (&eng->lock);
    g_free (eng);
}

/* ── Load entry point ───────────────────────────────────────────────────── */

typedef struct {
    DvEngine       *eng;
    char           *path;
    DvEnginePassCb  cb;
    gpointer        user_data;
    gint            gen;   /* load generation at time of dispatch */
    GCancellable   *cancel;   /* this load's own; the thread owns one reference */
} LoadArgs;

/*
 * Fires after the user's configured delay if the load is still in
 * progress. Delivers the placeholder texture/overlay for the first time
 * -- deferred delivery, not immediate. If the real image already
 * finished before this fires, deliver_idle will have already cancelled
 * this timer (see its final-pass handling), so this callback only ever
 * runs while a load is genuinely still pending.
 */
static gboolean
on_placeholder_timer (gpointer data)
{
    DvEngine *eng = data;
    eng->placeholder_timer = 0;   /* GLib removes the source on FALSE return */

    if (eng->placeholder_tex && eng->pending_cb) {
        eng->is_placeholder = TRUE;
        eng->pending_cb (eng->placeholder_tex, 0,
            gdk_texture_get_width  (eng->placeholder_tex),
            gdk_texture_get_height (eng->placeholder_tex),
            FALSE, eng->pending_user_data);
    }
    return G_SOURCE_REMOVE;
}

/* An SVG that parses but draws nothing (no shapes, only stray text) is as
 * useless to look at as a corrupt one. Renders a small preview and looks for
 * any non-transparent pixel. */
static gboolean
svg_draws_nothing (RsvgHandle *h)
{
    const int N = 128;
    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, N, N);
    cairo_t *cr = cairo_create (surf);
    RsvgRectangle vp = { 0, 0, (double) N, (double) N };
    gboolean ok = rsvg_handle_render_document (h, cr, &vp, NULL);
    cairo_destroy (cr);
    cairo_surface_flush (surf);
    const guint8 *d = cairo_image_surface_get_data (surf);
    int stride = cairo_image_surface_get_stride (surf);
    gboolean any = FALSE;
    for (int y = 0; y < N && !any; y++)
        for (int x = 0; x < N; x++)
            if (d[y * stride + x * 4 + 3]) { any = TRUE; break; }
    cairo_surface_destroy (surf);
    return ok && !any;
}

void
dv_engine_load (DvEngine      *eng,
                const char    *path,
                DvEnginePassCb cb,
                gpointer       user_data,
                gboolean       show_placeholder,
                double         placeholder_delay)
{
    /* Cancel previous load -- increment generation so stale callbacks are ignored.
     * Detach the old thread rather than joining -- avoids blocking the main thread
     * while a large image is still decoding. */
    g_atomic_int_inc (&eng->load_gen);
    /* Always starts FALSE -- becomes TRUE only when the placeholder is
     * actually delivered, either immediately below or later via the
     * delayed timer. Never true during the delay window itself. */
    eng->is_placeholder = FALSE;
    eng->is_failed      = FALSE;   /* a new attempt clears any earlier failure banner */
    eng->is_hdr         = FALSE;   /* HDR is a property of one image, never carried to the next */
    dv_engine_cancel (eng);   /* the old thread keeps its own reference to the cancelled object */

    /* Stop animation timer */
    if (eng->anim_timer) {
        g_source_remove (eng->anim_timer);
        eng->anim_timer = 0;
    }

    /* Stop any pending delayed-placeholder timer from a previous load */
    if (eng->placeholder_timer) {
        g_source_remove (eng->placeholder_timer);
        eng->placeholder_timer = 0;
    }

    /* Reset state */
    g_mutex_lock (&eng->lock);
    g_ptr_array_set_size (eng->frames, 0);
    g_array_set_size (eng->frame_delays, 0);
    eng->frame_idx  = 0;
    g_atomic_int_set (&eng->is_animated, FALSE);
    eng->native_w   = 0;
    eng->native_h   = 0;
    g_free (eng->current_path);
    eng->current_path = g_strdup (path);
    g_object_unref (eng->cancel);
    eng->cancel = g_cancellable_new ();
    g_mutex_unlock (&eng->lock);

    /* SVG: detect by content not extension -- any XML with <svg tag */
    gboolean _is_svg = FALSE;
    {
        FILE *_sf = fopen (path, "rb");
        if (_sf) {
            char _buf[512] = {0};
            int  _n = (int) fread (_buf, 1, 511, _sf);
            fclose (_sf);
            for (int _i = 0; _i < _n - 3; _i++) {
                if (_buf[_i]   == '<' &&
                    (_buf[_i+1]=='s'||_buf[_i+1]=='S') &&
                    (_buf[_i+2]=='v'||_buf[_i+2]=='V') &&
                    (_buf[_i+3]=='g'||_buf[_i+3]=='G')) {
                    _is_svg = TRUE; break;
                }
            }
        }
    }
    if (_is_svg) {
        g_clear_object (&eng->svg_handle);
        g_clear_object (&eng->svg_texture);
        eng->svg_last_w  = 0;
        eng->svg_last_h  = 0;
        eng->is_svg      = TRUE;

        GError *err = NULL;
        dv_debug_log ("load: rsvg_handle_new_from_file %s, starting", path);
        eng->svg_handle = rsvg_handle_new_from_file (path, &err);
        dv_debug_log ("load: rsvg_handle_new_from_file returned, handle=%s",
                      eng->svg_handle ? "ok" : "NULL");
        if (err) {
            dv_debug_log ("load: SVG load error: %s", err->message);
            g_warning ("SVG load error: %s", err->message);
            g_error_free (err);
        }
        if (eng->svg_handle && svg_draws_nothing (eng->svg_handle)) {
            dv_debug_log ("load: SVG draws nothing, treating as invalid");
            g_clear_object (&eng->svg_handle);
        }
        /* An SVG that failed is shown like any other failure: through the
         * raster path (placeholder texture), not the SVG renderer. */
        if (!eng->svg_handle)
            eng->is_svg = FALSE;
        /* Get intrinsic dimensions for zoom calculations */
        if (eng->svg_handle) {
            gdouble iw = 0, ih = 0;
            dv_debug_log ("load: rsvg_handle_get_intrinsic_size_in_pixels, starting");
            gboolean has_wh = rsvg_handle_get_intrinsic_size_in_pixels (
                eng->svg_handle, &iw, &ih);
            dv_debug_log ("load: rsvg_handle_get_intrinsic_size_in_pixels returned "
                         "has_wh=%d %.0fx%.0f", has_wh, iw, ih);
            if (has_wh && iw > 0 && ih > 0) {
                eng->native_w = (int) iw;
                eng->native_h = (int) ih;
            } else {
                /* Fallback: use a sensible default */
                eng->native_w = 1024;
                eng->native_h = 1024;
            }
        }
        /* For SVG, fire cb with native dimensions -- widget will call get_svg() */
        if (cb) {
            gint cur_gen = g_atomic_int_get (&eng->load_gen);
            post_result (eng, cb, user_data, cur_gen,
                         NULL, 0, eng->native_w, eng->native_h,
                         TRUE, (eng->svg_handle == NULL));
        }
        return;
    }


    /* Xcursor: detect by magic bytes (files have no extension) */
    {
        guint8 magic[4] = {0};
        FILE *mf = fopen (path, "rb");
        if (mf) {
            fread (magic, 1, 4, mf);
            fclose (mf);
        }
        if (memcmp (magic, "Xcur", 4) == 0) {
            GdkTexture *tex = dv_load_xcursor (path);
            if (tex) {
                int w = gdk_texture_get_width (tex);
                int h = gdk_texture_get_height (tex);
                g_mutex_lock (&eng->lock);
                eng->native_w = w;
                eng->native_h = h;
                g_mutex_unlock (&eng->lock);
                if (cb) {
                    post_result (eng, cb, user_data, g_atomic_int_get (&eng->load_gen),
                                 tex, w, w, h,
                                 TRUE, FALSE);
                } else {
                    g_object_unref (tex);
                }
                return;
            }
        }
    }

    eng->is_svg = FALSE;

    /* Raster: spawn progressive load thread */
    LoadArgs *args  = g_new0 (LoadArgs, 1);
    args->eng       = eng;
    args->path      = g_strdup (path);
    args->cb        = cb;
    args->user_data = user_data;
    args->gen       = g_atomic_int_get (&eng->load_gen);
    args->cancel    = g_object_ref (eng->cancel);

    /* Placeholder delivery -- three cases:
     *   show_placeholder FALSE          -- never shown, nothing to do.
     *   show_placeholder TRUE, delay<=0 -- shown immediately (unchanged
     *                                      behavior from before the delay
     *                                      setting existed).
     *   show_placeholder TRUE, delay>0  -- deferred: previous image/blank
     *                                      stays visible until the timer
     *                                      fires (on_placeholder_timer) or
     *                                      the real image arrives first
     *                                      (deliver_idle cancels the timer
     *                                      on the final pass), whichever
     *                                      is sooner.
     * gen is already set above so a stale check on the deferred path
     * cannot discard it -- the timer is unconditionally cancelled at the
     * top of the next dv_engine_load call regardless. */
    if (show_placeholder) {
        eng->pending_cb        = cb;
        eng->pending_user_data = user_data;
        if (placeholder_delay > 0.0) {
            eng->placeholder_timer = g_timeout_add (
                (guint) (placeholder_delay * 1000.0),
                on_placeholder_timer, eng);
        } else if (eng->placeholder_tex && cb) {
            eng->is_placeholder = TRUE;
            cb (eng->placeholder_tex, 0,
                gdk_texture_get_width  (eng->placeholder_tex),
                gdk_texture_get_height (eng->placeholder_tex),
                FALSE, user_data);
        }
    }

    g_mutex_lock (&eng->lock);
    eng->active_loads++;
    g_mutex_unlock (&eng->lock);
    g_thread_unref (g_thread_new ("dv-load", load_thread_func, args));   /* detached; counted in active_loads */
}

void
dv_engine_cancel (DvEngine *eng)
{
    /* Signal the load thread to stop -- it checks cancellable between frames */
    g_cancellable_cancel (eng->cancel);
}


/* ── Xcursor loader ─────────────────────────────────────────────────────── */

/* Xcursor format constants */
#define XCUR_MAGIC       "Xcur"
#define XCUR_IMAGE_TYPE  0xfffd0002u

static GdkTexture *
dv_load_xcursor (const char *path)
{
    dv_debug_log ("dv_load_xcursor: opening %s (DwarvenView's own binary parser)", path);
    FILE *f = fopen (path, "rb");
    if (!f) { dv_debug_log ("dv_load_xcursor: fopen failed"); return NULL; }

    /* Read header: magic(4) + header_size(4) + version(4) + ntoc(4) */
    guint8  hdr[16];
    if (fread (hdr, 1, 16, f) < 16 || memcmp (hdr, "Xcur", 4) != 0) {
        dv_debug_log ("dv_load_xcursor: header invalid or too short");
        fclose (f); return NULL;
    }
    dv_debug_log ("dv_load_xcursor: header ok, parsing table of contents");

    guint32 ntoc;
    memcpy (&ntoc, hdr + 12, 4);  /* little-endian */

    /* Read TOC -- find best (largest) image chunk */
    guint32 best_subtype = 0;
    guint32 best_pos     = 0;

    for (guint32 i = 0; i < ntoc && i < 256; i++) {
        guint8  entry[12];
        if (fread (entry, 1, 12, f) < 12) break;
        guint32 chunk_type, subtype, position;
        memcpy (&chunk_type, entry,     4);
        memcpy (&subtype,    entry + 4, 4);
        memcpy (&position,   entry + 8, 4);
        if (chunk_type == XCUR_IMAGE_TYPE && subtype > best_subtype) {
            best_subtype = subtype;
            best_pos     = position;
        }
    }

    if (!best_pos) { fclose (f); return NULL; }

    /* Read image chunk header (36 bytes):
     * header_size(4) type(4) subtype(4) version(4) width(4) height(4)
     * xhot(4) yhot(4) delay(4) */
    fseek (f, (long) best_pos, SEEK_SET);
    guint8 img_hdr[36];
    if (fread (img_hdr, 1, 36, f) < 36) { fclose (f); return NULL; }

    guint32 img_hdr_size, width, height;
    memcpy (&img_hdr_size, img_hdr,      4);
    memcpy (&width,        img_hdr + 16, 4);
    memcpy (&height,       img_hdr + 20, 4);

    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        fclose (f); return NULL;
    }

    /* Pixel data: ARGB uint32 little-endian, one per pixel */
    guint32 n_pixels = width * height;
    guint32 *argb    = g_malloc (n_pixels * 4);

    fseek (f, (long)(best_pos + img_hdr_size), SEEK_SET);
    if (fread (argb, 4, n_pixels, f) < n_pixels) {
        g_free (argb); fclose (f); return NULL;
    }
    fclose (f);

    /* Convert ARGB (cairo/X11 order) to RGBA (GTK4/GDK order) */
    guint8 *rgba = g_malloc (n_pixels * 4);
    for (guint32 i = 0; i < n_pixels; i++) {
        guint32 px = argb[i];
        rgba[i*4 + 0] = (px >> 16) & 0xff;  /* R */
        rgba[i*4 + 1] = (px >>  8) & 0xff;  /* G */
        rgba[i*4 + 2] =  px        & 0xff;  /* B */
        rgba[i*4 + 3] = (px >> 24) & 0xff;  /* A */
    }
    g_free (argb);

    GBytes     *bytes = g_bytes_new_take (rgba, n_pixels * 4);
    GdkTexture *tex   = gdk_memory_texture_new (
        (int) width, (int) height,
        GDK_MEMORY_R8G8B8A8,
        bytes,
        width * 4);
    g_bytes_unref (bytes);
    return tex;
}

/* ── libavcodec helpers ─────────────────────────────────────────────────── */

/* Map deprecated yuvj* formats to their non-deprecated equivalents.
 * Returns the corrected format and sets *out_range to the color range. */
static enum AVPixelFormat
normalize_pix_fmt (enum AVPixelFormat fmt, int *out_range)
{
    *out_range = AVCOL_RANGE_MPEG;   /* default: TV/limited range */
    switch (fmt) {
    case AV_PIX_FMT_YUVJ420P: *out_range = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: *out_range = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: *out_range = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: *out_range = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P: *out_range = AVCOL_RANGE_JPEG; return AV_PIX_FMT_YUV411P;
    default: return fmt;
    }
}

/* libswscale's SIMD writers can store past the end of the destination
 * buffer. Measured with valgrind memcheck on ti_spork.jpg (600x600 YUVJ420P):
 * 8 byte stores landing 0 and 16 bytes past a 1,440,000 byte block. That
 * corrupted the next heap chunk and the process died on a later malloc/free,
 * only when the heap happened to be laid out badly (hence "needs a prior
 * load"). ASan cannot see it: the stores come from uninstrumented libswscale.
 * The pad covers the widest vector store (64 bytes, AVX-512). */
#define DV_SWS_DST_PAD 64

/* The one place a decoded AVFrame becomes an RGBA texture, at its own size
 * or scaled to dst_w x dst_h. Every sws_scale() destination comes from here. */
static GdkTexture *
frame_to_texture (AVFrame *frame, int dst_w, int dst_h, int sws_flags)
{
    int                src_range = AVCOL_RANGE_MPEG;
    enum AVPixelFormat src_fmt   = normalize_pix_fmt (
        (enum AVPixelFormat) frame->format, &src_range);

    /* Also honour frame->color_range if set */
    if (frame->color_range == AVCOL_RANGE_JPEG)
        src_range = AVCOL_RANGE_JPEG;

    dv_debug_log ("frame_to_texture: sws_getContext %dx%d -> %dx%d, starting",
                  frame->width, frame->height, dst_w, dst_h);
    struct SwsContext *sws = sws_getContext (
        frame->width, frame->height, src_fmt,
        dst_w, dst_h, AV_PIX_FMT_RGBA,
        sws_flags, NULL, NULL, NULL);
    dv_debug_log ("frame_to_texture: sws_getContext returned, sws=%s", sws ? "ok" : "NULL");
    if (!sws) return NULL;

    /* Set color range to suppress deprecated format warnings */
    sws_setColorspaceDetails (sws,
        sws_getCoefficients (SWS_CS_DEFAULT), src_range,
        sws_getCoefficients (SWS_CS_DEFAULT), AVCOL_RANGE_JPEG,
        0, 1 << 16, 1 << 16);

    gsize   size      = (gsize) dst_w * (gsize) dst_h * 4;
    guint8 *rgba      = g_malloc (size + DV_SWS_DST_PAD);
    guint8 *dst[4]    = { rgba, NULL, NULL, NULL };
    int     dst_ls[4] = { dst_w * 4, 0, 0, 0 };

    dv_debug_log ("frame_to_texture: sws_scale, starting");
    sws_scale (sws,
               (const uint8_t * const *) frame->data, frame->linesize,
               0, frame->height, dst, dst_ls);
    dv_debug_log ("frame_to_texture: sws_scale returned");
    sws_freeContext (sws);

    GBytes     *bytes = g_bytes_new_take (rgba, size);
    GdkTexture *tex   = gdk_memory_texture_new (dst_w, dst_h,
                            GDK_MEMORY_R8G8B8A8, bytes, (gsize) dst_w * 4);
    g_bytes_unref (bytes);
    return tex;
}

static GdkTexture *
avframe_to_texture (AVFrame *frame)
{
    dv_debug_log ("avframe_to_texture: %dx%d fmt=%d",
                  frame->width, frame->height, (int) frame->format);
    if (frame->width <= 0 || frame->height <= 0) return NULL;
    return frame_to_texture (frame, frame->width, frame->height, SWS_BILINEAR);
}

/* Animation memory cap. A GIF or APNG is held in memory frame by frame; this stops a
 * huge one from stalling the viewer. The animation is cut short, never refused. */
#ifndef DV_ANIM_MAX_BYTES
#define DV_ANIM_MAX_BYTES ((gsize) 512 * 1024 * 1024)
#endif

/* A frame delay as browsers read it: 10 ms or less (and unknown) means 100 ms. */
static guint
normalise_delay_ms (gint64 ms)
{
    return ms > 10 ? (guint) MIN (ms, 60000) : 100;
}

/* Formats whose frames are an animation to play. */
static gboolean
format_is_animated_image (const AVFormatContext *f)
{
    static const char *const NAMES[] = { "gif", "apng", "webp_pipe", NULL };
    for (int i = 0; NAMES[i]; i++)
        if (strcmp (f->iformat->name, NAMES[i]) == 0)
            return TRUE;
    return FALSE;
}

/* Formats that hold a still picture (image2, png_pipe, jpeg_pipe, ...). */
static gboolean
format_is_still_image (const AVFormatContext *f)
{
    return strcmp (f->iformat->name, "image2") == 0 ||
           g_str_has_suffix (f->iformat->name, "_pipe");
}

/* A video container (mkv, mp4, avi, ...) is shown as one representative frame, not played:
 * decoding all of it would stall the viewer and fill memory. Cover art is not video. */
static gboolean
stream_is_video (const AVFormatContext *f, const AVStream *vs)
{
    return !(vs->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
           !format_is_animated_image (f) && !format_is_still_image (f);
}

/* Position the video: about 10% in, as ffmpegthumbnailer does (to_start FALSE), or back at
 * the beginning (to_start TRUE), the fallback when the first seek found nothing to decode.
 * Returns TRUE if a seek was made. */
static gboolean
seek_video (AVFormatContext *f, AVCodecContext *c, gboolean to_start)
{
    int64_t start = f->start_time != AV_NOPTS_VALUE ? f->start_time : 0;
    int rc;
    if (to_start)
        rc = av_seek_frame (f, -1, 0, AVSEEK_FLAG_BYTE) >= 0 ? 0 :
             av_seek_frame (f, -1, start, AVSEEK_FLAG_BACKWARD);
    else {
        if (f->duration <= 0 || f->duration == AV_NOPTS_VALUE) return FALSE;
        rc = av_seek_frame (f, -1, start + f->duration / 10, AVSEEK_FLAG_BACKWARD);
    }
    dv_debug_log ("load: seek (%s) returned %d", to_start ? "to start" : "10%", rc);
    if (rc < 0) return FALSE;
    avcodec_flush_buffers (c);
    return TRUE;
}

static GdkTexture *
decode_pass_av (GCancellable   *cancel,
                AVFormatContext *fmt_ctx,
                int              video_stream,
                int              target_w,
                int              native_w,
                int              native_h)
{
    dv_debug_log ("decode_pass_av: target_w=%d native=%dx%d, starting", target_w, native_w, native_h);
    const AVCodec *codec = avcodec_find_decoder (
        fmt_ctx->streams[video_stream]->codecpar->codec_id);
    if (!codec) { dv_debug_log ("decode_pass_av: no decoder found"); return NULL; }

    AVCodecContext *ctx = avcodec_alloc_context3 (codec);
    if (!ctx) { dv_debug_log ("decode_pass_av: avcodec_alloc_context3 failed"); return NULL; }

    avcodec_parameters_to_context (ctx, fmt_ctx->streams[video_stream]->codecpar);
    ctx->thread_count = g_get_num_processors ();
    ctx->thread_type  = FF_THREAD_SLICE;
    dv_debug_log ("decode_pass_av: avcodec_open2, starting");
    if (avcodec_open2 (ctx, codec, NULL) < 0) {
        dv_debug_log ("decode_pass_av: avcodec_open2 failed");
        avcodec_free_context (&ctx);
        return NULL;
    }
    dv_debug_log ("decode_pass_av: avcodec_open2 returned ok");

    av_seek_frame (fmt_ctx, video_stream, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers (ctx);

    AVPacket   *pkt   = av_packet_alloc ();
    AVFrame    *frame = av_frame_alloc ();
    GdkTexture *tex   = NULL;

    while (!g_cancellable_is_cancelled (cancel) && !tex) {
        int ret = av_read_frame (fmt_ctx, pkt);
        if (ret < 0) break;
        if (pkt->stream_index != video_stream) { av_packet_unref (pkt); continue; }
        ret = avcodec_send_packet (ctx, pkt);
        av_packet_unref (pkt);
        if (ret < 0) break;

        ret = avcodec_receive_frame (ctx, frame);
        if (ret == AVERROR (EAGAIN)) continue;
        if (ret < 0) break;

        int scale_h = (target_w > 0 && target_w < native_w)
            ? (int)((gint64) native_h * target_w / native_w)
            : native_h;
        int scale_w = (target_w > 0 && target_w < native_w) ? target_w : native_w;

        /* Use fast bilinear when downscaling significantly -- much faster
         * for large images being thumbnailed */
        int sws_flags = (scale_w > 0 && frame->width / scale_w >= 4)
            ? SWS_FAST_BILINEAR : SWS_BILINEAR;
        tex = frame_to_texture (frame, scale_w, scale_h, sws_flags);
        av_frame_unref (frame);
    }

    av_frame_free (&frame);
    av_packet_free (&pkt);
    avcodec_free_context (&ctx);
    return tex;
}

/* ── Load thread ─────────────────────────────────────────────────────────── */

/* A load thread may change engine state only while its load is still the
 * current one, and the check is made under the same lock dv_engine_load takes
 * to reset that state. Every write a load thread makes to the engine goes
 * through one of these three. */
static gboolean
engine_set_native_size (DvEngine *eng, gint gen, int w, int h)
{
    g_mutex_lock (&eng->lock);
    gboolean current = !LOAD_IS_STALE (eng, gen);
    if (current) {
        eng->native_w = w;
        eng->native_h = h;
    }
    g_mutex_unlock (&eng->lock);
    return current;
}

static void
engine_mark_hdr (DvEngine *eng, gint gen)
{
    g_mutex_lock (&eng->lock);
    if (!LOAD_IS_STALE (eng, gen))
        eng->is_hdr = TRUE;
    g_mutex_unlock (&eng->lock);
}

static gboolean
engine_publish_frames (DvEngine *eng, gint gen, GPtrArray *frames, GArray *delays, gboolean is_anim)
{
    g_mutex_lock (&eng->lock);
    gboolean current = !LOAD_IS_STALE (eng, gen);
    if (current) {
        g_atomic_int_set (&eng->is_animated, is_anim);
        if (is_anim) {
            g_ptr_array_set_size (eng->frames, 0);
            g_array_set_size (eng->frame_delays, 0);
            for (guint i = 0; i < frames->len; i++) {
                g_ptr_array_add (eng->frames, g_object_ref (g_ptr_array_index (frames, i)));
                g_array_append_val (eng->frame_delays, g_array_index (delays, guint, i));
            }
        }
    }
    g_mutex_unlock (&eng->lock);
    return current;
}

static void
load_run (LoadArgs *args)
{
    DvEngine *eng  = args->eng;
    char     *path = args->path;

    /* Set once by any of the success-delivery sites below. If every decode
     * stage fails (rsvg / libavcodec / ffmpeg CLI / gdk-pixbuf) and this is
     * still FALSE when we reach fail_args, the widget is told explicitly
     * rather than being left showing "Loading..." forever -- that overlay
     * only ever clears today when a real result arrives. */
    gboolean any_success = FALSE;

    dv_debug_log ("load_thread_func: starting for %s (gen=%d)", path, args->gen);

    AVFormatContext *fmt_ctx = NULL;

    /* An archive or document holds other files. libav would read a stored zip that contains an
     * mpeg as that mpeg, so say plainly that this is not a picture. */
    if (dv_file_is_archive (path)) {
        dv_debug_log ("load: %s is an archive or document, not decoding it", path);
        goto fail_args;
    }

    /* ── Animated WebP: libav cannot decode it, libwebp can ────────────── */
    if (dv_webp_is_animated (path)) {
        GPtrArray *frames = g_ptr_array_new_with_free_func (g_object_unref);
        GArray    *delays = g_array_new (FALSE, FALSE, sizeof (guint));
        if (dv_webp_load_animation (path, args->cancel, DV_ANIM_MAX_BYTES, frames, delays) &&
            !g_cancellable_is_cancelled (args->cancel)) {
            for (guint i = 0; i < delays->len; i++)
                g_array_index (delays, guint, i) = normalise_delay_ms (g_array_index (delays, guint, i));
            GdkTexture *first = g_ptr_array_index (frames, 0);
            int w = gdk_texture_get_width (first), h = gdk_texture_get_height (first);
            gboolean is_anim = frames->len > 1;
            if (engine_set_native_size (eng, args->gen, w, h) &&
                engine_publish_frames (eng, args->gen, frames, delays, is_anim)) {
                post_result (eng, args->cb, args->user_data, args->gen,
                             g_object_ref (first), w, w, h, TRUE, FALSE);
                any_success = TRUE;
            }
            g_ptr_array_free (frames, TRUE);
            g_array_free (delays, TRUE);
            goto fail_args;   /* cleanup args -- not an error */
        }
        g_ptr_array_free (frames, TRUE);
        g_array_free (delays, TRUE);
        /* libwebp refused it: fall through to the other decoders */
    }

    /* ── Try libavcodec, fall back to ffmpeg CLI on any failure ───────────
     * Suppress av_log during probe so errors from oversized images don't
     * clutter the console. If libavcodec can't handle the file (e.g. a
     * PNG whose stride overflows int32), we retry via ffmpeg CLI pipe
     * which has no such limitation. */
    int av_log_level_saved = av_log_get_level ();
    av_log_set_level (AV_LOG_QUIET);

    dv_debug_log ("load: avformat_open_input, starting");
    int open_ok = (avformat_open_input (&fmt_ctx, path, NULL, NULL) == 0);
    dv_debug_log ("load: avformat_open_input returned open_ok=%d", open_ok);
    int info_ok = FALSE;
    if (open_ok) {
        dv_debug_log ("load: avformat_find_stream_info, starting");
        info_ok = (avformat_find_stream_info (fmt_ctx, NULL) == 0);
        dv_debug_log ("load: avformat_find_stream_info returned info_ok=%d", info_ok);
    }

    av_log_set_level (av_log_level_saved);   /* restore */

    if (!info_ok) {
        dv_debug_log ("load: libavcodec probe failed, falling back to ffmpeg pipe");
        /* libavcodec failed -- try ffmpeg CLI fallback */
        if (fmt_ctx) { avformat_close_input (&fmt_ctx); fmt_ctx = NULL; }

        /* Get native dims from file header; fall back to display cap if unknown */
        int fw = 0, fh = 0;
        read_image_dims_fast (path, &fw, &fh);
        if (fw <= 0 || fh <= 0) { fw = DV_MAX_DISPLAY_PX; fh = DV_MAX_DISPLAY_PX; }

        double scale = (fw > fh)
            ? (double) DV_MAX_DISPLAY_PX / fw
            : (double) DV_MAX_DISPLAY_PX / fh;
        if (scale > 1.0) scale = 1.0;
        int out_w = (int)(fw * scale);
        int out_h = (int)(fh * scale);

        engine_set_native_size (eng, args->gen, out_w, out_h);

        if (LOAD_IS_STALE (eng, args->gen)) goto fail_args;

        GdkTexture *tex = decode_via_ffmpeg_pipe (path, out_w, out_h);
        if (tex) {
            post_result (eng, args->cb, args->user_data, args->gen,
                         tex, out_w, out_w, out_h,
                         TRUE, FALSE);
            any_success = TRUE;
        }
        goto fail_args;   /* cleanup args -- not an error path */
    }

    {
        int video_stream = av_find_best_stream (fmt_ctx, AVMEDIA_TYPE_VIDEO,
                                                -1, -1, NULL, 0);
        if (video_stream < 0) {
            /* Audio with no picture: show the theme's audio icon, if the main thread gave one. */
            gboolean is_audio = av_find_best_stream (fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0;
            avformat_close_input (&fmt_ctx);
            if (!is_audio) goto fail_args;

            g_mutex_lock (&eng->lock);
            GdkTexture *icon = eng->audio_icon_tex ? g_object_ref (eng->audio_icon_tex) : NULL;
            g_mutex_unlock (&eng->lock);
            if (!icon) goto fail_args;

            int iw = gdk_texture_get_width (icon), ih = gdk_texture_get_height (icon);
            if (engine_set_native_size (eng, args->gen, iw, ih))
                post_result (eng, args->cb, args->user_data, args->gen, icon, iw, iw, ih, TRUE, FALSE);
            else
                g_object_unref (icon);
            any_success = TRUE;
            goto fail_args;   /* cleanup args -- not an error */
        }

        AVStream *vs      = fmt_ctx->streams[video_stream];
        int       native_w = vs->codecpar->width;
        int       native_h = vs->codecpar->height;

        if (native_w <= 0 || native_h <= 0) {
            /* avformat_find_stream_info found the stream but couldn't decode
             * a frame to populate dimensions -- image likely too large for
             * libavcodec. Try read_image_dims_fast + GdkPixbuf fallback. */
            avformat_close_input (&fmt_ctx);

            int fw = 0, fh = 0;
            read_image_dims_fast (path, &fw, &fh);
            if (fw <= 0 || fh <= 0) goto fail_args;

            double scale = (fw > fh)
                ? (double) DV_MAX_DISPLAY_PX / fw
                : (double) DV_MAX_DISPLAY_PX / fh;
            if (scale > 1.0) scale = 1.0;
            int out_w = (int)(fw * scale);
            int out_h = (int)(fh * scale);

            engine_set_native_size (eng, args->gen, out_w, out_h);

            if (LOAD_IS_STALE (eng, args->gen)) goto fail_args;

            GdkTexture *tex = decode_via_ffmpeg_pipe (path, out_w, out_h);
            if (tex) {
                post_result (eng, args->cb, args->user_data, args->gen,
                             tex, out_w, out_w, out_h,
                             TRUE, FALSE);
                any_success = TRUE;
            }
            goto fail_args;
        }

        /* Store true source dimensions -- needed for export quality */
        int src_w = native_w, src_h = native_h;
        (void) src_w; (void) src_h;

        /* Cap display resolution -- prevents stall/OOM on huge images.
         * Exports bypass this and use the original file directly. */
        if (native_w > DV_MAX_DISPLAY_PX || native_h > DV_MAX_DISPLAY_PX) {
            double scale = (double) DV_MAX_DISPLAY_PX /
                           (native_w > native_h ? native_w : native_h);
            native_w = (int)(native_w * scale);
            native_h = (int)(native_h * scale);
        }

        /* If source exceeds libavcodec decode limit, use ffmpeg CLI fallback.
         * This handles mega-PNGs that libavcodec refuses due to stride overflow. */
        int av_max = av_max_decode_dim ();
        if (vs->codecpar->width > av_max || vs->codecpar->height > av_max) {
            avformat_close_input (&fmt_ctx);

            engine_set_native_size (eng, args->gen, native_w, native_h);

            if (LOAD_IS_STALE (eng, args->gen)) goto fail_args;

            GdkTexture *tex = decode_via_ffmpeg_pipe (path, native_w, native_h);
            if (tex) {
                post_result (eng, args->cb, args->user_data, args->gen,
                             tex, native_w, native_w, native_h,
                             TRUE, FALSE);
                any_success = TRUE;
            }
            goto fail_args;   /* cleanup args -- not an error */
        }

        /* Abort if a newer load has been requested */
        if (LOAD_IS_STALE (eng, args->gen)) {
            avformat_close_input (&fmt_ctx);
            goto fail_args;
        }

        engine_set_native_size (eng, args->gen, native_w, native_h);

        /* Open codec */
        dv_debug_log ("load: opening main codec for stream %d, starting", video_stream);
        const AVCodec *codec = avcodec_find_decoder (vs->codecpar->codec_id);
        AVCodecContext *codec_ctx = NULL;
        if (codec) {
            codec_ctx = avcodec_alloc_context3 (codec);
            avcodec_parameters_to_context (codec_ctx, vs->codecpar);
            /* Use all available CPU cores -- PNG decoder supports slice threading */
            codec_ctx->thread_count = g_get_num_processors ();
            codec_ctx->thread_type  = FF_THREAD_SLICE;
            if (avcodec_open2 (codec_ctx, codec, NULL) < 0) {
                avcodec_free_context (&codec_ctx);
                codec_ctx = NULL;
            }
        }
        dv_debug_log ("load: opening main codec returned codec_ctx=%s",
                      codec_ctx ? "ok" : "NULL");
        if (!codec_ctx) {
            avformat_close_input (&fmt_ctx);
            goto fail_args;
        }

        gboolean is_video     = stream_is_video (fmt_ctx, vs);
        gboolean single_frame = is_video || (vs->disposition & AV_DISPOSITION_ATTACHED_PIC);
        gsize    anim_bytes   = 0;
        gboolean seeked       = is_video && seek_video (fmt_ctx, codec_ctx, FALSE);

        dv_debug_log ("load: entering frame decode loop");
        GPtrArray *all_frames = g_ptr_array_new_with_free_func (g_object_unref);
        GArray    *all_delays = g_array_new (FALSE, FALSE, sizeof (guint));
        guint      pkt_ms     = 100;
        AVPacket  *pkt        = av_packet_alloc ();
        AVFrame   *frame      = av_frame_alloc ();

decode_frames:
        while (!g_cancellable_is_cancelled (args->cancel)) {
            int ret = av_read_frame (fmt_ctx, pkt);
            if (ret < 0) { dv_debug_log ("load: av_read_frame ret=%d, end of stream", ret); break; }

            if (pkt->stream_index != video_stream) {
                av_packet_unref (pkt); continue;
            }

            pkt_ms = normalise_delay_ms (av_rescale_q (pkt->duration, vs->time_base, (AVRational) { 1, 1000 }));
            if (avcodec_send_packet (codec_ctx, pkt) < 0) {
                dv_debug_log ("load: avcodec_send_packet failed, stopping decode");
                av_packet_unref (pkt); break;
            }
            av_packet_unref (pkt);

            while (!g_cancellable_is_cancelled (args->cancel)) {
                ret = avcodec_receive_frame (codec_ctx, frame);
                if (ret == AVERROR (EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    dv_debug_log ("load: avcodec_receive_frame ret=%d, aborting decode", ret);
                    goto done_reading;
                }

                /* HDR detection: PQ (SMPTE2084) or HLG (ARIB_STD_B67) */
                if (frame->color_trc == AVCOL_TRC_SMPTE2084 ||
                    frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
                    engine_mark_hdr (eng, args->gen);
                GdkTexture *tex = avframe_to_texture (frame);
                av_frame_unref (frame);
                if (!tex) { dv_debug_log ("load: avframe_to_texture returned NULL, dropping frame"); continue; }

                dv_debug_log ("load: frame %u decoded ok", all_frames->len);
                anim_bytes += (gsize) gdk_texture_get_width (tex) * gdk_texture_get_height (tex) * 4;
                g_ptr_array_add (all_frames, tex);
                g_array_append_val (all_delays, pkt_ms);

                if (all_frames->len == 1 &&
                    !g_cancellable_is_cancelled (args->cancel)) {
                    post_result (eng, args->cb, args->user_data, args->gen,
                                 g_object_ref (tex), native_w, native_w, native_h,
                                 FALSE, FALSE);
                    any_success = TRUE;
                }

                if (single_frame) goto done_reading;
                if (anim_bytes > DV_ANIM_MAX_BYTES) {
                    dv_debug_log ("load: animation memory cap reached at %u frames, stopping", all_frames->len);
                    goto done_reading;
                }
            }
        }

done_reading:
        dv_debug_log ("load: frame decode loop ended, %u frame(s) decoded, cancelled=%d",
                      all_frames->len, g_cancellable_is_cancelled (args->cancel));
        if (seeked && all_frames->len == 0 && !g_cancellable_is_cancelled (args->cancel)) {
            seeked = FALSE;                         /* seek found nothing: decode from the start */
            if (seek_video (fmt_ctx, codec_ctx, TRUE)) goto decode_frames;
        }
        av_frame_free (&frame);
        av_packet_free (&pkt);
        avcodec_free_context (&codec_ctx);

        if (g_cancellable_is_cancelled (args->cancel) || all_frames->len == 0) {
            g_ptr_array_free (all_frames, TRUE);
            g_array_free (all_delays, TRUE);
            avformat_close_input (&fmt_ctx);
            goto fail_args;
        }

        gboolean is_anim = (all_frames->len > 1);
        if (!engine_publish_frames (eng, args->gen, all_frames, all_delays, is_anim)) {
            g_ptr_array_free (all_frames, TRUE);
            g_array_free (all_delays, TRUE);
            avformat_close_input (&fmt_ctx);
            goto fail_args;   /* superseded while decoding: change nothing */
        }

        if (is_anim) {
            post_result (eng, args->cb, args->user_data, args->gen,
                         g_object_ref (g_ptr_array_index (all_frames, 0)), native_w, native_w, native_h,
                         TRUE, FALSE);
            any_success = TRUE;

        } else {
            /* Progressive ladder passes using a second format context. Not for a video frame or
             * cover art: that is one small picture inside a possibly huge file, and the ladder's
             * seek and re-read would scan the whole file for nothing. */
            AVFormatContext *ladder_ctx = NULL;
            if (!single_frame &&
                avformat_open_input (&ladder_ctx, path, NULL, NULL) >= 0 &&
                avformat_find_stream_info (ladder_ctx, NULL) >= 0) {

                for (int i = 0; DV_LADDER[i] != 0 &&
                                !g_cancellable_is_cancelled (args->cancel); i++) {
                    int tw = DV_LADDER[i];
                    if (tw >= native_w) break;

                    GdkTexture *ltex = decode_pass_av (args->cancel, ladder_ctx,
                                                       video_stream, tw,
                                                       native_w, native_h);
                    if (!ltex) continue;
                    if (g_cancellable_is_cancelled (args->cancel)) {
                        g_object_unref (ltex); break;
                    }
                    post_result (eng, args->cb, args->user_data, args->gen,
                                 ltex, tw, native_w, native_h,
                                 FALSE, FALSE);
                    any_success = TRUE;
                }
                avformat_close_input (&ladder_ctx);
            }

            post_result (eng, args->cb, args->user_data, args->gen,
                         g_object_ref (g_ptr_array_index (all_frames, 0)), native_w, native_w, native_h,
                         TRUE, FALSE);
            any_success = TRUE;
        }

        g_ptr_array_free (all_frames, TRUE);
        g_array_free (all_delays, TRUE);
        avformat_close_input (&fmt_ctx);
    }

    return;

fail_args:
    if (!any_success && !LOAD_IS_STALE (eng, args->gen)) {
        dv_debug_log ("load: every decode stage failed for %s, telling the widget", path);
        post_result (eng, args->cb, args->user_data, args->gen,
                     NULL, 0, 0, 0,
                     TRUE, TRUE);
    }
    return;
}

static gpointer
load_thread_func (gpointer data)
{
    LoadArgs *args = data;
    DvEngine *eng  = args->eng;

    load_run (args);

    g_object_unref (args->cancel);
    g_free (args->path);
    g_free (args);

    /* Last use of eng by this thread: tell dv_engine_free it may proceed. */
    g_mutex_lock (&eng->lock);
    eng->active_loads--;
    g_cond_broadcast (&eng->loads_done);
    g_mutex_unlock (&eng->lock);
    return NULL;
}

/* ── Main-thread delivery ───────────────────────────────────────────────── */

static gboolean
deliver_idle (gpointer data)
{
    PassResult *r = data;

    g_mutex_lock (&r->eng->lock);
    g_hash_table_remove (r->eng->pending, r);
    g_mutex_unlock (&r->eng->lock);

    /* Discard stale results from superseded load operations */
    gboolean stale = r->eng &&
        (g_atomic_int_get (&r->eng->load_gen) != r->gen);

    if (!stale && r->cb) {
        /* Real content arrived -- clear placeholder flag and cancel any
         * pending delayed-placeholder timer so it can never fire after
         * the fact and flash "Loading..." over an already-loaded image. */
        if (r->eng && r->is_final) {
            r->eng->is_placeholder = FALSE;
            r->eng->is_failed      = r->failed;
            if (r->eng->placeholder_timer) {
                g_source_remove (r->eng->placeholder_timer);
                r->eng->placeholder_timer = 0;
            }
        }
        /* A failure always shows the placeholder graphic, whatever the
         * splash settings were: with the splash off or delayed nothing has
         * put it on screen yet. */
        if (r->eng && r->is_final && r->failed && !r->texture &&
            r->eng->placeholder_tex) {
            r->texture  = g_object_ref (r->eng->placeholder_tex);
            r->native_w = gdk_texture_get_width  (r->texture);
            r->native_h = gdk_texture_get_height (r->texture);
        }
        if (r->is_final && dv_engine_is_animated (r->eng))
            dv_engine_start_anim (r->eng);
        r->cb (r->texture, r->pass_width, r->native_w, r->native_h,
               r->is_final, r->user_data);
    }

    if (r->texture)
        g_object_unref (r->texture);
    g_free (r);
    return G_SOURCE_REMOVE;
}

/* ── Animation ──────────────────────────────────────────────────────────── */

/* How long frame idx stays up. Caller holds the lock. */
static guint
frame_delay_locked (DvEngine *eng, guint idx)
{
    return idx < eng->frame_delays->len ? g_array_index (eng->frame_delays, guint, idx) : 100;
}

/* One timeout per frame, because frames have their own delays. */
static gboolean
anim_tick (gpointer data)
{
    DvEngine *eng = data;
    g_mutex_lock (&eng->lock);
    guint n = eng->frames->len;
    if (n > 0)
        eng->frame_idx = (eng->frame_idx + 1) % n;
    guint ms = frame_delay_locked (eng, eng->frame_idx);
    g_mutex_unlock (&eng->lock);
    eng->anim_timer = g_timeout_add (ms, anim_tick, eng);
    return G_SOURCE_REMOVE;
}

/* Called from main thread via idle after all animation frames loaded */
void
dv_engine_start_anim (DvEngine *eng)
{
    if (eng->anim_timer) return;
    g_mutex_lock (&eng->lock);
    guint ms = frame_delay_locked (eng, eng->frame_idx);
    g_mutex_unlock (&eng->lock);
    eng->anim_timer = g_timeout_add (ms, anim_tick, eng);
}

guint
dv_engine_frame_delay_ms (DvEngine *eng, guint idx)
{
    g_mutex_lock (&eng->lock);
    guint ms = idx < eng->frame_delays->len ? g_array_index (eng->frame_delays, guint, idx) : 0;
    g_mutex_unlock (&eng->lock);
    return ms;
}

/* ── Public accessors ───────────────────────────────────────────────────── */

gboolean
dv_engine_is_animated (DvEngine *eng)
{
    return g_atomic_int_get (&eng->is_animated);
}

GdkTexture *
dv_engine_get_frame (DvEngine *eng)
{
    GdkTexture *tex = NULL;
    g_mutex_lock (&eng->lock);
    if (eng->frames->len > 0)
        tex = g_ptr_array_index (eng->frames, eng->frame_idx);
    g_mutex_unlock (&eng->lock);
    return tex;
}

gboolean
dv_engine_is_svg (DvEngine *eng)
{
    return eng->is_svg;
}

gboolean
dv_engine_is_hdr (DvEngine *eng)
{
    g_mutex_lock (&eng->lock);
    gboolean hdr = eng->is_hdr;
    g_mutex_unlock (&eng->lock);
    return hdr;
}

gboolean
dv_engine_is_placeholder (DvEngine *eng)
{
    return eng->is_placeholder;
}

gboolean
dv_engine_is_failed (DvEngine *eng)
{
    return eng->is_failed;
}

void
dv_engine_init_placeholder (DvEngine *eng)
{
    if (eng->placeholder_tex) return;   /* already initialised */

    RsvgHandle *svg = rsvg_handle_new_from_data (
        (const guint8 *) DV_PLACEHOLDER_SVG,
        sizeof (DV_PLACEHOLDER_SVG) - 1, NULL);
    if (!svg) return;

    RsvgDimensionData dim;
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    rsvg_handle_get_dimensions (svg, &dim);
G_GNUC_END_IGNORE_DEPRECATIONS
    int pw = dim.width  > 0 ? dim.width  : 100;
    int ph_h = dim.height > 0 ? dim.height : 100;

    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, pw, ph_h);
    cairo_t *cr = cairo_create (surf);
    RsvgRectangle vp = { 0, 0, (double)pw, (double)ph_h };
    rsvg_handle_render_document (svg, cr, &vp, NULL);
    cairo_destroy (cr);
    g_object_unref (svg);

    eng->placeholder_tex = dv_texture_from_surface (surf);
    cairo_surface_destroy (surf);
}

void
dv_engine_set_audio_icon (DvEngine *eng, GdkTexture *icon)
{
    GdkTexture *ref = icon ? g_object_ref (icon) : NULL;
    g_mutex_lock (&eng->lock);
    g_clear_object (&eng->audio_icon_tex);
    eng->audio_icon_tex = ref;
    g_mutex_unlock (&eng->lock);
}

GdkTexture *
dv_engine_get_svg (DvEngine *eng, int render_w, int render_h)
{
    if (!eng->svg_handle) return NULL;
    /* Clamp to reasonable max to avoid huge allocations */
    if (render_w > 8192) render_w = 8192;
    if (render_h > 8192) render_h = 8192;
    if (render_w < 1) render_w = 1;
    if (render_h < 1) render_h = 1;

    if (eng->svg_texture &&
        eng->svg_last_w == render_w && eng->svg_last_h == render_h)
        return eng->svg_texture;

    cairo_surface_t *surf = cairo_image_surface_create (
        CAIRO_FORMAT_ARGB32, render_w, render_h);
    cairo_t *cr = cairo_create (surf);

    RsvgRectangle vp = { .x = 0, .y = 0,
                         .width = render_w, .height = render_h };
    rsvg_handle_render_document (eng->svg_handle, cr, &vp, NULL);
    cairo_destroy (cr);

    /* Hand the pixels straight to GDK. No temp file, so nothing to race on.
     * cairo ARGB32 is premultiplied and native-endian, which is exactly
     * GDK_MEMORY_DEFAULT. */
    cairo_surface_flush (surf);
    gsize   stride = (gsize) cairo_image_surface_get_stride (surf);
    GBytes *px     = g_bytes_new (cairo_image_surface_get_data (surf),
                                  stride * (gsize) render_h);
    GdkTexture *tex = gdk_memory_texture_new (render_w, render_h,
                                              GDK_MEMORY_DEFAULT, px, stride);
    g_bytes_unref (px);
    cairo_surface_destroy (surf);

    g_clear_object (&eng->svg_texture);
    eng->svg_texture = tex;
    eng->svg_last_w  = render_w;
    eng->svg_last_h  = render_h;

    return eng->svg_texture;
}

GdkTexture *
dv_engine_get_current_texture (DvEngine *eng)
{
    g_mutex_lock (&eng->lock);
    GdkTexture *tex = NULL;
    if (eng->frames && eng->frames->len > 0)
        tex = g_ptr_array_index (eng->frames, eng->frame_idx);
    g_mutex_unlock (&eng->lock);
    return tex;
}

void
dv_engine_get_size (DvEngine *eng, int *w, int *h)
{
    g_mutex_lock (&eng->lock);
    *w = eng->native_w;
    *h = eng->native_h;
    g_mutex_unlock (&eng->lock);
}
