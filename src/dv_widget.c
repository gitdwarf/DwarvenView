#include "dv_widget.h"
#include "dv_props_dialog.h"
#include <locale.h>
#include "dv_wallpaper.h"
#include "dv_themeicon.h"

#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <math.h>
#include <string.h>

/* ── Widget private data ────────────────────────────────────────────────── */

struct _DvWidget {
    GtkWidget   parent;

    DvEngine   *engine;
    DvViewport  viewport;
    char       *current_path;

    /* Current display texture (updated by engine pass callback) */
    GdkTexture *display_tex;

    /* Last known mouse position (for zoom centre) */
    double      last_mx, last_my;

    /* Pan inversion prefs */
    DvPanPrefs  pan_prefs;

    /* User preferences (not owned) */
    const DvPrefs *prefs;

    /* Persistent widget action group -- installed at init */
    GSimpleActionGroup *action_group;
    GSimpleAction      *a_undo;
    GSimpleAction      *a_reset;
    GSimpleAction      *a_export;

    /* One-shot: apply prefs->default_view_mode on the next final load pass.
     * Set by dv_widget_load, cancelled by dv_widget_load_keep_zoom so a
     * zoom-preserving reload never overwrites the restored view_mode. */
    gboolean            apply_default_view_mode;

    /* Export queue -- pending exports when one is already running */
    GQueue             *export_queue;
    gboolean            export_running;

    /* Drag state */
    gboolean    dragging;
    double      drag_start_x, drag_start_y;
    double      drag_pan_start_x, drag_pan_start_y;

    /* Box zoom rubber-band state */
    gboolean    boxing;
    double      box_x1, box_y1;
    double      box_x2, box_y2;

    /* Redraw timer for animation */
    guint       redraw_timer;

    /* Signals */
    guint       sig_load_complete;
    guint       sig_zoom_changed;
};

G_DEFINE_TYPE (DvWidget, dv_widget, GTK_TYPE_WIDGET)

/* ── Signals ────────────────────────────────────────────────────────────── */

enum {
    SIGNAL_LOAD_COMPLETE,
    SIGNAL_ZOOM_CHANGED,
    SIGNAL_TRANSFORM_CHANGED,
    SIGNAL_HELP_REQUESTED,
    SIGNAL_PREFS_REQUESTED,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* ExportTask -- must be before forward declarations that reference it */
typedef struct {
    const DvExportFormat *fmt;
    char                 *source_path;
} ExportTask;

/* ── Forward declarations ───────────────────────────────────────────────── */

static void     dv_widget_snapshot      (GtkWidget *w, GtkSnapshot *snap);
static void     dv_widget_size_allocate (GtkWidget *w, int width, int height, int baseline);
static void     dv_widget_dispose       (GObject *obj);

static void     setup_gesture_controllers  (DvWidget *self);
static void     on_menu_undo_zoom          (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_reset_zoom         (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_export             (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_toggle_fullscreen  (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_quit               (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_rotate_cw          (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_rotate_ccw         (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_flip_h             (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_flip_v             (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_send_to            (GSimpleAction *a, GVariant *p, gpointer data);
static void     export_thread_func         (GTask *task, gpointer src, gpointer data, GCancellable *cancel);
static void     on_export_done             (GObject *src, GAsyncResult *res, gpointer data);
static void     start_export               (DvWidget *self, const DvExportFormat *fmt);
static void     export_task_free           (ExportTask *et);
static void     on_menu_set_wallpaper      (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_reset_transform    (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_menu_properties         (GSimpleAction *a, GVariant *p, gpointer data);
static void     on_engine_pass            (GdkTexture *tex, int pw, int nw, int nh,
                                           gboolean final, gpointer user_data);
static gboolean redraw_tick              (gpointer data);

/* ── Context menu ───────────────────────────────────────────────────────── */

static void     show_context_menu       (DvWidget *self, double x, double y);
static void     dv_widget_activate_help       (DvWidget *self, GVariant *p, GSimpleAction *a);
static void     dv_widget_activate_prefs      (DvWidget *self, GVariant *p, GSimpleAction *a);
static void     dv_widget_toggle_zoom_actual  (DvWidget *self, GVariant *p, GSimpleAction *a);

/* ── Class init ─────────────────────────────────────────────────────────── */

static void
dv_widget_class_init (DvWidgetClass *klass)
{
    GObjectClass   *obj_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wgt_class = GTK_WIDGET_CLASS (klass);

    obj_class->dispose          = dv_widget_dispose;
    wgt_class->snapshot         = dv_widget_snapshot;
    wgt_class->size_allocate    = dv_widget_size_allocate;

    signals[SIGNAL_TRANSFORM_CHANGED] = g_signal_new (
        "transform-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_HELP_REQUESTED] = g_signal_new (
        "help-requested",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_PREFS_REQUESTED] = g_signal_new (
        "prefs-requested",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_LOAD_COMPLETE] = g_signal_new (
        "load-complete",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL,
        NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    signals[SIGNAL_ZOOM_CHANGED] = g_signal_new (
        "zoom-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL,
        NULL, G_TYPE_NONE, 1, G_TYPE_DOUBLE);

    gtk_widget_class_set_css_name (wgt_class, "dv-widget");
}

/* The picture shown for audio with no cover art: the current icon theme's audio file icon.
 * Rebuilt when the user changes the icon theme. */
static void
refresh_audio_icon (DvWidget *self)
{
    GdkTexture *icon = dv_theme_icon_texture ("audio-x-generic", 256);
    dv_engine_set_audio_icon (self->engine, icon);
    g_clear_object (&icon);
}

static void
on_icon_theme_changed (GObject *settings, GParamSpec *pspec, gpointer self)
{
    (void) settings; (void) pspec;
    refresh_audio_icon (self);
}

static void
dv_widget_init (DvWidget *self)
{
    self->engine = dv_engine_new ();
    dv_engine_init_placeholder (self->engine);   /* must be on main thread */
    refresh_audio_icon (self);
    GtkSettings *settings = gtk_settings_get_default ();
    if (settings)
        g_signal_connect_object (settings, "notify::gtk-icon-theme-name",
                                 G_CALLBACK (on_icon_theme_changed), self, 0);
    dv_viewport_init (&self->viewport);

    gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
    gtk_widget_set_hexpand   (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand   (GTK_WIDGET (self), TRUE);

    /* Create persistent "widget" action group so toolbar popovers
     * can always find widget.export, widget.undo-zoom, etc. */
    self->action_group = g_simple_action_group_new ();

    self->a_undo   = g_simple_action_new ("undo-zoom", NULL);
    self->a_reset  = g_simple_action_new ("reset-zoom", NULL);
    self->a_export = g_simple_action_new ("export", G_VARIANT_TYPE_INT32);

    GSimpleAction *a_fs      = g_simple_action_new ("toggle-fullscreen", NULL);
    GSimpleAction *a_quit    = g_simple_action_new ("quit", NULL);
    GSimpleAction *a_help_w  = g_simple_action_new ("help", NULL);
    GSimpleAction *a_prefs_w = g_simple_action_new ("prefs", NULL);
    GSimpleAction *a_setwp   = g_simple_action_new ("set-wallpaper", NULL);
    GSimpleAction *a_send    = g_simple_action_new ("send-to", G_VARIANT_TYPE_STRING);
    GSimpleAction *a_zoom_act    = g_simple_action_new ("zoom-actual",     NULL);
    GSimpleAction *a_reset_xform = g_simple_action_new ("reset-transform", NULL);
    GSimpleAction *a_rot_cw      = g_simple_action_new ("rotate-cw",  NULL);
    GSimpleAction *a_rot_ccw = g_simple_action_new ("rotate-ccw", NULL);
    GSimpleAction *a_flip_h  = g_simple_action_new ("flip-h",     NULL);
    GSimpleAction *a_flip_v  = g_simple_action_new ("flip-v",     NULL);
    GSimpleAction *a_props_w = g_simple_action_new ("properties", NULL);

    /* Disable image-dependent actions until an image is loaded */
    GSimpleAction *img_actions[] = {
        self->a_undo, self->a_reset, self->a_export,
        a_setwp, a_send, a_reset_xform,
        a_rot_cw, a_rot_ccw, a_flip_h, a_flip_v, a_props_w, NULL
    };
    for (int i = 0; img_actions[i]; i++)
        g_simple_action_set_enabled (img_actions[i], FALSE);

    g_signal_connect (self->a_undo,   "activate", G_CALLBACK (on_menu_undo_zoom),        self);
    g_signal_connect (self->a_reset,  "activate", G_CALLBACK (on_menu_reset_zoom),       self);
    g_signal_connect (self->a_export, "activate", G_CALLBACK (on_menu_export),           self);
    g_signal_connect (a_fs,      "activate", G_CALLBACK (on_menu_toggle_fullscreen), self);
    g_signal_connect (a_quit,    "activate", G_CALLBACK (on_menu_quit),              self);
    g_signal_connect (a_setwp,   "activate", G_CALLBACK (on_menu_set_wallpaper),       self);
    g_signal_connect (a_send,    "activate", G_CALLBACK (on_menu_send_to),            self);
    g_signal_connect (a_reset_xform, "activate", G_CALLBACK (on_menu_reset_transform), self);
    g_signal_connect (a_rot_cw,      "activate", G_CALLBACK (on_menu_rotate_cw),         self);
    g_signal_connect (a_rot_ccw, "activate", G_CALLBACK (on_menu_rotate_ccw),        self);
    g_signal_connect (a_flip_h,  "activate", G_CALLBACK (on_menu_flip_h),            self);
    g_signal_connect (a_flip_v,  "activate", G_CALLBACK (on_menu_flip_v),            self);
    g_signal_connect (a_props_w, "activate", G_CALLBACK (on_menu_properties),        self);

    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (self->a_undo));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (self->a_reset));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_zoom_act));
    g_simple_action_set_enabled (a_zoom_act, FALSE);
    g_signal_connect_swapped (a_zoom_act, "activate",
        G_CALLBACK (dv_widget_toggle_zoom_actual), self);
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (self->a_export));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_fs));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_quit));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_help_w));
    g_signal_connect_swapped (a_help_w, "activate",
        G_CALLBACK (dv_widget_activate_help), self);
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_prefs_w));
    g_signal_connect_swapped (a_prefs_w, "activate",
        G_CALLBACK (dv_widget_activate_prefs), self);
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_setwp));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_send));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_reset_xform));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_rot_cw));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_rot_ccw));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_flip_h));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_flip_v));
    g_action_map_add_action (G_ACTION_MAP (self->action_group), G_ACTION (a_props_w));

    gtk_widget_insert_action_group (GTK_WIDGET (self), "widget",
                                    G_ACTION_GROUP (self->action_group));

    self->export_queue   = g_queue_new ();   /* items freed by export_task_free */
    self->export_running = FALSE;

    setup_gesture_controllers (self);
}

static void
dv_widget_dispose (GObject *obj)
{
    DvWidget *self = DV_WIDGET (obj);

    /* Children (incl. any open context popups) cleaned up by parent class */

    if (self->redraw_timer) {
        g_source_remove (self->redraw_timer);
        self->redraw_timer = 0;
    }

    g_clear_object (&self->display_tex);
    if (self->export_queue) {
        g_queue_free_full (self->export_queue, (GDestroyNotify) export_task_free);
        self->export_queue = NULL;
    }
    g_clear_object (&self->action_group);
    g_clear_object (&self->a_undo);
    g_clear_object (&self->a_reset);
    g_clear_object (&self->a_export);
    dv_engine_free (self->engine);
    dv_viewport_free (&self->viewport);
    g_free (self->current_path);

    G_OBJECT_CLASS (dv_widget_parent_class)->dispose (obj);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

GtkWidget *
dv_widget_new (void)
{
    return g_object_new (DV_TYPE_WIDGET, NULL);
}

void
dv_widget_load (DvWidget *self, const char *path)
{
    g_free (self->current_path);
    self->current_path = g_strdup (path);

    /* Properties follows the image: a no-op if the window isn't open. */
    dv_props_dialog_notify_path (self->current_path);

    /* Enable all image-dependent widget actions -- except reset-transform
     * and reset-zoom which are managed separately based on state */
    {
        const char *img_act[] = {
            "undo-zoom", "zoom-actual", "export",
            "set-wallpaper", "send-to",
            "rotate-cw", "rotate-ccw", "flip-h", "flip-v", "properties", NULL
        };
        for (int i = 0; img_act[i]; i++) {
            GAction *act = g_action_map_lookup_action (
                G_ACTION_MAP (self->action_group), img_act[i]);
            if (act) g_simple_action_set_enabled (G_SIMPLE_ACTION (act), TRUE);
        }
    }

    g_clear_object (&self->display_tex);
    dv_viewport_reset (&self->viewport);
    /* Clear stale image dimensions so no border flash on image change */
    self->viewport.image_w = 0;
    self->viewport.image_h = 0;
    /* Reset view transform */
    self->viewport.rotation = 0;
    self->viewport.flip_h   = FALSE;
    self->viewport.flip_v   = FALSE;
    self->viewport.soft_zoom  = 1.0;
    self->viewport.soft_pan_x = 0.0;
    self->viewport.soft_pan_y = 0.0;

    /* Apply the user's preferred default view mode once dimensions are
     * known -- see on_engine_pass. */
    self->apply_default_view_mode = TRUE;

    dv_engine_load (self->engine, path, on_engine_pass, self,
        !self->prefs || self->prefs->show_loading_splash,
        self->prefs ? self->prefs->loading_splash_delay : 0.0);

    /* Grab keyboard focus so arrow keys work */
    gtk_widget_grab_focus (GTK_WIDGET (self));

    /* Ensure redraw timer is running */
    if (!self->redraw_timer)
        self->redraw_timer = g_timeout_add (16, redraw_tick, self);
}

void
dv_widget_load_keep_zoom (DvWidget *self, const char *path)
{
    /* Save zoom/pan/crop/transform state */
    gboolean   soft_mode  = self->viewport.soft_mode;
    DvViewMode view_mode  = self->viewport.view_mode;
    double     zoom       = self->viewport.zoom;
    double     pan_x      = self->viewport.pan_x;
    double     pan_y      = self->viewport.pan_y;
    gboolean   has_crop   = self->viewport.has_crop;
    double     crop_x     = self->viewport.crop_x;
    double     crop_y     = self->viewport.crop_y;
    double     crop_w     = self->viewport.crop_w;
    double     crop_h     = self->viewport.crop_h;
    int        rotation   = self->viewport.rotation;
    gboolean   flip_h     = self->viewport.flip_h;
    gboolean   flip_v     = self->viewport.flip_v;

    dv_widget_load (self, path);

    /* Restore zoom/pan/crop/transform */
    self->viewport.soft_mode = soft_mode;
    self->viewport.view_mode = view_mode;
    self->viewport.zoom      = zoom;
    self->viewport.pan_x     = pan_x;
    self->viewport.pan_y     = pan_y;
    self->viewport.has_crop  = has_crop;
    self->viewport.crop_x    = crop_x;
    self->viewport.crop_y    = crop_y;
    self->viewport.crop_w    = crop_w;
    self->viewport.crop_h    = crop_h;
    self->viewport.rotation  = rotation;
    self->viewport.flip_h    = flip_h;
    self->viewport.flip_v    = flip_v;

    /* State was just explicitly restored above -- the fresh-load default
     * (set inside dv_widget_load) must not be applied on top of it. */
    self->apply_default_view_mode = FALSE;
}

const char *
dv_widget_get_path (DvWidget *self)
{
    return self->current_path;
}

void
dv_widget_set_pan_prefs (DvWidget *self, const DvPanPrefs *pan)
{
    self->pan_prefs = *pan;
}

void
dv_widget_set_prefs (DvWidget *self, const DvPrefs *prefs)
{
    self->prefs = prefs;
}

void dv_widget_rotate_cw  (DvWidget *self)
{
    dv_viewport_rotate (&self->viewport, 90);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_TRANSFORM_CHANGED], 0);
}
void dv_widget_rotate_ccw (DvWidget *self)
{
    dv_viewport_rotate (&self->viewport, -90);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_TRANSFORM_CHANGED], 0);
}
void dv_widget_flip_h     (DvWidget *self)
{
    dv_viewport_flip (&self->viewport, TRUE);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_TRANSFORM_CHANGED], 0);
}
void dv_widget_flip_v (DvWidget *self)
{
    dv_viewport_flip (&self->viewport, FALSE);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_TRANSFORM_CHANGED], 0);
}

DvEngine *
dv_widget_get_engine (DvWidget *self)
{
    return self->engine;
}

GActionGroup *
dv_widget_get_action_group (DvWidget *self)
{
    return G_ACTION_GROUP (self->action_group);
}

gboolean
dv_widget_is_actual_size (DvWidget *self)
{
    return self->viewport.view_mode == DV_VIEW_MODE_ACTUAL;
}

gboolean
dv_widget_has_zoom_history (DvWidget *self)
{
    return dv_viewport_has_history (&self->viewport);
}

gboolean
dv_widget_has_transform (DvWidget *self)
{
    return self->viewport.rotation != 0 ||
           self->viewport.flip_h ||
           self->viewport.flip_v;
}

void dv_widget_reset_zoom (DvWidget *self)
{
    /* Reset to user's chosen baseline view mode (checked BEFORE any
     * history-clearing call, since clearing must never decide the mode) */
    if (self->viewport.view_mode == DV_VIEW_MODE_ACTUAL) {
        /* zoom_actual clears its own history and re-applies 1:1 */
        dv_widget_zoom_actual (self);
    } else {
        dv_viewport_clear_history (&self->viewport);
        dv_viewport_fit (&self->viewport);
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

void dv_widget_reset_transform (DvWidget *self)
{
    self->viewport.rotation = 0;
    self->viewport.flip_h   = FALSE;
    self->viewport.flip_v   = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_TRANSFORM_CHANGED], 0);
}

static void
dv_widget_toggle_zoom_actual (DvWidget *self, GVariant *p, GSimpleAction *a)
{
    (void) p; (void) a;
    /* Toggle: 1:1 if not at 1:1, fit to viewport if already at 1:1 */
    gboolean is_actual = (self->viewport.view_mode == DV_VIEW_MODE_ACTUAL);
    if (is_actual) {
        /* Switch view mode to fit -- clears history */
        self->viewport.view_mode = DV_VIEW_MODE_FIT;
        dv_viewport_clear_history (&self->viewport);
        dv_viewport_fit (&self->viewport);
        gtk_widget_queue_draw (GTK_WIDGET (self));
        g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0, self->viewport.soft_zoom);
    } else {
        dv_widget_zoom_actual (self);
    }
}

void dv_widget_zoom_actual (DvWidget *self)
{
    int nw = 0, nh = 0;
    dv_engine_get_size (self->engine, &nw, &nh);
    if (nw <= 0 || nh <= 0) return;

    /* Set view mode */
    self->viewport.view_mode = DV_VIEW_MODE_ACTUAL;
    self->viewport.soft_mode = FALSE;
    self->viewport.has_crop  = FALSE;

    /* Set zoom directly to 1:1, centred -- no zoom_at (which pushes history) */
    self->viewport.zoom  = 1.0;
    self->viewport.pan_x = (self->viewport.widget_w - nw) / 2.0;
    self->viewport.pan_y = (self->viewport.widget_h - nh) / 2.0;

    /* Clear history AFTER setting position -- this is the new baseline */
    if (self->viewport.history) g_array_set_size (self->viewport.history, 0);

    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0, self->viewport.zoom);
}

void dv_widget_zoom_centre (DvWidget *self, double factor)
{
    double cx = gtk_widget_get_width  (GTK_WIDGET (self)) / 2.0;
    double cy = gtk_widget_get_height (GTK_WIDGET (self)) / 2.0;
    dv_viewport_zoom_at (&self->viewport, factor, cx, cy);
    g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0, self->viewport.zoom);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
dv_widget_get_status (DvWidget *self,
                      int      *out_native_w,
                      int      *out_native_h,
                      double   *out_zoom,
                      gboolean *out_is_hard,
                      gboolean *out_has_crop)
{
    dv_engine_get_size (self->engine, out_native_w, out_native_h);
    double pan_x, pan_y;
    dv_viewport_get_transform (&self->viewport, out_zoom, &pan_x, &pan_y);
    *out_is_hard  = dv_viewport_is_hard (&self->viewport);
    *out_has_crop = self->viewport.has_crop;
}

DvExportResult *
dv_widget_export (DvWidget *self, const DvExportFormat *fmt)
{
    if (!self->current_path) return NULL;
    return dv_export (self->current_path, fmt, self->engine,
                      &self->viewport, self->prefs);
}

/* ── Engine pass callback ───────────────────────────────────────────────── */

static void
on_engine_pass (GdkTexture *tex,
                int pw, int nw, int nh,
                gboolean final,
                gpointer user_data)
{
    (void) pw;
    DvWidget *self = DV_WIDGET (user_data);

    if (tex) {
        g_clear_object (&self->display_tex);
        self->display_tex = g_object_ref (tex);
    }

    /* Update viewport image size every pass so the low-res passes
     * are stretched to fill the window -- blurry first, sharp later.
     * Use the NATIVE dimensions (not pass dimensions) so the fit
     * zoom stays constant across passes. */
    if (nw > 0 && nh > 0) {
        /* Set image size from native dims -- constant across all passes */
        if (self->viewport.image_w != nw || self->viewport.image_h != nh)
            dv_viewport_set_image (&self->viewport, nw, nh);
        /* The texture may be smaller (early pass) -- refit each time
         * so the display always fills the window. */
        if (self->viewport.soft_mode)
            dv_viewport_fit (&self->viewport);
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));

    if (final) {
        if (self->apply_default_view_mode) {
            self->apply_default_view_mode = FALSE;
            if (self->prefs &&
                self->prefs->default_view_mode == DV_VIEW_MODE_ACTUAL)
                dv_widget_zoom_actual (self);
        }
        g_signal_emit (self, signals[SIGNAL_LOAD_COMPLETE], 0, TRUE);
    }
}

/* ── Rendering ──────────────────────────────────────────────────────────── */

static void
dv_widget_snapshot (GtkWidget *w, GtkSnapshot *snap)
{
    DvWidget *self = DV_WIDGET (w);
    int width  = gtk_widget_get_width (w);
    int height = gtk_widget_get_height (w);

    /* Black background */
    GdkRGBA black = { 0, 0, 0, 1 };
    gtk_snapshot_append_color (snap, &black,
        &GRAPHENE_RECT_INIT (0, 0, width, height));

    GdkTexture *tex = NULL;

    if (dv_engine_is_svg (self->engine)) {
        /* SVG: render at native*zoom, apply same rotation/flip as raster */
        double zoom, pan_x, pan_y;
        dv_viewport_get_transform (&self->viewport, &zoom, &pan_x, &pan_y);
        int nw = self->viewport.image_w ? self->viewport.image_w : width;
        int nh = self->viewport.image_h ? self->viewport.image_h : height;
        int render_w = (int)(nw * zoom);
        int render_h = (int)(nh * zoom);
        tex = dv_engine_get_svg (self->engine, render_w, render_h);
        if (tex) {
            int      svg_rot   = self->viewport.rotation;
            gboolean svg_fh    = self->viewport.flip_h;
            gboolean svg_fv    = self->viewport.flip_v;
            gboolean has_xform = (svg_rot != 0 || svg_fh || svg_fv);

            gtk_snapshot_push_clip (snap,
                &GRAPHENE_RECT_INIT (0, 0, width, height));

            if (has_xform) {
                float cx = (float)pan_x + render_w * 0.5f;
                float cy = (float)pan_y + render_h * 0.5f;
                gtk_snapshot_save (snap);
                GskTransform *t = gsk_transform_new ();
                t = gsk_transform_translate (t, &GRAPHENE_POINT_INIT (cx, cy));
                if (svg_rot) t = gsk_transform_rotate (t, (float) svg_rot);
                if (svg_fh)  t = gsk_transform_scale (t, -1.0f, 1.0f);
                if (svg_fv)  t = gsk_transform_scale (t, 1.0f, -1.0f);
                t = gsk_transform_translate (t, &GRAPHENE_POINT_INIT (-cx, -cy));
                gtk_snapshot_transform (snap, t);
                gsk_transform_unref (t);
            }

            gtk_snapshot_append_scaled_texture (snap, tex,
                GSK_SCALING_FILTER_LINEAR,
                &GRAPHENE_RECT_INIT (pan_x, pan_y, render_w, render_h));

            if (has_xform) gtk_snapshot_restore (snap);
            gtk_snapshot_pop (snap);
        }
        goto draw_overlay;
    }

    if (dv_engine_is_animated (self->engine))
        tex = dv_engine_get_frame (self->engine);
    else
        tex = self->display_tex;

    if (!tex)
        goto draw_overlay;

    {
        double zoom, pan_x, pan_y;
        dv_viewport_get_transform (&self->viewport, &zoom, &pan_x, &pan_y);

        /* For animated images use actual frame dimensions.
         * For static images, use NATIVE viewport dimensions so low-res
         * progressive passes stretch to fill (blurry->sharp effect). */
        int native_w, native_h;
        if (dv_engine_is_animated (self->engine)) {
            native_w = gdk_texture_get_width (tex);
            native_h = gdk_texture_get_height (tex);
            /* Sync viewport if not yet set */
            if (self->viewport.image_w == 0)
                dv_viewport_set_image (&self->viewport, native_w, native_h);
        } else {
            native_w = self->viewport.image_w;
            native_h = self->viewport.image_h;
            if (native_w == 0) native_w = gdk_texture_get_width (tex);
            if (native_h == 0) native_h = gdk_texture_get_height (tex);
        }

        /* Image dimensions after rotation (swap W/H for 90/270) */
        int rot       = self->viewport.rotation;
        gboolean swap = (rot == 90 || rot == 270);
        (void) swap;

        float dst_x = (float) pan_x;
        float dst_y = (float) pan_y;

        /* Clip to widget bounds */
        gtk_snapshot_push_clip (snap,
            &GRAPHENE_RECT_INIT (0, 0, width, height));

        /* Apply rotation + flip as a GSK transform around image centre */
        gboolean has_xform = (rot != 0 || self->viewport.flip_h || self->viewport.flip_v);

        if (has_xform) {
            /* Centre of the (pre-rotation) image in display coords */
            float draw_w = (float)(native_w * zoom);
            float draw_h = (float)(native_h * zoom);
            float cx = dst_x + draw_w * 0.5f;
            float cy = dst_y + draw_h * 0.5f;

            /* gtk_snapshot_save/restore + gtk_snapshot_transform is the
             * correct GTK4 way to push a transform onto the snapshot stack */
            gtk_snapshot_save (snap);

            GskTransform *t = gsk_transform_new ();
            t = gsk_transform_translate (t, &GRAPHENE_POINT_INIT (cx, cy));
            if (rot != 0)
                t = gsk_transform_rotate (t, (float) rot);
            if (self->viewport.flip_h)
                t = gsk_transform_scale (t, -1.0f, 1.0f);
            if (self->viewport.flip_v)
                t = gsk_transform_scale (t, 1.0f, -1.0f);
            t = gsk_transform_translate (t, &GRAPHENE_POINT_INIT (-cx, -cy));
            gtk_snapshot_transform (snap, t);
            gsk_transform_unref (t);
        }

        /* Draw at native (pre-rotation) dimensions -- GSK handles the rotation */
        float draw_w = (float)(native_w * zoom);
        float draw_h = (float)(native_h * zoom);
        gtk_snapshot_append_scaled_texture (snap, tex,
            GSK_SCALING_FILTER_LINEAR,
            &GRAPHENE_RECT_INIT (dst_x, dst_y, draw_w, draw_h));

        if (has_xform)
            gtk_snapshot_restore (snap);   /* pop transform */
        gtk_snapshot_pop (snap);           /* pop clip */
    }

draw_overlay:
    /* ── Placeholder text overlay ──────────────────────────────────────── */
    if (dv_engine_is_placeholder (self->engine) || dv_engine_is_failed (self->engine)) {
        /* is_placeholder and is_failed are set together, on the main thread,
         * before this draw can run, so at most one is ever true here -- the
         * ?: just picks which text goes with whichever one it is. */
        const char *msg = dv_engine_is_failed (self->engine)
            ? "Not a valid image file"
            : "Loading\xe2\x80\xa6";
        PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), msg);
        PangoFontDescription *fd = pango_font_description_from_string ("Sans Bold 14");
        pango_layout_set_font_description (layout, fd);
        pango_font_description_free (fd);

        int tw, th;
        pango_layout_get_pixel_size (layout, &tw, &th);
        float tx = ((float)width  - tw) / 2.0f;
        float ty = (float)height * 0.72f;

        gtk_snapshot_save (snap);
        gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT (tx, ty));
        GdkRGBA shadow = { 0, 0, 0, 0.7 };
        gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT (1, 1));
        gtk_snapshot_append_layout (snap, layout, &shadow);
        gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT (-1, -1));
        GdkRGBA fg = { 1, 1, 1, 0.95 };
        gtk_snapshot_append_layout (snap, layout, &fg);
        gtk_snapshot_restore (snap);
        g_object_unref (layout);
    }

    /* Draw rubber-band box if active */
    if (self->boxing) {
        double x1 = fmin (self->box_x1, self->box_x2);
        double y1 = fmin (self->box_y1, self->box_y2);
        double x2 = fmax (self->box_x1, self->box_x2);
        double y2 = fmax (self->box_y1, self->box_y2);

        /* Semi-transparent fill */
        GdkRGBA fill = { 1, 1, 1, 0.15 };
        gtk_snapshot_append_color (snap, &fill,
            &GRAPHENE_RECT_INIT (x1, y1, x2 - x1, y2 - y1));

        /* Border lines (1px white) */
        GdkRGBA border = { 1, 1, 1, 0.8 };
        gtk_snapshot_append_color (snap, &border,
            &GRAPHENE_RECT_INIT (x1, y1, x2 - x1, 1));
        gtk_snapshot_append_color (snap, &border,
            &GRAPHENE_RECT_INIT (x1, y2 - 1, x2 - x1, 1));
        gtk_snapshot_append_color (snap, &border,
            &GRAPHENE_RECT_INIT (x1, y1, 1, y2 - y1));
        gtk_snapshot_append_color (snap, &border,
            &GRAPHENE_RECT_INIT (x2 - 1, y1, 1, y2 - y1));
    }
}

static void
dv_widget_size_allocate (GtkWidget *w, int width, int height, int baseline)
{
    GTK_WIDGET_CLASS (dv_widget_parent_class)
        ->size_allocate (w, width, height, baseline);

    DvWidget *self = DV_WIDGET (w);
    dv_viewport_set_widget (&self->viewport, width, height);
    gtk_widget_queue_draw (w);
}

static gboolean
redraw_tick (gpointer data)
{
    gtk_widget_queue_draw (GTK_WIDGET (data));
    return G_SOURCE_CONTINUE;
}

/* ── Input controllers ──────────────────────────────────────────────────── */

/*
 * Rotate / flip keys. Works only with an image loaded (same rule as the
 * toolbar buttons) and never on Ctrl/Alt combinations.
 *   R          rotate 90 clockwise        Shift+R   counter-clockwise
 *   L          rotate 90 counter-clockwise Shift+L   clockwise
 *   H / V      flip horizontal / vertical
 * Shift is read from the modifier bit and the keyval is lowercased, so Caps
 * Lock cannot invert the direction.
 * Returns TRUE if the key was consumed.
 */
static gboolean
handle_transform_key (DvWidget *self, guint keyval, GdkModifierType state)
{
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
        return FALSE;
    if (!self->current_path)
        return FALSE;

    gboolean shift = (state & GDK_SHIFT_MASK) != 0;

    switch (gdk_keyval_to_lower (keyval)) {
    case GDK_KEY_r:
        if (shift) dv_widget_rotate_ccw (self); else dv_widget_rotate_cw (self);
        return TRUE;
    case GDK_KEY_l:
        if (shift) dv_widget_rotate_cw (self);  else dv_widget_rotate_ccw (self);
        return TRUE;
    case GDK_KEY_h:
        dv_widget_flip_h (self);
        return TRUE;
    case GDK_KEY_v:
        dv_widget_flip_v (self);
        return TRUE;
    default:
        return FALSE;
    }
}

/* Keyboard */
static gboolean
on_key_pressed (GtkEventControllerKey *ctrl,
                guint keyval, guint keycode,
                GdkModifierType state,
                gpointer data)
{
    DvWidget *self = DV_WIDGET (data);
    int width  = gtk_widget_get_width (GTK_WIDGET (self));
    int height = gtk_widget_get_height (GTK_WIDGET (self));
    double cx  = width / 2.0;
    double cy  = height / 2.0;
    gboolean ctrl_held = (state & GDK_CONTROL_MASK) != 0;
    (void) ctrl;
    (void) keycode;

    /* ESC cancels rubber-band box draw, then steps back zoom history */
    if (keyval == GDK_KEY_Escape) {
        if (self->boxing) {
            self->boxing = FALSE;
            gtk_widget_queue_draw (GTK_WIDGET (self));
            return TRUE;
        }
        dv_viewport_undo (&self->viewport);
        g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0,
                       self->viewport.zoom);
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return TRUE;
    }

    if (handle_transform_key (self, keyval, state))
        return TRUE;

    /* Ctrl+Left and Ctrl+Right browse (the window handles them), they do not pan. */
    if (ctrl_held && (keyval == GDK_KEY_Left || keyval == GDK_KEY_Right))
        return FALSE;

    switch (keyval) {
    case GDK_KEY_Left:
        dv_viewport_pan (&self->viewport,
            self->pan_prefs.invert_arrow_lr ? 20 : -20, 0);
        break;
    case GDK_KEY_Right:
        dv_viewport_pan (&self->viewport,
            self->pan_prefs.invert_arrow_lr ? -20 : 20, 0);
        break;
    case GDK_KEY_Up:
        dv_viewport_pan (&self->viewport, 0,
            self->pan_prefs.invert_arrow_ud ? 20 : -20);
        break;
    case GDK_KEY_Down:
        dv_viewport_pan (&self->viewport, 0,
            self->pan_prefs.invert_arrow_ud ? -20 : 20);
        break;
    case GDK_KEY_plus:
    case GDK_KEY_equal:
    case GDK_KEY_KP_Add:
        if (ctrl_held)
            dv_viewport_zoom_at (&self->viewport, 1.25, cx, cy);
        break;
    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract:
        if (ctrl_held)
            dv_viewport_zoom_at (&self->viewport, 0.8, cx, cy);
        break;
    /* ESC handled above */
    case GDK_KEY_0:
        if (ctrl_held)
            dv_widget_reset_zoom (self);
        break;
    default:
        return FALSE;
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));
    return TRUE;
}

/* Scroll -- pan (plain) or zoom (Ctrl) */
static gboolean
on_scroll (GtkEventControllerScroll *ctrl,
           double dx, double dy,
           gpointer data)
{
    DvWidget *self   = DV_WIDGET (data);
    GdkEvent *event  = gtk_event_controller_get_current_event (
                           GTK_EVENT_CONTROLLER (ctrl));
    GdkModifierType mods = gdk_event_get_modifier_state (event);

    /* Use tracked mouse position -- gdk_event_get_position can return
     * NaN for scroll events from some input devices. */
    double mx = self->last_mx;
    double my = self->last_my;
    (void) gdk_event_get_position (event, &mx, &my);  /* update if available */
    if (mx != mx) mx = self->last_mx;  /* NaN check */
    if (my != my) my = self->last_my;

    if (mods & GDK_CONTROL_MASK) {
        double factor = (dy < 0) ? 1.1 : 0.9;
        dv_viewport_zoom_at (&self->viewport, factor, mx, my);
        g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0,
                       self->viewport.zoom);
    } else {
        double sdx = self->pan_prefs.invert_wheel_lr ? dx : -dx;
        double sdy = self->pan_prefs.invert_wheel_ud ? dy : -dy;
        dv_viewport_pan (&self->viewport, sdx * 20, sdy * 20);
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));
    return TRUE;
}

/* Drag -- mouse button down/move/up */
static void
on_drag_begin (GtkGestureDrag *drag, double x, double y, gpointer data)
{
    DvWidget *self = DV_WIDGET (data);
    GdkEvent *event = gtk_event_controller_get_current_event (
                          GTK_EVENT_CONTROLLER (drag));
    GdkModifierType mods = gdk_event_get_modifier_state (event);

    {
        /* Left drag = pan always */
        (void) mods;
        self->dragging      = TRUE;
        self->drag_start_x  = x;
        self->drag_start_y  = y;
        double z, px, py;
        dv_viewport_get_transform (&self->viewport, &z, &px, &py);
        self->drag_pan_start_x = px;
        self->drag_pan_start_y = py;
        gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
    }
}

static void
on_drag_update (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
    (void) drag;
    DvWidget *self = DV_WIDGET (data);

    if (self->boxing) {
        self->box_x2 = self->box_x1 + dx;
        self->box_y2 = self->box_y1 + dy;
        gtk_widget_queue_draw (GTK_WIDGET (self));
    } else if (self->dragging) {
        /* Direct pan: compute new pan from drag delta */
        DvViewport *vp = &self->viewport;
        if (vp->soft_mode) {
            vp->soft_mode = FALSE;
            vp->zoom      = vp->soft_zoom;
        }
        vp->pan_x = self->drag_pan_start_x + dx;
        vp->pan_y = self->drag_pan_start_y + dy;
        dv_viewport_clamp_pan (&self->viewport);
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
on_drag_end (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
    (void) drag; (void) dx; (void) dy;
    DvWidget *self  = DV_WIDGET (data);
    self->dragging  = FALSE;
}

static gint
cmp_app_name_w (gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp (
        g_app_info_get_display_name (G_APP_INFO (a)),
        g_app_info_get_display_name (G_APP_INFO (b)));
}

/* Right-click context menu */
static void
on_right_click (GtkGestureClick *click,
                int n_press, double x, double y,
                gpointer data)
{
    DvWidget *self = DV_WIDGET (data);
    (void) click; (void) n_press;
    show_context_menu (self, x, y);
}

static void
on_motion (GtkEventControllerMotion *ctrl, double x, double y, gpointer data)
{
    (void) ctrl;
    DvWidget *self   = DV_WIDGET (data);
    self->last_mx    = x;
    self->last_my    = y;
}

/* Middle-click box zoom handlers */
static void
on_box_begin (GtkGestureDrag *drag, double x, double y, gpointer data)
{
    DvWidget *self  = DV_WIDGET (data);
    self->boxing    = TRUE;
    self->box_x1    = x;
    self->box_y1    = y;
    self->box_x2    = x;
    self->box_y2    = y;
    gtk_gesture_set_state (GTK_GESTURE (drag), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_box_update (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
    (void) drag;
    DvWidget *self  = DV_WIDGET (data);
    self->box_x2    = self->box_x1 + dx;
    self->box_y2    = self->box_y1 + dy;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_box_end (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
    (void) drag;
    DvWidget *self = DV_WIDGET (data);
    if (self->boxing) {
        self->boxing = FALSE;
        dv_viewport_box_zoom (&self->viewport,
                              self->box_x1, self->box_y1,
                              self->box_x1 + dx, self->box_y1 + dy);
        g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0,
                       self->viewport.zoom);
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
setup_gesture_controllers (DvWidget *self)
{
    GtkWidget *w = GTK_WIDGET (self);

    /* Keyboard */
    GtkEventController *key = gtk_event_controller_key_new ();
    g_signal_connect (key, "key-pressed", G_CALLBACK (on_key_pressed), self);
    gtk_widget_add_controller (w, key);

    /* Scroll */
    GtkEventController *scroll = gtk_event_controller_scroll_new (
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    gtk_widget_add_controller (w, scroll);

    /* Left-drag = pan */
    GtkGesture *drag = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
    g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
    g_signal_connect (drag, "drag-end",    G_CALLBACK (on_drag_end),    self);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (drag));

    /* Middle-drag = box zoom (Bladerunner enhance) */
    GtkGesture *mdrag = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (mdrag), GDK_BUTTON_MIDDLE);
    g_signal_connect (mdrag, "drag-begin",  G_CALLBACK (on_box_begin),  self);
    g_signal_connect (mdrag, "drag-update", G_CALLBACK (on_box_update), self);
    g_signal_connect (mdrag, "drag-end",    G_CALLBACK (on_box_end),    self);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (mdrag));

    /* Motion -- track mouse position for zoom-at-cursor */
    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
    gtk_widget_add_controller (w, motion);

    /* Left click -- grab focus for keyboard input */
    GtkGesture *lclick = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (lclick), GDK_BUTTON_PRIMARY);
    g_signal_connect_swapped (lclick, "pressed",
                              G_CALLBACK (gtk_widget_grab_focus), w);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (lclick));

    /* Right-click */
    GtkGesture *rclick = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect (rclick, "pressed", G_CALLBACK (on_right_click), self);
    gtk_widget_add_controller (w, GTK_EVENT_CONTROLLER (rclick));
}

/* ── Context menu ───────────────────────────────────────────────────────── */

static void
on_menu_undo_zoom (GSimpleAction *a, GVariant *p, gpointer data)
{
    DvWidget *self = DV_WIDGET (data);
    (void) a; (void) p;
    dv_viewport_undo (&self->viewport);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_menu_reset_zoom (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    dv_widget_reset_zoom (DV_WIDGET (data));
}

/* ── Async export via GTask with queue ──────────────────────────────────── */

static void start_export (DvWidget *self, const DvExportFormat *fmt);

/* Runs on GLib thread pool thread -- no GTK calls allowed here */
static void
export_thread_func (GTask        *task,
                    gpointer      source_obj,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
    (void) cancellable;
    DvWidget   *self = DV_WIDGET (source_obj);
    ExportTask *et   = (ExportTask *) task_data;
    /* Use captured source_path -- current_path may have changed since dispatch */
    DvExportResult *result = dv_export (et->source_path, et->fmt,
                                         self->engine, &self->viewport, self->prefs);
    g_task_return_pointer (task, result, NULL);
}

/* Runs on main thread after export completes */
static void
on_export_done (GObject *source_obj, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    DvWidget *self = DV_WIDGET (source_obj);
    self->export_running = FALSE;

    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
    if (GTK_IS_WINDOW (root))
        gtk_widget_set_cursor (GTK_WIDGET (root), NULL);

    GError         *err    = NULL;
    DvExportResult *result = g_task_propagate_pointer (G_TASK (res), &err);
    if (err) g_error_free (err);
    if (result) {
        if (result->ok && result->warning_msg) {
            GtkAlertDialog *alert = gtk_alert_dialog_new ("%s", result->warning_msg);
            gtk_alert_dialog_set_modal (alert, TRUE);
            gtk_alert_dialog_show (alert,
                GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL);
            g_object_unref (alert);
        } else if (!result->ok) {
            g_warning ("Export failed: %s",
                result->error_msg ? result->error_msg : "unknown");
        }
        dv_export_result_free (result);
    }

    /* Start next queued export, or re-enable the action */
    ExportTask *next = g_queue_pop_head (self->export_queue);
    if (next) {
        start_export (self, next->fmt);
        g_free (next);
    } else {
        g_simple_action_set_enabled (self->a_export, TRUE);
    }
}

/* Launch one export -- sets busy cursor, dispatches GTask */
static void
export_task_free (ExportTask *et)
{
    g_free (et->source_path);
    g_free (et);
}

static void
start_export (DvWidget *self, const DvExportFormat *fmt)
{
    self->export_running = TRUE;

    GdkCursor *busy = gdk_cursor_new_from_name ("wait", NULL);
    GtkRoot   *root = gtk_widget_get_root (GTK_WIDGET (self));
    if (GTK_IS_WINDOW (root))
        gtk_widget_set_cursor (GTK_WIDGET (root), busy);
    g_object_unref (busy);

    ExportTask *et = g_new0 (ExportTask, 1);
    et->fmt         = fmt;
    et->source_path = g_strdup (self->current_path);   /* capture now, safe in thread */

    GTask *gtask = g_task_new (self, NULL, on_export_done, NULL);
    g_task_set_task_data (gtask, et, (GDestroyNotify) export_task_free);
    g_task_run_in_thread (gtask, export_thread_func);
    g_object_unref (gtask);
}

static void
on_menu_export (GSimpleAction *a, GVariant *p, gpointer data)
{
    DvWidget *self = DV_WIDGET (data);
    (void) a;
    int idx = (int) g_variant_get_int32 (p);
    if (idx < 0 || idx >= DV_EXPORT_NFORMATS) return;
    const DvExportFormat *fmt = &DV_EXPORT_FORMATS[idx];

    /* Close any open popover -- collect first to avoid iterator invalidation */
    {
        GSList *popovers = NULL;
        GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self));
        while (child) {
            if (GTK_IS_POPOVER (child))
                popovers = g_slist_prepend (popovers, child);
            child = gtk_widget_get_next_sibling (child);
        }
        for (GSList *l = popovers; l; l = l->next)
            gtk_popover_popdown (GTK_POPOVER (l->data));
        g_slist_free (popovers);
    }

    if (self->export_running) {
        /* Queue this export -- it will start when the current one finishes */
        ExportTask *queued = g_new0 (ExportTask, 1);
        queued->fmt         = fmt;
        queued->source_path = g_strdup (self->current_path);
        g_queue_push_tail (self->export_queue, queued);
        return;
    }

    /* Disable export action until all queued exports finish */
    g_simple_action_set_enabled (self->a_export, FALSE);
    start_export (self, fmt);
}

static void
on_menu_toggle_fullscreen (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    DvWidget  *self = DV_WIDGET (data);
    GtkWindow *win  = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
    if (!win) return;
    if (gtk_window_is_fullscreen (win))
        gtk_window_unfullscreen (win);
    else
        gtk_window_fullscreen (win);
}

static void
on_menu_quit (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    DvWidget        *self = DV_WIDGET (data);
    GtkApplication  *app  = GTK_APPLICATION (
        gtk_window_get_application (
            GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)))));
    if (app)
        g_application_quit (G_APPLICATION (app));
}

static void
on_menu_rotate_cw (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    dv_widget_rotate_cw (DV_WIDGET (data));

}

static void
on_menu_rotate_ccw (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    dv_widget_rotate_ccw (DV_WIDGET (data));

}

static void
on_menu_flip_h (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    dv_widget_flip_h (DV_WIDGET (data));

}

static void
on_menu_flip_v (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    dv_widget_flip_v (DV_WIDGET (data));

}

static void
on_menu_properties (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    DvWidget *self = DV_WIDGET (data);
    GtkRoot  *root = gtk_widget_get_root (GTK_WIDGET (self));
    dv_props_dialog_show (root ? GTK_WINDOW (root) : NULL, self->current_path);
}

static void
on_menu_reset_transform (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    dv_widget_reset_transform (DV_WIDGET (data));

}

static void
on_menu_set_wallpaper (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p;
    DvWidget *self = DV_WIDGET (data);
    if (!self->current_path) return;

    char *err = NULL;
    if (!dv_wallpaper_set_smart (self->current_path,
                                  self->engine,
                                  &self->viewport,
                                  &err)) {
        g_warning ("Set wallpaper failed: %s", err ? err : "unknown");
        g_free (err);
    }
}

static void
on_menu_send_to (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a;
    DvWidget   *self   = DV_WIDGET (data);
    const char *app_id = g_variant_get_string (p, NULL);
    if (!app_id || !self->current_path) return;

    GAppInfo *app = G_APP_INFO (g_desktop_app_info_new (app_id));
    if (!app) return;

    GFile  *f     = g_file_new_for_path (self->current_path);
    GList  *files = g_list_append (NULL, f);
    GError *err   = NULL;
    g_app_info_launch (app, files, NULL, &err);
    if (err) {
        g_warning ("Send-to launch failed: %s", err->message);
        g_error_free (err);
    }
    g_list_free (files);
    g_object_unref (f);
    g_object_unref (app);
}

static void
dv_widget_activate_help (DvWidget *self, GVariant *p, GSimpleAction *a)
{
    (void) p; (void) a;
    g_signal_emit (self, signals[SIGNAL_HELP_REQUESTED], 0);
}

static void
dv_widget_activate_prefs (DvWidget *self, GVariant *p, GSimpleAction *a)
{
    (void) p; (void) a;
    g_signal_emit (self, signals[SIGNAL_PREFS_REQUESTED], 0);
}

static void
show_context_menu (DvWidget *self, double x, double y)
{
    /* Clean up any popover children from previous right-click */
    {
        GtkWidget *ch = gtk_widget_get_first_child (GTK_WIDGET (self));
        while (ch) {
            GtkWidget *nx = gtk_widget_get_next_sibling (ch);
            if (GTK_IS_POPOVER (ch)) gtk_widget_unparent (ch);
            ch = nx;
        }
    }

    /* Undo Zoom and Reset Zoom both greyed unless the user has manually
     * zoomed away from the current view_mode baseline (1:1 or Fit) */
    g_simple_action_set_enabled (self->a_undo,
        dv_viewport_has_history (&self->viewport));
    g_simple_action_set_enabled (self->a_reset,
        dv_viewport_has_history (&self->viewport));
    /* reset-transform only when a transform is active */
    GAction *a_rxf = g_action_map_lookup_action (
        G_ACTION_MAP (self->action_group), "reset-transform");
    if (a_rxf)
        g_simple_action_set_enabled (G_SIMPLE_ACTION (a_rxf),
            dv_widget_has_transform (self));

    gboolean  has_image = (self->current_path != NULL);
    gboolean  show_all  = self->prefs && !self->prefs->ctx_dynamic;
    GtkWindow *win   = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
    gboolean   is_fs = win ? gtk_window_is_fullscreen (win) : FALSE;

    /* Flat GMenu -- no sections (sections trigger GtkScrolledWindow wrapping).
     * Separators are injected as custom GtkSeparator widgets via add_child. */
    GMenu *menu = g_menu_new ();
    int sep_id  = 0;   /* incrementing ID for custom separator widgets */

    /* Helper macro: append a named custom separator placeholder */
    #define APPEND_SEP(m, id_buf, n) \
        do { \
            g_snprintf (id_buf, sizeof (id_buf), "sep%d", n++); \
            GMenuItem *_s = g_menu_item_new (NULL, NULL); \
            g_menu_item_set_attribute (_s, "custom", "s", id_buf); \
            g_menu_append_item (m, _s); \
            g_object_unref (_s); \
        } while (0)

    char sep_name[16];

    /* Fullscreen */
    g_menu_append (menu, is_fs ? "Exit Fullscreen" : "Enter Fullscreen",
                   "widget.toggle-fullscreen");

    if (has_image || show_all) {
        APPEND_SEP (menu, sep_name, sep_id);

        g_menu_append (menu, "Undo Zoom",          "widget.undo-zoom");
        g_menu_append (menu, "Reset Zoom",         "widget.reset-zoom");
        {
            gboolean _is_actual = (self->viewport.view_mode == DV_VIEW_MODE_ACTUAL);
            g_menu_append (menu,
                _is_actual ? "Fit to Viewport" : "Actual Size (1:1)",
                "widget.zoom-actual");
        }

        GMenu *xform = g_menu_new ();
        g_menu_append (xform, "Rotate 90\xc2\xb0 Clockwise",         "widget.rotate-cw");
        g_menu_append (xform, "Rotate 90\xc2\xb0 Counter-clockwise", "widget.rotate-ccw");
        g_menu_append (xform, "Flip Horizontal",                      "widget.flip-h");
        g_menu_append (xform, "Flip Vertical",                        "widget.flip-v");
        g_menu_append (xform, "Reset Transform",                      "widget.reset-transform");
        if (has_image) {
            g_menu_append_submenu (menu, "Transform", G_MENU_MODEL (xform));
        } else {
            GMenuItem *dis = g_menu_item_new_submenu ("Transform", G_MENU_MODEL (xform));
            g_menu_item_set_attribute (dis, "action", "s", "widget.set-wallpaper");
            g_menu_append_item (menu, dis);
            g_object_unref (dis);
        }
        g_object_unref (xform);

        APPEND_SEP (menu, sep_name, sep_id);

        g_menu_append (menu, "Set as _Wallpaper", "widget.set-wallpaper");

        GMenu *conv = g_menu_new ();
        gboolean is_svg = dv_engine_is_svg (self->engine);
        for (int i = 0; i < DV_EXPORT_NFORMATS; i++) {
            if (strcmp (DV_EXPORT_FORMATS[i].ext, "svg") == 0 && !is_svg) continue;
            GMenuItem *item = g_menu_item_new (DV_EXPORT_FORMATS[i].label, NULL);
            g_menu_item_set_action_and_target (item, "widget.export", "i", (gint32) i);
            g_menu_append_item (conv, item);
            g_object_unref (item);
        }
        if (has_image) {
            g_menu_append_submenu (menu, "Convert to...", G_MENU_MODEL (conv));
        } else {
            GMenuItem *dis = g_menu_item_new_submenu ("Convert to...", G_MENU_MODEL (conv));
            g_menu_item_set_attribute (dis, "action", "s", "widget.set-wallpaper");
            g_menu_append_item (menu, dis);
            g_object_unref (dis);
        }
        g_object_unref (conv);

        GMenu *send_menu = g_menu_new ();
        GList *apps = g_app_info_get_all_for_type ("image/png");
        apps = g_list_sort (apps, cmp_app_name_w);
        for (GList *l = apps; l; l = l->next) {
            GAppInfo   *app  = G_APP_INFO (l->data);
            const char *id   = g_app_info_get_id (app);
            const char *name = g_app_info_get_display_name (app);
            if (id && strstr (id, "dwarvenview")) continue;
            if (!id || !name) continue;
            GMenuItem *item = g_menu_item_new (name, NULL);
            g_menu_item_set_action_and_target (item, "widget.send-to", "s", id);
            g_menu_append_item (send_menu, item);
            g_object_unref (item);
        }
        g_list_free_full (apps, g_object_unref);
        if (g_menu_model_get_n_items (G_MENU_MODEL (send_menu)) > 0) {
            if (has_image) {
                g_menu_append_submenu (menu, "Send to...", G_MENU_MODEL (send_menu));
            } else {
                GMenuItem *dis = g_menu_item_new_submenu ("Send to...", G_MENU_MODEL (send_menu));
                g_menu_item_set_attribute (dis, "action", "s", "widget.set-wallpaper");
                g_menu_append_item (menu, dis);
                g_object_unref (dis);
            }
        }
        g_object_unref (send_menu);

        g_menu_append (menu, "Properties...", "widget.properties");
    }

    APPEND_SEP (menu, sep_name, sep_id);
    g_menu_append (menu, "Preferences...",    "widget.prefs");
    g_menu_append (menu, "Help",              "widget.help");
    g_menu_append (menu, "Exit DwarvenView", "widget.quit");

    #undef APPEND_SEP

    GtkWidget *popup = gtk_popover_menu_new_from_model_full (
        G_MENU_MODEL (menu), GTK_POPOVER_MENU_NESTED);
    g_object_unref (menu);
    gtk_widget_set_parent (popup, GTK_WIDGET (self));
    gtk_widget_set_size_request (popup, 220, -1);

    /* Inject GtkSeparator widgets for each custom placeholder */
    for (int i = 0; i < sep_id; i++) {
        char id_buf[16];
        g_snprintf (id_buf, sizeof (id_buf), "sep%d", i);
        GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_margin_top    (sep, 4);
        gtk_widget_set_margin_bottom (sep, 4);
        gtk_popover_menu_add_child (GTK_POPOVER_MENU (popup), sep, id_buf);
    }

    GdkRectangle rect = { (int) x, (int) y, 1, 1 };
    gtk_popover_set_pointing_to (GTK_POPOVER (popup), &rect);
    gtk_popover_set_autohide (GTK_POPOVER (popup), TRUE);

    gtk_popover_popup (GTK_POPOVER (popup));
}

