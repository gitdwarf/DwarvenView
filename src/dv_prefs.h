#pragma once

#include <glib.h>
#include "dv_viewport.h"   /* DvViewMode */

/* Pan inversion flags */
typedef struct {
    gboolean invert_arrow_lr;   /* Left/Right arrow keys */
    gboolean invert_arrow_ud;   /* Up/Down arrow keys */
    gboolean invert_wheel_ud;   /* Scroll wheel vertical */
    gboolean invert_wheel_lr;   /* Scroll wheel horizontal */
} DvPanPrefs;

/* Full preferences */
typedef struct {
    DvPanPrefs  pan;
    char       *fallback_save_dir;   /* User-specified fallback, may be NULL */
    char       *slideshow_interval;   /* e.g. "3", "0.5", "1/30" */
    gboolean    ctx_dynamic;           /* TRUE = hide unavailable right-click items */
    DvViewMode  default_view_mode;     /* view mode applied to every freshly loaded image */
    gboolean    show_loading_splash;   /* FALSE = never show the loading placeholder/overlay */
    double      loading_splash_delay;  /* seconds (0.0-1.0) to wait before showing it, when ON */
    gboolean    browse_media;          /* TRUE = Open and next/previous also take audio and video; FALSE = images only */
} DvPrefs;

/* Load prefs from ~/.config/dwarvenview/dwarvenview.conf.
 * Returns a newly allocated DvPrefs with defaults if file missing. */
DvPrefs    *dv_prefs_load  (void);

/* Save prefs to config file. */
void        dv_prefs_save  (const DvPrefs *prefs);

/* Free a DvPrefs. */
void        dv_prefs_free  (DvPrefs *prefs);

/*
 * Resolve the output path for a conversion.
 *
 * source_path  -- path of the source file being converted
 * ext          -- output extension (e.g. "png")
 * prefs        -- current preferences (for fallback_save_dir)
 *
 * Resolution order:
 *   1. Source dir (if writable, not system path, not $TMPDIR)
 *   2. prefs->fallback_save_dir (if set, writable, not system path)
 *   3. ~/Pictures (if exists and writable)
 *   4. $HOME
 *
 * Root user: same rules -- no writes outside /root subtree.
 *
 * Returns a newly allocated path string. Caller must g_free().
 * Returns NULL if no suitable path found (should not happen in practice).
 */
/* Last accessed directory -- cached in ~/.cache/dwarvenview/last_dir */
void        dv_prefs_save_last_dir  (const char *dir);
char       *dv_prefs_load_last_dir  (void);   /* caller g_free() */

char       *dv_prefs_resolve_output (const char    *source_path,
                                     const char    *ext,
                                     const DvPrefs *prefs);
