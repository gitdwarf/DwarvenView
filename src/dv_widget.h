#pragma once

#include <gtk/gtk.h>
#include "dv_engine.h"
#include "dv_viewport.h"
#include "dv_export.h"
#include "dv_prefs.h"

G_BEGIN_DECLS

#define DV_TYPE_WIDGET (dv_widget_get_type ())
G_DECLARE_FINAL_TYPE (DvWidget, dv_widget, DV, WIDGET, GtkWidget)

/* Create a new DwarvenView widget */
GtkWidget  *dv_widget_new       (void);

/* Load an image -- begins progressive loading */
void        dv_widget_load          (DvWidget *self, const char *path);
void        dv_widget_load_keep_zoom (DvWidget *self, const char *path);

/* Get the current source path (may be NULL) */
const char *dv_widget_get_path  (DvWidget *self);

/* Export current view -- returns output path or NULL on failure.
 * Caller must g_free() the result. */
DvExportResult *dv_widget_export (DvWidget *self, const DvExportFormat *fmt);

/* Set pan preferences (inversion flags) */
void        dv_widget_set_pan_prefs (DvWidget *self, const DvPanPrefs *pan);

/* Set full user preferences */
void        dv_widget_set_prefs      (DvWidget *self, const DvPrefs *prefs);

/* Rotation and flip -- for toolbar button callbacks */
void        dv_widget_rotate_cw      (DvWidget *self);
void        dv_widget_rotate_ccw     (DvWidget *self);
void        dv_widget_flip_h         (DvWidget *self);
void        dv_widget_flip_v         (DvWidget *self);

/* Zoom on centre of widget */
void        dv_widget_zoom_centre    (DvWidget *self, double factor);
void        dv_widget_reset_zoom      (DvWidget *self);
void        dv_widget_zoom_actual     (DvWidget *self);  /* zoom to 1:1 pixel */
DvEngine   *dv_widget_get_engine      (DvWidget *self);
GActionGroup *dv_widget_get_action_group (DvWidget *self);
gboolean      dv_widget_has_transform     (DvWidget *self);
gboolean      dv_widget_is_actual_size    (DvWidget *self);  /* TRUE if view_mode == ACTUAL */
gboolean      dv_widget_has_zoom_history  (DvWidget *self);  /* TRUE if manually zoomed away from baseline */  /* TRUE if rotation or flip applied */
void        dv_widget_reset_transform (DvWidget *self);

/* Get status info for display */
void        dv_widget_get_status (DvWidget    *self,
                                  int         *out_native_w,
                                  int         *out_native_h,
                                  double      *out_zoom,
                                  gboolean    *out_is_hard,
                                  gboolean    *out_has_crop);

/* Signals emitted by DvWidget:
 *   "load-complete"  (gboolean success)
 *   "zoom-changed"   (double zoom)
 */

G_END_DECLS
