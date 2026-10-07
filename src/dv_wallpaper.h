#pragma once

/*
 * dv_wallpaper.h -- XDG-compliant wallpaper management
 *
 * Follows the XDG Base Directory Specification:
 *   User wallpapers:   $XDG_DATA_HOME/wallpapers/   (~/.local/share/wallpapers/)
 *   System wallpapers: /usr/share/wallpapers/
 *
 * The active wallpaper is indicated by a symlink:
 *   $XDG_DATA_HOME/wallpapers/.current -> <active wallpaper file>
 *
 * DwarvenDE (and any other XDG-compliant DE) watches this symlink
 * via inotify to detect wallpaper changes.
 *
 * Part of DwarvenSuite -- follows and extends XDG/freedesktop specs.
 */

#include <glib.h>

/*
 * Set the given image file as the desktop wallpaper.
 *
 * Copies the file into $XDG_DATA_HOME/wallpapers/ if it is not already there,
 * then updates the .current symlink to point to it.
 *
 * Returns TRUE on success, FALSE on failure.
 * On failure, error_out (if non-NULL) is set to a human-readable message
 * the caller must g_free().
 */
/*
 * Smart wallpaper set -- applies the decision tree:
 *   No viewport transform   -> copy source file as-is (any format)
 *   SVG + transform         -> export modified-viewBox SVG
 *   Raster + transform      -> HDR: JXL (fallback PNG), SDR: PNG
 *
 * Requires the current DvEngine and DvViewport to check transform state.
 */
#include "dv_engine.h"
#include "dv_viewport.h"

gboolean dv_wallpaper_set_smart (const char  *source_path,
                                  DvEngine    *engine,
                                  DvViewport  *viewport,
                                  char       **error_out);

/* Simple copy-as-is (no transform) */
gboolean dv_wallpaper_set (const char *image_path, char **error_out);

/*
 * Returns the path of the current wallpaper (the .current symlink target),
 * or NULL if none is set. Caller must g_free() the result.
 */
char    *dv_wallpaper_get_current (void);

/*
 * Returns the user wallpaper directory ($XDG_DATA_HOME/wallpapers/).
 * Creates it if it does not exist.
 * Caller must g_free() the result.
 */
char    *dv_wallpaper_dir (void);
