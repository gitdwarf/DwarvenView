#pragma once

#include <glib.h>
#include <gdk/gdk.h>

/* Resolution ladder for progressive loading (widths in pixels, 0-terminated) */
extern const int DV_LADDER[];

/* Callback fired on the main thread each time a new pass is ready */
typedef void (*DvEnginePassCb) (GdkTexture *texture,
                                int         pass_width,
                                int         native_w,
                                int         native_h,
                                gboolean    is_final,
                                gpointer    user_data);

/* Opaque engine handle */
typedef struct _DvEngine DvEngine;

DvEngine   *dv_engine_new          (void);
void        dv_engine_free         (DvEngine *eng);

/*
 * Begin loading path. Cancels any in-flight load immediately.
 * cb is called on the main thread for each progressive pass.
 * show_placeholder: FALSE suppresses the loading placeholder texture and
 * "Loading..." overlay entirely for this load.
 * placeholder_delay: seconds to wait, showing the previous image (or
 * blank if none) before the placeholder appears. 0.0 means immediate,
 * matching the pre-delay behavior. Ignored when show_placeholder is FALSE.
 */
void        dv_engine_load         (DvEngine      *eng,
                                    const char    *path,
                                    DvEnginePassCb cb,
                                    gpointer       user_data,
                                    gboolean       show_placeholder,
                                    double         placeholder_delay);

/* Cancel any in-flight load (non-blocking -- fires-and-forgets) */
void        dv_engine_cancel       (DvEngine *eng);

/* True if the current image is animated */
gboolean    dv_engine_is_animated  (DvEngine *eng);

/* Get current animation frame texture (may be NULL) */
GdkTexture *dv_engine_get_frame    (DvEngine *eng);

/* True if the current image is SVG */
gboolean    dv_engine_is_svg       (DvEngine *eng);
gboolean    dv_engine_is_hdr       (DvEngine *eng);
gboolean    dv_engine_is_placeholder  (DvEngine *eng);
gboolean    dv_engine_is_failed       (DvEngine *eng);

/* Milliseconds animation frame idx stays up, or 0 if there is no such frame. */
guint       dv_engine_frame_delay_ms  (DvEngine *eng, guint idx);

/* Call once from main thread after engine creation to pre-render the
 * placeholder texture. Must be called before dv_engine_load(). */
void        dv_engine_init_placeholder (DvEngine *eng);

/* The texture shown for an audio file with no cover art (the theme's audio icon). NULL clears it.
 * Main thread. Without one, such a file fails to load as before. */
void        dv_engine_set_audio_icon (DvEngine *eng, GdkTexture *icon);

/* Render SVG at given display size -- only re-renders if size changed */
GdkTexture *dv_engine_get_svg      (DvEngine *eng, int w, int h);

/* Native image dimensions after first pass completes */
void        dv_engine_get_size             (DvEngine *eng, int *w, int *h);
GdkTexture *dv_engine_get_current_texture  (DvEngine *eng);
