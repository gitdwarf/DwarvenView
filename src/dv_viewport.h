#pragma once

#include <glib.h>

/* Base view mode -- persists across manual zooms */
typedef enum {
    DV_VIEW_MODE_FIT    = 0,   /* fit to viewport (default) */
    DV_VIEW_MODE_ACTUAL = 1,   /* 1:1 pixel */
} DvViewMode;

/* One entry in the hard-zoom history stack */
typedef struct {
    double zoom;
    double pan_x;
    double pan_y;
    /* If this was a box-crop zoom, the crop region in image coordinates */
    gboolean has_crop;
    double crop_x, crop_y, crop_w, crop_h;
} DvZoomState;

/* The viewport manages all display geometry */
typedef struct {
    /* Soft zoom -- fit-to-window, display only, never used for export */
    gboolean soft_mode;
    double   soft_zoom;
    double   soft_pan_x, soft_pan_y;

    /* Hard zoom -- explicit user action, used for export */
    double   zoom;
    double   pan_x, pan_y;

    /* Box crop state (hard zoom variant) */
    gboolean has_crop;
    double   crop_x, crop_y, crop_w, crop_h;  /* in image coordinates */

    /* Rubber-band box being drawn (display coordinates) */
    gboolean box_drawing;
    double   box_x1, box_y1;   /* start point (display coords) */
    double   box_x2, box_y2;   /* current end point */

    /* Hard zoom history stack (GArray of DvZoomState) */
    GArray  *history;

    /* Image dimensions (needed for coordinate mapping) */
    int      image_w, image_h;
    /* Widget dimensions */
    int      widget_w, widget_h;

    /* View transform: rotation and flip (reset on image change) */
    int      rotation;   /* degrees CW: 0, 90, 180, 270 */
    gboolean flip_h;     /* horizontal mirror */
    gboolean flip_v;     /* vertical flip */

    /* User's chosen baseline -- persists across manual zooms */
    DvViewMode view_mode;
} DvViewport;


void        dv_viewport_init        (DvViewport *vp);
void        dv_viewport_free        (DvViewport *vp);

/* Call when image or widget size changes */
void        dv_viewport_set_image   (DvViewport *vp, int img_w, int img_h);
void        dv_viewport_set_widget  (DvViewport *vp, int w, int h);

/* Recompute soft zoom to fit image in widget */
void        dv_viewport_fit         (DvViewport *vp);

/* Hard zoom -- centred on display point (cx, cy) */
void        dv_viewport_zoom_at     (DvViewport *vp, double factor,
                                     double cx, double cy);

/* Box zoom -- commit rubber-band selection (display coords) */
void        dv_viewport_box_zoom    (DvViewport *vp,
                                     double x1, double y1,
                                     double x2, double y2);

/* Pan by delta (display pixels) */
void        dv_viewport_pan         (DvViewport *vp, double dx, double dy);

/* Clamp pan to keep image on screen */
void        dv_viewport_clamp_pan   (DvViewport *vp);

/* Rotate view 90 degrees clockwise or counter-clockwise */
void        dv_viewport_rotate     (DvViewport *vp, int degrees_cw);

/* Flip horizontally or vertically */
void        dv_viewport_flip       (DvViewport *vp, gboolean horizontal);

/* Get current rotation/flip state */
void        dv_viewport_get_transform_full (const DvViewport *vp,
                                             double *zoom, double *pan_x, double *pan_y,
                                             int *rotation, gboolean *flip_h, gboolean *flip_v);

/* TRUE if there is at least one zoom step to undo */
void        dv_viewport_clear_history (DvViewport *vp);
gboolean    dv_viewport_has_history (const DvViewport *vp);

/* Step back one level in hard zoom history */
void        dv_viewport_undo        (DvViewport *vp);

/* Reset hard zoom, return to soft/fit */
void        dv_viewport_reset       (DvViewport *vp);

/* Map display coordinate to image coordinate */
void        dv_viewport_display_to_image (const DvViewport *vp,
                                          double dx, double dy,
                                          double *ix, double *iy);

/* Get the effective transform (soft or hard) */
void        dv_viewport_get_transform (const DvViewport *vp,
                                       double *out_zoom,
                                       double *out_pan_x,
                                       double *out_pan_y);

/* True if we're in a hard-zoom state (affects export) */
gboolean    dv_viewport_is_hard     (const DvViewport *vp);

/* Get crop region in image coordinates (only valid if has_crop) */
gboolean    dv_viewport_get_crop    (const DvViewport *vp,
                                     double *x, double *y,
                                     double *w, double *h);
/* Compute the source image rectangle currently visible in the widget.
 * Use this for export to capture exactly what the user sees. */
void dv_viewport_get_export_rect (const DvViewport *vp,
                                  int               native_w,
                                  int               native_h,
                                  double           *out_x,
                                  double           *out_y,
                                  double           *out_w,
                                  double           *out_h);
