#include "dv_viewport.h"
#include <math.h>
#include <string.h>

void
dv_viewport_init (DvViewport *vp)
{
    memset (vp, 0, sizeof (*vp));
    vp->soft_mode  = TRUE;
    vp->view_mode  = DV_VIEW_MODE_FIT;
    vp->rotation  = 0;
    vp->flip_h    = FALSE;
    vp->flip_v    = FALSE;
    vp->zoom      = 1.0;
    vp->soft_zoom = 1.0;
    vp->history   = g_array_new (FALSE, FALSE, sizeof (DvZoomState));
}

void
dv_viewport_free (DvViewport *vp)
{
    if (vp->history)
        g_array_free (vp->history, TRUE);
}

void
dv_viewport_set_image (DvViewport *vp, int img_w, int img_h)
{
    vp->image_w = img_w;
    vp->image_h = img_h;
    dv_viewport_fit (vp);
}

void
dv_viewport_set_widget (DvViewport *vp, int w, int h)
{
    int old_w = vp->widget_w;
    int old_h = vp->widget_h;
    vp->widget_w = w;
    vp->widget_h = h;
    dv_viewport_fit (vp);

    /* For hard zoom: keep the same image centre point visible.
     * The image centre in widget coords was (widget/2 - pan) / zoom.
     * With a new widget size, shift pan so the same image point is centred. */
    if (dv_viewport_is_hard (vp) && old_w > 0 && old_h > 0 && vp->zoom > 0.0) {
        /* Image coordinate of old widget centre */
        double cx = (old_w / 2.0 - vp->pan_x) / vp->zoom;
        double cy = (old_h / 2.0 - vp->pan_y) / vp->zoom;
        /* New pan to keep that image point at new widget centre */
        vp->pan_x = w / 2.0 - cx * vp->zoom;
        vp->pan_y = h / 2.0 - cy * vp->zoom;
    }
}

void
dv_viewport_fit (DvViewport *vp)
{
    if (vp->image_w == 0 || vp->image_h == 0 ||
        vp->widget_w == 0 || vp->widget_h == 0)
        return;

    double sx = (double) vp->widget_w / vp->image_w;
    double sy = (double) vp->widget_h / vp->image_h;
    vp->soft_zoom  = fmin (sx, sy);
    vp->soft_pan_x = (vp->widget_w - vp->image_w * vp->soft_zoom) / 2.0;
    vp->soft_pan_y = (vp->widget_h - vp->image_h * vp->soft_zoom) / 2.0;
}

/* Push current hard state onto history */
static void
_push_history (DvViewport *vp)
{
    DvZoomState s = {
        .zoom     = vp->zoom,
        .pan_x    = vp->pan_x,
        .pan_y    = vp->pan_y,
        .has_crop = vp->has_crop,
        .crop_x   = vp->crop_x,
        .crop_y   = vp->crop_y,
        .crop_w   = vp->crop_w,
        .crop_h   = vp->crop_h,
    };
    g_array_append_val (vp->history, s);
}

/* Enter hard mode from soft if needed */
static void
_enter_hard (DvViewport *vp)
{
    if (vp->soft_mode) {
        vp->soft_mode = FALSE;
        vp->zoom  = vp->soft_zoom;
        vp->pan_x = vp->soft_pan_x;
        vp->pan_y = vp->soft_pan_y;
        dv_viewport_clamp_pan (vp);
    }
}

void
dv_viewport_zoom_at (DvViewport *vp, double factor, double cx, double cy)
{
    _push_history (vp);
    _enter_hard (vp);

    /* Keep the point under cursor fixed */
    double old_zoom = vp->zoom;
    double new_zoom = old_zoom * factor;
    /* Clamp zoom between 0.01x and 64x */
    new_zoom = fmax (0.01, new_zoom);

    vp->pan_x = cx - (cx - vp->pan_x) * (new_zoom / old_zoom);
    vp->pan_y = cy - (cy - vp->pan_y) * (new_zoom / old_zoom);
    /* No upper zoom limit -- viewport can zoom to any level.
     * SVG render size is clamped separately in dv_engine_get_svg(). */
    vp->zoom  = new_zoom;
    vp->has_crop = FALSE;
    dv_viewport_clamp_pan (vp);
}

void
dv_viewport_box_zoom (DvViewport *vp,
                      double x1, double y1,
                      double x2, double y2)
{
    if (fabs (x2 - x1) < 4 || fabs (y2 - y1) < 4)
        return; /* Too small -- ignore */

    _push_history (vp);
    _enter_hard (vp);

    /* Normalise so x1,y1 is top-left */
    if (x1 > x2) { double t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { double t = y1; y1 = y2; y2 = t; }

    /* Convert display coords to image coords */
    double ix1, iy1, ix2, iy2;
    dv_viewport_display_to_image (vp, x1, y1, &ix1, &iy1);
    dv_viewport_display_to_image (vp, x2, y2, &ix2, &iy2);

    /* Clamp to image bounds */
    ix1 = fmax (0, fmin (vp->image_w, ix1));
    iy1 = fmax (0, fmin (vp->image_h, iy1));
    ix2 = fmax (0, fmin (vp->image_w, ix2));
    iy2 = fmax (0, fmin (vp->image_h, iy2));

    double cw = ix2 - ix1;
    double ch = iy2 - iy1;

    if (cw < 1 || ch < 1)
        return;

    /* Zoom so the crop region fills the widget */
    double sx = vp->widget_w / cw;
    double sy = vp->widget_h / ch;
    double new_zoom = fmin (sx, sy);

    /* Centre the crop region */
    double pan_x = (vp->widget_w - cw * new_zoom) / 2.0 - ix1 * new_zoom;
    double pan_y = (vp->widget_h - ch * new_zoom) / 2.0 - iy1 * new_zoom;

    vp->zoom     = new_zoom;
    vp->pan_x    = pan_x;
    vp->pan_y    = pan_y;
    vp->has_crop = TRUE;
    vp->crop_x   = ix1;
    vp->crop_y   = iy1;
    vp->crop_w   = cw;
    vp->crop_h   = ch;
}

void
dv_viewport_clamp_pan (DvViewport *vp)
{
    if (vp->image_w == 0 || vp->widget_w == 0) return;

    double img_pw = vp->image_w * vp->zoom;
    double img_ph = vp->image_h * vp->zoom;

    if (img_pw <= vp->widget_w) {
        /* Image fits horizontally -- centre it, no horizontal pan */
        vp->pan_x = (vp->widget_w - img_pw) / 2.0;
    } else {
        /* Image wider than window -- clamp so no blank space on either side */
        if (vp->pan_x > 0.0)            vp->pan_x = 0.0;
        if (vp->pan_x < vp->widget_w - img_pw) vp->pan_x = vp->widget_w - img_pw;
    }

    if (img_ph <= vp->widget_h) {
        /* Image fits vertically -- centre it, no vertical pan */
        vp->pan_y = (vp->widget_h - img_ph) / 2.0;
    } else {
        /* Image taller than window -- clamp so no blank space on either side */
        if (vp->pan_y > 0.0)            vp->pan_y = 0.0;
        if (vp->pan_y < vp->widget_h - img_ph) vp->pan_y = vp->widget_h - img_ph;
    }
}

void
dv_viewport_pan (DvViewport *vp, double dx, double dy)
{
    _enter_hard (vp);
    vp->pan_x += dx;
    vp->pan_y += dy;
    dv_viewport_clamp_pan (vp);
}

void
dv_viewport_rotate (DvViewport *vp, int degrees_cw)
{
    vp->rotation = ((vp->rotation + degrees_cw) % 360 + 360) % 360;
}

void
dv_viewport_flip (DvViewport *vp, gboolean horizontal)
{
    if (horizontal)
        vp->flip_h = !vp->flip_h;
    else
        vp->flip_v = !vp->flip_v;
}

void
dv_viewport_get_transform_full (const DvViewport *vp,
                                 double *zoom, double *pan_x, double *pan_y,
                                 int *rotation, gboolean *flip_h, gboolean *flip_v)
{
    dv_viewport_get_transform (vp, zoom, pan_x, pan_y);
    *rotation = vp->rotation;
    *flip_h   = vp->flip_h;
    *flip_v   = vp->flip_v;
}

/* Clear all hard-zoom history (call when resetting to a base mode).
 * Only clears history + returns to soft rendering -- does NOT touch
 * view_mode. Setting the baseline mode is the caller's responsibility. */
void
dv_viewport_clear_history (DvViewport *vp)
{
    if (vp->history) g_array_set_size (vp->history, 0);
    vp->soft_mode  = TRUE;
}

gboolean
dv_viewport_has_history (const DvViewport *vp)
{
    return vp->history && vp->history->len > 0;
}

void
dv_viewport_undo (DvViewport *vp)
{
    guint n = vp->history->len;
    if (n == 0) {
        /* Nothing to undo -- a_undo is disabled when history is empty,
         * so this is a defensive no-op, not the normal path. */
        return;
    }

    DvZoomState *s = &g_array_index (vp->history, DvZoomState, n - 1);
    vp->zoom     = s->zoom;
    vp->pan_x    = s->pan_x;
    vp->pan_y    = s->pan_y;
    vp->has_crop = s->has_crop;
    vp->crop_x   = s->crop_x;
    vp->crop_y   = s->crop_y;
    vp->crop_w   = s->crop_w;
    vp->crop_h   = s->crop_h;
    g_array_remove_index (vp->history, n - 1);

    /* Undo Zoom only touches zoom/pan/crop -- rotation, flip, and
     * view_mode are separate concerns and must not be reset here. */
    if (vp->history->len == 0 &&
        vp->view_mode == DV_VIEW_MODE_FIT &&
        fabs (vp->zoom - vp->soft_zoom) < 0.001)
        vp->soft_mode  = TRUE;
}

void
dv_viewport_reset (DvViewport *vp)
{
    g_array_set_size (vp->history, 0);
    vp->soft_mode  = TRUE;
    vp->view_mode  = DV_VIEW_MODE_FIT;
    vp->rotation  = 0;
    vp->flip_h    = FALSE;
    vp->flip_v    = FALSE;
    vp->has_crop  = FALSE;
    vp->zoom      = vp->soft_zoom;
    vp->pan_x     = vp->soft_pan_x;
    vp->pan_y     = vp->soft_pan_y;
}

void
dv_viewport_display_to_image (const DvViewport *vp,
                               double dx, double dy,
                               double *ix, double *iy)
{
    double z   = vp->soft_mode ? vp->soft_zoom : vp->zoom;
    double px  = vp->soft_mode ? vp->soft_pan_x : vp->pan_x;
    double py  = vp->soft_mode ? vp->soft_pan_y : vp->pan_y;

    *ix = (dx - px) / z;
    *iy = (dy - py) / z;
}

void
dv_viewport_get_transform (const DvViewport *vp,
                            double *out_zoom,
                            double *out_pan_x,
                            double *out_pan_y)
{
    if (vp->soft_mode) {
        *out_zoom  = vp->soft_zoom;
        *out_pan_x = vp->soft_pan_x;
        *out_pan_y = vp->soft_pan_y;
    } else {
        *out_zoom  = vp->zoom;
        *out_pan_x = vp->pan_x;
        *out_pan_y = vp->pan_y;
    }
}

gboolean
dv_viewport_is_hard (const DvViewport *vp)
{
    return !vp->soft_mode;
}

gboolean
dv_viewport_get_crop (const DvViewport *vp,
                      double *x, double *y,
                      double *w, double *h)
{
    if (!vp->has_crop)
        return FALSE;
    *x = vp->crop_x;
    *y = vp->crop_y;
    *w = vp->crop_w;
    *h = vp->crop_h;
    return TRUE;
}

/*
 * Compute the visible image rectangle for export.
 * This is the region of the SOURCE image currently visible in the widget,
 * accounting for zoom and pan. If no hard zoom, returns the full image.
 * out_x/y/w/h are in source image pixels (may be fractional, clamped to image bounds).
 */
void
dv_viewport_get_export_rect (const DvViewport *vp,
                              int               native_w,
                              int               native_h,
                              double           *out_x,
                              double           *out_y,
                              double           *out_w,
                              double           *out_h)
{
    if (!dv_viewport_is_hard (vp) || vp->zoom <= 0.0) {
        /* No hard zoom -- export full image */
        *out_x = 0; *out_y = 0;
        *out_w = native_w; *out_h = native_h;
        return;
    }

    /* Widget corners in image coordinates:
     *   image_x = (widget_x - pan_x) / zoom
     *   image_y = (widget_y - pan_y) / zoom  */
    double x0 = (0.0          - vp->pan_x) / vp->zoom;
    double y0 = (0.0          - vp->pan_y) / vp->zoom;
    double x1 = (vp->widget_w - vp->pan_x) / vp->zoom;
    double y1 = (vp->widget_h - vp->pan_y) / vp->zoom;

    /* Clamp to image bounds */
    if (x0 < 0)         x0 = 0;
    if (y0 < 0)         y0 = 0;
    if (x1 > native_w)  x1 = native_w;
    if (y1 > native_h)  y1 = native_h;

    *out_x = x0;
    *out_y = y0;
    *out_w = x1 - x0;
    *out_h = y1 - y0;
}

