#include "dv_webp_anim.h"
#include "dv_debug.h"

#include <webp/demux.h>
#include <stdio.h>
#include <string.h>

gboolean
dv_webp_is_animated (const char *path)
{
    FILE *f = fopen (path, "rb");
    if (!f) return FALSE;
    guint8 h[32] = {0};
    size_t got = fread (h, 1, sizeof h, f);
    fclose (f);
    /* RIFF, size, WEBP, then a VP8X chunk whose flags byte has the animation bit (0x02). */
    return got >= 21 && memcmp (h, "RIFF", 4) == 0 && memcmp (h + 8, "WEBPVP8X", 8) == 0 &&
           (h[20] & 0x02);
}

gboolean
dv_webp_load_animation (const char *path, GCancellable *cancel, gsize max_bytes,
                        GPtrArray *frames, GArray *delays)
{
    GMappedFile *map = g_mapped_file_new (path, FALSE, NULL);
    if (!map) return FALSE;

    WebPData data = { (const uint8_t *) g_mapped_file_get_contents (map),
                      g_mapped_file_get_length (map) };
    WebPAnimDecoderOptions opts;
    WebPAnimDecoderOptionsInit (&opts);
    opts.color_mode  = MODE_rgbA;      /* premultiplied, which is what GDK wants */
    opts.use_threads = 1;

    dv_debug_log ("webp: WebPAnimDecoderNew %s", path);
    WebPAnimDecoder *dec = WebPAnimDecoderNew (&data, &opts);
    dv_debug_log ("webp: WebPAnimDecoderNew returned %s", dec ? "ok" : "NULL");
    if (!dec) { g_mapped_file_unref (map); return FALSE; }

    WebPAnimInfo info;
    gsize held = 0;
    int   prev_end = 0;
    if (WebPAnimDecoderGetInfo (dec, &info)) {
        gsize stride = (gsize) info.canvas_width * 4;
        while (!g_cancellable_is_cancelled (cancel) && WebPAnimDecoderHasMoreFrames (dec)) {
            uint8_t *rgba; int end_ms;
            if (!WebPAnimDecoderGetNext (dec, &rgba, &end_ms)) break;

            /* The decoder reuses its buffer, so the texture must own a copy. */
            GBytes *bytes = g_bytes_new (rgba, stride * info.canvas_height);
            GdkTexture *tex = gdk_memory_texture_new (info.canvas_width, info.canvas_height,
                                                      GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, bytes, stride);
            g_bytes_unref (bytes);

            guint ms = (guint) (end_ms - prev_end);
            prev_end = end_ms;
            g_ptr_array_add (frames, tex);
            g_array_append_val (delays, ms);
            held += stride * info.canvas_height;
            if (held > max_bytes) {
                dv_debug_log ("webp: memory cap reached at %u frames, stopping", frames->len);
                break;
            }
        }
    }
    WebPAnimDecoderDelete (dec);
    g_mapped_file_unref (map);
    dv_debug_log ("webp: %u frame(s) decoded", frames->len);
    return frames->len > 0;
}
