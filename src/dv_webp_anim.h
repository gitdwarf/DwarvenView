#pragma once
/*
 * dv_webp_anim -- animated WebP, which libav cannot decode, through libwebp's own animation decoder.
 *
 * The decoder composites each frame onto the canvas (blend and dispose handled), so every frame
 * handed back is a complete picture.
 */
#include <gio/gio.h>
#include <gdk/gdk.h>

/* TRUE if the file is a WebP whose header says it holds an animation. Reads 32 bytes. */
gboolean dv_webp_is_animated (const char *path);

/* Decodes every frame. frames receives GdkTexture* (owned by the array, so create it with
 * g_object_unref as the free func), delays receives the guint milliseconds each frame stays up.
 * Stops early, keeping what it has, once max_bytes of pixels are held or cancel fires.
 * Returns TRUE if at least one frame was decoded. */
gboolean dv_webp_load_animation (const char   *path,
                                 GCancellable *cancel,
                                 gsize         max_bytes,
                                 GPtrArray    *frames,
                                 GArray       *delays);
