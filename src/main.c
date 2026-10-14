#include <gtk/gtk.h>
#include <glib.h>
#include <string.h>
#include "config.h"
#include "dv_widget.h"
#include "dv_engine.h"
#include "dv_prefs.h"
#include "dv_dialog_keys.h"
#include "dv_props_dialog.h"
#include "dv_debug.h"
#include "dv_viewable.h"

#define DWARVEN4GTK3RESTORE_IMPLEMENTATION
#include "dwarven4gtk3restore.h"

#include <libavutil/log.h>

#define DWARVENVIEW_ICON_IMPL
#include "dv_icon.h"

/* Forward declarations */
static void update_statusbar       (void);
static void load_file              (const char *path);
static void slideshow_stop         (void);
static gboolean on_window_close     (GtkWindow *w, gpointer d);
static void slideshow_start        (void);
static void on_slideshow_toggle    (GtkButton *b, gpointer d);
static void on_slideshow_action    (GSimpleAction *a, GVariant *p, gpointer d);
static void on_fullscreen_action   (GSimpleAction *a, GVariant *p, gpointer d);
static void toggle_fullscreen      (void);
static void sync_fullscreen_chrome  (void);
static void on_fullscreen_notify   (GObject *win, GParamSpec *ps, gpointer action);
static void show_convert_menu      (GtkButton *btn, gpointer data);
static void on_browse_fallback_dir (GtkButton *btn, gpointer data);
static void on_browse_response     (GObject *source, GAsyncResult *result, gpointer data);
static void on_prefs_save          (GtkButton *btn, gpointer dlg);
static void on_prefs_cancel        (GtkButton *btn, gpointer dlg);
static void prefs_accept_key       (GtkWidget *dlg, gpointer unused);
static void prefs_cancel_key       (GtkWidget *dlg, gpointer unused);
static void on_zoom_actual_btn     (GtkButton *b, gpointer d);
static void on_splash_cb_toggled   (GtkCheckButton *cb, gpointer user_data);
static void     on_help_wrapper     (GObject *obj, gpointer data);
static void     on_help            (GSimpleAction *a, GVariant *p, gpointer d);
static void on_prefs_cancel        (GtkButton *btn, gpointer dlg);

#define APP_ID "com.dwarven.view"

typedef struct {
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *view;
    GtkWidget      *headerbar;
    GtkWidget      *menu_btn;
    GtkWidget      *statusbar;
    GtkWidget      *menubar, *toolbar;   /* hidden in fullscreen when the pref says so */

    /* Current file and directory for navigation */
    char           *current_dir;
    char           *current_file;   /* basename only */

    /* Navigation/view action refs for enable/disable */
    GSimpleAction  *a_prev;
    GSimpleAction  *a_slideshow;
    GSimpleAction  *a_zoom_in;
    GSimpleAction  *a_zoom_actual;
    GSimpleAction  *a_zoom_out;
    GSimpleAction  *a_zoom_rst;
    GSimpleAction  *a_reset_xform;
    GSimpleAction  *a_props;
    GMenu          *export_convert_m;  /* rebuilt on image load */
    GMenu          *xform_zoom_m;      /* zoom-actual label swapped live */
    GtkWidget      *tb_zoom_in;
    GtkWidget      *tb_zoom_out;
    GtkWidget      *tb_zoom_rst;
    GtkWidget      *tb_zoom_actual;
    GSimpleAction  *a_next;

    /* Toolbar widgets that need enabling on image load */
    GtkWidget      *tb_prev, *tb_next;

    /* Slideshow */
    guint           slideshow_timer;   /* 0 = stopped */
    GtkWidget      *tb_slideshow;      /* play/pause button */
    gboolean        slideshow_advancing; /* TRUE when slideshow triggers load */
    GtkWidget      *tb_rot_cw, *tb_rot_ccw, *tb_flip_h, *tb_flip_v;
    GtkWidget      *tb_xform_rst;
    GtkWidget      *tb_convert;

    /* Preferences */
    DvPrefs        *prefs;
} AppState;

static AppState state = {0};

/* ── Directory navigation (lazy, no pre-scan) ──────────────────────────── */

/*
 * Build a sorted list of all regular files in dir.
 * Returns a GPtrArray of newly-allocated strings. Caller frees with g_ptr_array_free.
 */
/* Locale-aware comparator for g_ptr_array_sort.
 * Elements are (char**) -- dereference to get the path string.
 * Compare basenames only for correct locale sort of filenames. */
static int
_locale_path_cmp (gconstpointer a, gconstpointer b)
{
    const char *pa = *(const char **) a;
    const char *pb = *(const char **) b;
    /* Compare basenames for filename-level locale sort */
    const char *na = strrchr (pa, G_DIR_SEPARATOR);
    const char *nb = strrchr (pb, G_DIR_SEPARATOR);
    na = na ? na + 1 : pa;
    nb = nb ? nb + 1 : pb;
    return strcoll (na, nb);
}

static GPtrArray *
list_dir_sorted (const char *dir)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func (g_free);
    GError *err = NULL;
    GDir   *d   = g_dir_open (dir, 0, &err);
    if (!d) { g_clear_error (&err); return arr; }

    const char *name;
    while ((name = g_dir_read_name (d))) {
        if (name[0] == '.') continue;   /* skip hidden */
        char *full = g_build_filename (dir, name, NULL);
        if (g_file_test (full, G_FILE_TEST_IS_REGULAR))
            g_ptr_array_add (arr, full);
        else
            g_free (full);
    }
    g_dir_close (d);

    /* Locale-aware sort -- respects LC_COLLATE for correct alphanumeric order.
     * g_ptr_array_sort passes pointers-to-elements, so we must dereference.
     * We compare basenames only so /a/z.png sorts before /a/aa.png correctly. */
    g_ptr_array_sort (arr, (GCompareFunc) _locale_path_cmp);
    return arr;
}

/*
 * Navigate to the next (+1) or previous (-1) viewable file relative to
 * state.current_file in state.current_dir.
 * Skips non-viewable files. Wraps around.
 */
static void
navigate (int direction)
{
    if (!state.current_dir) return;

    GPtrArray *files = list_dir_sorted (state.current_dir);
    if (!files->len) { g_ptr_array_free (files, TRUE); return; }

    /* Find current file position */
    int current_idx = -1;
    if (state.current_file) {
        char *current_full = g_build_filename (state.current_dir,
                                               state.current_file, NULL);
        for (guint i = 0; i < files->len; i++) {
            if (strcmp ((char *) files->pdata[i], current_full) == 0) {
                current_idx = (int) i;
                break;
            }
        }
        g_free (current_full);
    }
    if (current_idx < 0) current_idx = (direction > 0) ? -1 : (int) files->len;

    /* Walk in direction until we find a viewable file */
    int n = (int) files->len;
    for (int step = 1; step <= n; step++) {
        int idx = ((current_idx + direction * step) % n + n) % n;
        const char *candidate = files->pdata[idx];
        if (dv_file_is_browsable (candidate, state.prefs && state.prefs->browse_media)) {
            load_file (candidate);
            break;
        }
    }

    g_ptr_array_free (files, TRUE);
}

static void
go_prev (void) { navigate (-1); }

static void
go_next (void) { navigate (+1); }

static void
load_file (const char *path)
{
    /* If given a directory, find the first viewable file in it */
    if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
        g_free (state.current_dir);
        g_free (state.current_file);
        state.current_dir  = g_strdup (path);
        state.current_file = NULL;
        navigate (+1);
        return;
    }

    /* Load file -- preserve zoom/crop/transform during slideshow, reset on manual nav */
    if (state.slideshow_advancing)
        dv_widget_load_keep_zoom (DV_WIDGET (state.view), path);
    else
        dv_widget_load (DV_WIDGET (state.view), path);
    gtk_window_set_title (GTK_WINDOW (state.window), g_path_get_basename (path));

    g_free (state.current_dir);
    g_free (state.current_file);
    state.current_dir  = g_path_get_dirname (path);
    state.current_file = g_path_get_basename (path);

    /* Stop any running slideshow on manual navigation */
    /* (don't stop if slideshow itself triggered the load) */

    /* Enable nav/transform/convert now that an image is loaded */
    if (state.a_prev)    g_simple_action_set_enabled (state.a_prev, TRUE);
    if (state.a_next)    g_simple_action_set_enabled (state.a_next, TRUE);
    if (state.tb_prev)       gtk_widget_set_sensitive (state.tb_prev,       TRUE);
    if (state.tb_next)       gtk_widget_set_sensitive (state.tb_next,       TRUE);
    if (state.tb_slideshow)  gtk_widget_set_sensitive (state.tb_slideshow,  TRUE);
    if (state.tb_zoom_in)    gtk_widget_set_sensitive (state.tb_zoom_in,    TRUE);
    if (state.tb_zoom_out)   gtk_widget_set_sensitive (state.tb_zoom_out,   TRUE);
    if (state.a_slideshow)   g_simple_action_set_enabled (state.a_slideshow, TRUE);
    if (state.a_zoom_in)     g_simple_action_set_enabled (state.a_zoom_in,    TRUE);
    if (state.a_zoom_actual) g_simple_action_set_enabled (state.a_zoom_actual, TRUE);
    if (state.a_zoom_out)    g_simple_action_set_enabled (state.a_zoom_out,   TRUE);
    /* zoom-reset enabled only when a hard zoom is active */
    if (state.a_zoom_rst)    g_simple_action_set_enabled (state.a_zoom_rst,   FALSE);
    if (state.a_reset_xform) g_simple_action_set_enabled (state.a_reset_xform, FALSE);
    if (state.tb_xform_rst) gtk_widget_set_sensitive (state.tb_xform_rst, FALSE);   /* managed by update_statusbar */
    if (state.tb_rot_cw)    gtk_widget_set_sensitive (state.tb_rot_cw,  TRUE);
    if (state.tb_rot_ccw) gtk_widget_set_sensitive (state.tb_rot_ccw, TRUE);
    if (state.tb_flip_h)  gtk_widget_set_sensitive (state.tb_flip_h,  TRUE);
    if (state.tb_flip_v)  gtk_widget_set_sensitive (state.tb_flip_v,  TRUE);
    if (state.tb_convert) gtk_widget_set_sensitive (state.tb_convert, TRUE);
    if (state.a_props)    g_simple_action_set_enabled (state.a_props, TRUE);

    /* Cache last accessed directory */
    dv_prefs_save_last_dir (state.current_dir);

    update_statusbar ();
}

/* ── Open file dialog ───────────────────────────────────────────────────── */

static void
on_open_response (GObject *dialog, GAsyncResult *result, gpointer data)
{
    (void) data;
    GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (dialog),
                                               result, NULL);
    if (file) {
        char *path = g_file_get_path (file);
        if (path) load_file (path);
        g_free (path);
        g_object_unref (file);
    }
}

static void
action_open (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p; (void) data;

    GtkFileDialog *dlg = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dlg, "Open Image");

    /* GTK4 file filters: MIME types + suffixes.
     * Custom callback filtering was removed in GTK4.
     * Extensionless files (Xcursor) remain navigable via the viewer itself. */
    GtkFileFilter *filter_img = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_img,
        state.prefs && state.prefs->browse_media ? "Image, audio and video files" : "Image files");
    /* Raster */
    gtk_file_filter_add_mime_type (filter_img, "image/png");
    gtk_file_filter_add_mime_type (filter_img, "image/jpeg");
    gtk_file_filter_add_mime_type (filter_img, "image/gif");
    gtk_file_filter_add_mime_type (filter_img, "image/webp");
    gtk_file_filter_add_mime_type (filter_img, "image/bmp");
    gtk_file_filter_add_mime_type (filter_img, "image/tiff");
    gtk_file_filter_add_mime_type (filter_img, "image/avif");
    gtk_file_filter_add_mime_type (filter_img, "image/heif");
    gtk_file_filter_add_mime_type (filter_img, "image/heic");
    /* Vector */
    gtk_file_filter_add_mime_type (filter_img, "image/svg+xml");
    /* Xcursor -- no standard MIME, use suffix */
    gtk_file_filter_add_suffix (filter_img, "xcur");
    gtk_file_filter_add_suffix (filter_img, "cur");
    if (state.prefs && state.prefs->browse_media) {
        gtk_file_filter_add_mime_type (filter_img, "audio/*");
        gtk_file_filter_add_mime_type (filter_img, "video/*");
    }

    GtkFileFilter *filter_all = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_all, "All files");
    gtk_file_filter_add_pattern (filter_all, "*");

    /* Image files first and selected by default; All files stays reachable
     * from the dialog's own filter dropdown. (filter_img was built above but
     * never added to the list, so every Open had always shown "All files"
     * regardless of its contents -- this was a latent bug, not a choice.) */
    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, filter_img);
    g_list_store_append (filters, filter_all);
    gtk_file_dialog_set_filters (dlg, G_LIST_MODEL (filters));
    gtk_file_dialog_set_default_filter (dlg, filter_img);
    g_object_unref (filter_img);
    g_object_unref (filter_all);
    g_object_unref (filters);

    /* Restore last accessed directory */
    char *last_dir = dv_prefs_load_last_dir ();
    if (last_dir) {
        GFile *f = g_file_new_for_path (last_dir);
        gtk_file_dialog_set_initial_folder (dlg, f);
        g_object_unref (f);
        g_free (last_dir);
    }

    gtk_file_dialog_open (dlg, GTK_WINDOW (state.window), NULL,
                          on_open_response, NULL);
}


/* ── Key bindings ───────────────────────────────────────────────────────── */

static void action_open_folder (GSimpleAction *a, GVariant *p, gpointer data);

/* Properties (EXIF and file details) for the image on screen. */
static void
action_properties (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p; (void) data;
    const char *path = state.view ? dv_widget_get_path (DV_WIDGET (state.view)) : NULL;
    if (path)
        dv_props_dialog_show (GTK_WINDOW (state.window), path);
}

static gboolean
on_window_key (GtkEventControllerKey *ctrl,
               guint keyval, guint keycode,
               GdkModifierType state_mask,
               gpointer data)
{
    (void) ctrl; (void) keycode; (void) data;
    gboolean ctrl_held = (state_mask & GDK_CONTROL_MASK) != 0;

    /* Ctrl+O opens a file, Ctrl+Shift+O a folder. Lowercase the keyval:
     * Shift (or Caps Lock) turns GDK_KEY_o into GDK_KEY_O. */
    if (ctrl_held && gdk_keyval_to_lower (keyval) == GDK_KEY_o) {
        if (state_mask & GDK_SHIFT_MASK) action_open_folder (NULL, NULL, NULL);
        else                             action_open (NULL, NULL, NULL);
        return TRUE;
    }
    if (ctrl_held && gdk_keyval_to_lower (keyval) == GDK_KEY_i) {
        action_properties (NULL, NULL, NULL);
        return TRUE;
    }
    /* Page Up/Down browse. So do Ctrl+Left/Right: the arrows pan, and Ctrl+arrow reads as "go
     * back/forward" (undocumented on purpose). */
    gboolean ctrl_only = ctrl_held && !(state_mask & (GDK_SHIFT_MASK | GDK_ALT_MASK));
    if (keyval == GDK_KEY_Page_Up   || (ctrl_only && keyval == GDK_KEY_Left))  { go_prev (); return TRUE; }
    if (keyval == GDK_KEY_Page_Down || (ctrl_only && keyval == GDK_KEY_Right)) { go_next (); return TRUE; }
    if (keyval == GDK_KEY_space)     { on_slideshow_toggle (NULL, NULL); return TRUE; }
    if (keyval == GDK_KEY_1 && (state_mask & GDK_CONTROL_MASK)) {
        if (state.view) { dv_widget_zoom_actual (DV_WIDGET (state.view)); update_statusbar (); }
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        toggle_fullscreen ();
        return TRUE;
    }
    /* Escape also leaves fullscreen (the image view keeps it first for undoing a zoom) */
    if (keyval == GDK_KEY_Escape && gtk_window_is_fullscreen (GTK_WINDOW (state.window))) {
        gtk_window_unfullscreen (GTK_WINDOW (state.window));
        return TRUE;
    }
    return FALSE;
}

/* Widgets of the Preferences dialog that Save reads back. */
typedef struct { GtkWidget *dlg, *cb_lr, *cb_ud, *cb_wud, *cb_wlr, *entry, *ctx_cb, *view_actual_rb, *splash_cb, *delay_scale, *media_cb, *fs_cb; } DlgW;

/* ── Build UI ───────────────────────────────────────────────────────────── */


/* ── Status bar ─────────────────────────────────────────────────────────── */

static void
update_statusbar (void)
{
    if (!state.statusbar || !state.view) return;

    DvWidget *dv = DV_WIDGET (state.view);

    int native_w = 0, native_h = 0;
    double zoom = 1.0;
    gboolean is_hard = FALSE, has_crop = FALSE;
    dv_widget_get_status (dv, &native_w, &native_h, &zoom, &is_hard, &has_crop);

    if (native_w == 0 || native_h == 0) {
        gtk_label_set_text (GTK_LABEL (state.statusbar), "No image");
        return;
    }

    /* Show loading message while placeholder is displayed */
    if (dv_engine_is_placeholder (dv_widget_get_engine (dv))) {
        gtk_label_set_text (GTK_LABEL (state.statusbar),
            "â³  Loading large imageâ¦");
        return;
    }

    int disp_w = (int)(native_w * zoom + 0.5);
    int disp_h = (int)(native_h * zoom + 0.5);
    int pct    = (int)(zoom * 100.0 + 0.5);

    const char *mode;
    if (!is_hard)
        mode = "Auto-fit";
    else if (has_crop)
        mode = "Crop-zoom";
    else
        mode = "Manual";

    /* ── Centralised sensitivity sync (Mantra #1) ─────────────────────── */
    gboolean has_image = (native_w > 0 && native_h > 0);
    gboolean has_xform = has_image && state.view &&
                         dv_widget_has_transform (DV_WIDGET (state.view));
    gboolean has_zoom_hist = has_image && state.view &&
                         dv_widget_has_zoom_history (DV_WIDGET (state.view));

    /* Zoom reset -- only active if manually zoomed away from the
     * current view_mode baseline (1:1 or Fit), not just because
     * we're in a "hard" zoom mode */
    if (state.a_zoom_rst)          g_simple_action_set_enabled (state.a_zoom_rst, has_zoom_hist);
    if (state.tb_zoom_rst)         gtk_widget_set_sensitive     (state.tb_zoom_rst, has_zoom_hist);

    /* Zoom in/out toolbar (stay active while image loaded) */
    if (state.tb_zoom_in)          gtk_widget_set_sensitive (state.tb_zoom_in,  has_image);
    if (state.tb_zoom_out)         gtk_widget_set_sensitive (state.tb_zoom_out, has_image);

    /* Actual Size (1:1) toggle -- authoritative state is DvWidget's
     * view_mode. The toolbar reflects it via pressed/unpressed toggle
     * state; the title menu relabels the item text (matching the
     * right-click context menu's wording) instead of showing a GMenu
     * checkbox, which is what a stateful boolean action would render. */
    if (state.view) {
        gboolean is_actual = dv_widget_is_actual_size (DV_WIDGET (state.view));

        if (state.xform_zoom_m) {
            GMenuItem *item = g_menu_item_new (
                is_actual ? "_Fit to Viewport" : "_Actual Size (1:1)",
                "dv.zoom-actual");
            g_menu_remove (state.xform_zoom_m, 3);
            g_menu_insert_item (state.xform_zoom_m, 3, item);
            g_object_unref (item);
        }

        if (state.tb_zoom_actual) {
            gtk_widget_set_sensitive (state.tb_zoom_actual, has_image);
            gtk_widget_set_tooltip_text (state.tb_zoom_actual,
                is_actual ? "Fit to Viewport (Ctrl+1)"
                          : "Actual Size 1:1 (Ctrl+1)");
            g_signal_handlers_block_by_func (state.tb_zoom_actual,
                G_CALLBACK (on_zoom_actual_btn), NULL);
            gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (state.tb_zoom_actual), is_actual);
            g_signal_handlers_unblock_by_func (state.tb_zoom_actual,
                G_CALLBACK (on_zoom_actual_btn), NULL);
        }
    }

    /* Reset transform -- action and widget action group */
    if (state.a_reset_xform)       g_simple_action_set_enabled (state.a_reset_xform, has_xform);
    if (state.tb_xform_rst)        gtk_widget_set_sensitive     (state.tb_xform_rst,    has_xform);
    if (state.view) {
        GActionGroup *wag = dv_widget_get_action_group (DV_WIDGET (state.view));
        GAction *wa = g_action_map_lookup_action (G_ACTION_MAP (wag), "reset-transform");
        if (wa) g_simple_action_set_enabled (G_SIMPLE_ACTION (wa), has_xform);
        /* Reset zoom in widget group */
        GAction *wrz = g_action_map_lookup_action (G_ACTION_MAP (wag), "reset-zoom");
        if (wrz) g_simple_action_set_enabled (G_SIMPLE_ACTION (wrz), has_zoom_hist);
    }

    char *text = g_strdup_printf (
        "%d\xc3\x97%d  \xe2\x86\x92  %d\xc3\x97%d  [%d%%  %s]",
        native_w, native_h, disp_w, disp_h, pct, mode);
    gtk_label_set_text (GTK_LABEL (state.statusbar), text);
    g_free (text);
}

static void
on_zoom_changed (GtkWidget *widget, double zoom, gpointer data)
{
    (void) widget; (void) zoom; (void) data;
    update_statusbar ();
}

static gboolean
update_statusbar_idle (gpointer data)
{
    (void) data;
    update_statusbar ();
    return G_SOURCE_REMOVE;
}

static void
rebuild_export_convert_menu (void)
{
    if (!state.export_convert_m) return;

    /* Clear existing items */
    while (g_menu_model_get_n_items (G_MENU_MODEL (state.export_convert_m)) > 0)
        g_menu_remove (state.export_convert_m, 0);

    gboolean is_svg = state.view &&
        dv_engine_is_svg (dv_widget_get_engine (DV_WIDGET (state.view)));

    /* SVG first if source is SVG */
    if (is_svg) {
        for (int i = 0; i < DV_EXPORT_NFORMATS; i++) {
            if (strcmp (DV_EXPORT_FORMATS[i].ext, "svg") != 0) continue;
            GMenuItem *item = g_menu_item_new (DV_EXPORT_FORMATS[i].label, NULL);
            g_menu_item_set_action_and_target (item, "widget.export", "i", (gint32) i);
            g_menu_append_item (state.export_convert_m, item);
            g_object_unref (item);
        }
    }

    /* All non-SVG formats */
    for (int i = 0; i < DV_EXPORT_NFORMATS; i++) {
        if (strcmp (DV_EXPORT_FORMATS[i].ext, "svg") == 0) continue;
        GMenuItem *item = g_menu_item_new (DV_EXPORT_FORMATS[i].label, NULL);
        g_menu_item_set_action_and_target (item, "widget.export", "i", (gint32) i);
        g_menu_append_item (state.export_convert_m, item);
        g_object_unref (item);
    }
}

static void
on_load_complete (GtkWidget *widget, gboolean success, gpointer data)
{
    (void) widget; (void) success; (void) data;
    /* Defer slightly so engine state is fully committed */
    g_idle_add (update_statusbar_idle, NULL);
    rebuild_export_convert_menu ();
}


/* ── Preferences dialog ─────────────────────────────────────────────────── */

static void
on_browse_response (GObject *source, GAsyncResult *result, gpointer data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    GtkWidget     *entry  = GTK_WIDGET (data);
    GError        *err    = NULL;

    GFile *f = gtk_file_dialog_select_folder_finish (dialog, result, &err);
    if (f) {
        char *path = g_file_get_path (f);
        if (path) {
            gtk_editable_set_text (GTK_EDITABLE (entry), path);
            g_free (path);
        }
        g_object_unref (f);
    }
    g_clear_error (&err);
    g_object_unref (dialog);
}

static void
on_browse_fallback_dir (GtkButton *btn, gpointer data)
{
    (void) data;
    GtkWidget *entry = GTK_WIDGET (g_object_get_data (G_OBJECT (btn), "entry"));

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, "Select Fallback Save Directory");
    gtk_file_dialog_set_modal (dialog, TRUE);

    /* Pre-populate with current entry text if valid */
    const char *cur = gtk_editable_get_text (GTK_EDITABLE (entry));
    if (cur && *cur && g_file_test (cur, G_FILE_TEST_IS_DIR)) {
        GFile *f = g_file_new_for_path (cur);
        gtk_file_dialog_set_initial_folder (dialog, f);
        g_object_unref (f);
    }

    gtk_file_dialog_select_folder (dialog,
        GTK_WINDOW (state.window),
        NULL,
        on_browse_response,
        entry);
    /* dialog ownership transferred to callback via g_object_unref there */
}

/* A tab of the preferences dialog: a page box with the dialog's usual margins. */
static GtkWidget *
add_pref_page (GtkWidget *notebook, const char *title)
{
    GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top    (page, 12);
    gtk_widget_set_margin_bottom (page, 12);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_notebook_append_page (GTK_NOTEBOOK (notebook), page, gtk_label_new (title));
    return page;
}

static void
show_prefs_dialog (void)
{
    GtkWidget *dlg = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (dlg), "DwarvenView Preferences");
    gtk_window_set_transient_for (GTK_WINDOW (dlg), GTK_WINDOW (state.window));
    gtk_window_set_modal (GTK_WINDOW (dlg), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (dlg), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (dlg), 440, -1);

    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top    (outer, 16);
    gtk_widget_set_margin_bottom (outer, 16);
    gtk_widget_set_margin_start  (outer, 16);
    gtk_widget_set_margin_end    (outer, 16);

    /* Tabs, so the dialog fits a small screen without scrolling */
    GtkWidget *notebook      = gtk_notebook_new ();
    GtkWidget *page_viewing  = add_pref_page (notebook, "Viewing");
    GtkWidget *page_browsing = add_pref_page (notebook, "Browsing");
    GtkWidget *page_controls = add_pref_page (notebook, "Controls");
    GtkWidget *page_saving   = add_pref_page (notebook, "Saving");
    gtk_box_append (GTK_BOX (outer), notebook);

    /* Pan inversion section */
    GtkWidget *pan_frame = gtk_frame_new ("Pan Inversion");
    GtkWidget *pan_box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top    (pan_box, 8);
    gtk_widget_set_margin_bottom (pan_box, 8);
    gtk_widget_set_margin_start  (pan_box, 8);
    gtk_widget_set_margin_end    (pan_box, 8);

    GtkWidget *cb_lr  = gtk_check_button_new_with_label ("Invert Left/Right arrow keys");
    GtkWidget *cb_ud  = gtk_check_button_new_with_label ("Invert Up/Down arrow keys");
    GtkWidget *cb_wud = gtk_check_button_new_with_label ("Invert scroll wheel vertical");
    GtkWidget *cb_wlr = gtk_check_button_new_with_label (
        "Invert scroll wheel horizontal (dual-wheel mice)");

    gtk_check_button_set_active (GTK_CHECK_BUTTON (cb_lr),
        state.prefs->pan.invert_arrow_lr);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (cb_ud),
        state.prefs->pan.invert_arrow_ud);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (cb_wud),
        state.prefs->pan.invert_wheel_ud);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (cb_wlr),
        state.prefs->pan.invert_wheel_lr);

    gtk_box_append (GTK_BOX (pan_box), cb_lr);
    gtk_box_append (GTK_BOX (pan_box), cb_ud);
    gtk_box_append (GTK_BOX (pan_box), cb_wud);
    gtk_box_append (GTK_BOX (pan_box), cb_wlr);
    gtk_frame_set_child (GTK_FRAME (pan_frame), pan_box);
    gtk_box_append (GTK_BOX (page_controls), pan_frame);

    /* Fallback save dir section */
    GtkWidget *save_frame = gtk_frame_new ("Conversion Fallback Save Directory");
    GtkWidget *save_box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top    (save_box, 8);
    gtk_widget_set_margin_bottom (save_box, 8);
    gtk_widget_set_margin_start  (save_box, 8);
    gtk_widget_set_margin_end    (save_box, 8);

    /* Top label: says what this section is for */
    GtkWidget *save_top_lbl = gtk_label_new (
        "Used when the source file cannot be saved alongside the original.");
    gtk_label_set_wrap (GTK_LABEL (save_top_lbl), TRUE);
    gtk_widget_set_halign (save_top_lbl, GTK_ALIGN_START);

    /* Entry + Browse button row */
    GtkWidget *entry_row  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *save_entry = gtk_entry_new ();
    GtkWidget *browse_btn = gtk_button_new_with_mnemonic ("_Browse...");
    gtk_widget_set_hexpand (save_entry, TRUE);
    gtk_entry_set_placeholder_text (GTK_ENTRY (save_entry),
        "e.g. /home/user/Conversions");
    if (state.prefs->fallback_save_dir)
        gtk_editable_set_text (GTK_EDITABLE (save_entry),
            state.prefs->fallback_save_dir);
    g_object_set_data (G_OBJECT (browse_btn), "entry", save_entry);
    g_signal_connect (browse_btn, "clicked",
        G_CALLBACK (on_browse_fallback_dir), NULL);
    gtk_box_append (GTK_BOX (entry_row), save_entry);
    gtk_box_append (GTK_BOX (entry_row), browse_btn);

    /* Fallback note below the entry */
    GtkWidget *save_lbl = gtk_label_new (
        "Falls back to ~/Pictures, then $HOME, if this path is also unsuitable.");
    gtk_label_set_wrap (GTK_LABEL (save_lbl), TRUE);
    gtk_widget_set_halign (save_lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class (save_lbl, "dim-label");

    gtk_box_append (GTK_BOX (save_box), save_top_lbl);
    gtk_box_append (GTK_BOX (save_box), entry_row);
    gtk_box_append (GTK_BOX (save_box), save_lbl);
    gtk_frame_set_child (GTK_FRAME (save_frame), save_box);
    gtk_box_append (GTK_BOX (page_saving), save_frame);

    /* Slideshow interval section */
    GtkWidget *ss_frame = gtk_frame_new ("Slideshow Interval");
    GtkWidget *ss_box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top    (ss_box, 8);
    gtk_widget_set_margin_bottom (ss_box, 8);
    gtk_widget_set_margin_start  (ss_box, 8);
    gtk_widget_set_margin_end    (ss_box, 8);

    GtkWidget *ss_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (ss_entry), "e.g. 3  or  0.5  or  1/30");
    if (state.prefs->slideshow_interval)
        gtk_editable_set_text (GTK_EDITABLE (ss_entry),
            state.prefs->slideshow_interval);
    else
        gtk_editable_set_text (GTK_EDITABLE (ss_entry), "3");

    GtkWidget *ss_lbl = gtk_label_new (
        "Seconds per image. Use fractions for fast slideshow:\n"
        "1/30 = 30fps,  1/60 = 60fps,  0.5 = 2fps,  100 = 100 sec/image");
    gtk_label_set_wrap (GTK_LABEL (ss_lbl), TRUE);
    gtk_widget_set_halign (ss_lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class (ss_lbl, "dim-label");

    gtk_box_append (GTK_BOX (ss_box), ss_entry);
    gtk_box_append (GTK_BOX (ss_box), ss_lbl);
    gtk_frame_set_child (GTK_FRAME (ss_frame), ss_box);
    gtk_box_append (GTK_BOX (page_browsing), ss_frame);

    /* Right-click menu style */
    GtkWidget *ctx_frame  = gtk_frame_new ("Right-click Menu");
    GtkWidget *ctx_cb     = gtk_check_button_new_with_label (
        "Hide unavailable items (dynamic)");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (ctx_cb),
        state.prefs->ctx_dynamic);
    gtk_frame_set_child (GTK_FRAME (ctx_frame), ctx_cb);
    gtk_box_append (GTK_BOX (page_controls), ctx_frame);

    /* Default view mode -- applied to every freshly loaded image */
    GtkWidget *view_frame     = gtk_frame_new ("Default View Mode");
    GtkWidget *view_box       = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *view_fit_rb    = gtk_check_button_new_with_label ("Fit to Viewport");
    GtkWidget *view_actual_rb = gtk_check_button_new_with_label ("Actual Size (1:1)");
    gtk_check_button_set_group (GTK_CHECK_BUTTON (view_actual_rb),
                                GTK_CHECK_BUTTON (view_fit_rb));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (
        state.prefs->default_view_mode == DV_VIEW_MODE_ACTUAL
            ? view_actual_rb : view_fit_rb), TRUE);
    gtk_box_append (GTK_BOX (view_box), view_fit_rb);
    gtk_box_append (GTK_BOX (view_box), view_actual_rb);
    gtk_frame_set_child (GTK_FRAME (view_frame), view_box);
    gtk_box_append (GTK_BOX (page_viewing), view_frame);

    /* Loading splash screen */
    GtkWidget *splash_frame = gtk_frame_new ("Loading");
    GtkWidget *splash_box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *splash_cb    = gtk_check_button_new_with_label (
        "Show loading splash screen");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (splash_cb),
        state.prefs->show_loading_splash);

    GtkWidget *delay_row   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *delay_label = gtk_label_new ("Delay before showing (from 0 to 1 second):");
    GtkWidget *delay_scale = gtk_scale_new_with_range (
        GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_range_set_value (GTK_RANGE (delay_scale),
        state.prefs->loading_splash_delay);
    gtk_scale_set_draw_value (GTK_SCALE (delay_scale), TRUE);
    gtk_scale_set_value_pos (GTK_SCALE (delay_scale), GTK_POS_RIGHT);
    gtk_scale_set_digits (GTK_SCALE (delay_scale), 2);
    gtk_widget_set_hexpand (delay_scale, TRUE);
    /* Only meaningful when the splash is shown at all -- greyed otherwise,
     * and kept live in sync as the checkbox is toggled within the dialog. */
    gtk_widget_set_sensitive (delay_scale, state.prefs->show_loading_splash);
    g_signal_connect (splash_cb, "toggled",
        G_CALLBACK (on_splash_cb_toggled), delay_scale);
    gtk_box_append (GTK_BOX (delay_row), delay_label);
    gtk_box_append (GTK_BOX (delay_row), delay_scale);

    gtk_box_append (GTK_BOX (splash_box), splash_cb);
    gtk_box_append (GTK_BOX (splash_box), delay_row);
    gtk_frame_set_child (GTK_FRAME (splash_frame), splash_box);
    gtk_box_append (GTK_BOX (page_viewing), splash_frame);

    /* Fullscreen */
    GtkWidget *fs_frame = gtk_frame_new ("Fullscreen");
    GtkWidget *fs_cb    = gtk_check_button_new_with_label (
        "Image only: hide the menu, toolbar and status bar");
    gtk_widget_set_tooltip_text (fs_cb,
        "F11 or Escape returns to the window. The keyboard shortcuts keep working.");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (fs_cb), state.prefs->fullscreen_hide_ui);
    gtk_frame_set_child (GTK_FRAME (fs_frame), fs_cb);
    gtk_box_append (GTK_BOX (page_viewing), fs_frame);

    /* Which files Open and next/previous take */
    GtkWidget *media_frame = gtk_frame_new ("Browsing");
    GtkWidget *media_cb    = gtk_check_button_new_with_label (
        "Also browse audio and video files");
    gtk_widget_set_tooltip_text (media_cb,
        "Off: Open and next/previous take images only.\n"
        "On: they also take audio (cover art or a music icon) and video (one frame).\n"
        "A file opened directly is always shown.");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (media_cb), state.prefs->browse_media);
    gtk_frame_set_child (GTK_FRAME (media_frame), media_cb);
    gtk_box_prepend (GTK_BOX (page_browsing), media_frame);

    /* Buttons */
    GtkWidget *btn_row   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (btn_row, GTK_ALIGN_END);
    GtkWidget *btn_cancel = gtk_button_new_with_mnemonic ("_Cancel");
    GtkWidget *btn_save   = gtk_button_new_with_mnemonic ("_Save");
    gtk_widget_add_css_class (btn_save, "suggested-action");
    gtk_box_append (GTK_BOX (btn_row), btn_cancel);
    gtk_box_append (GTK_BOX (btn_row), btn_save);
    gtk_box_append (GTK_BOX (outer), btn_row);

    gtk_window_set_child (GTK_WINDOW (dlg), outer);

    /* Wire save button */
    DlgW *dw   = g_new0 (DlgW, 1);
    dw->dlg    = dlg;
    dw->cb_lr  = cb_lr;
    dw->cb_ud  = cb_ud;
    dw->cb_wud = cb_wud;
    dw->cb_wlr = cb_wlr;
    dw->entry  = save_entry;
    dw->ctx_cb = ctx_cb;
    dw->view_actual_rb = view_actual_rb;
    dw->splash_cb = splash_cb;
    dw->delay_scale = delay_scale;
    dw->media_cb = media_cb;
    dw->fs_cb    = fs_cb;
    g_object_set_data_full (G_OBJECT (dlg), "dw", dw, g_free);
    g_object_set_data (G_OBJECT (dlg), "ss_entry", ss_entry);

    g_signal_connect (btn_save,   "clicked", G_CALLBACK (on_prefs_save),   dlg);
    g_signal_connect (btn_cancel, "clicked", G_CALLBACK (on_prefs_cancel), dlg);

    /* Keyboard: Enter=save, Escape=cancel */
    dv_dialog_keys_attach (dlg, prefs_accept_key, prefs_cancel_key, NULL);

    gtk_window_present (GTK_WINDOW (dlg));
}

static void
prefs_accept_key (GtkWidget *dlg, gpointer unused)
{
    (void) unused;
    on_prefs_save (NULL, dlg);
}

static void
prefs_cancel_key (GtkWidget *dlg, gpointer unused)
{
    (void) unused;
    on_prefs_cancel (NULL, dlg);
}

static void
on_prefs_save (GtkButton *btn, gpointer dlg)
{
    (void) btn;
    DlgW *dw = g_object_get_data (G_OBJECT (dlg), "dw");

    state.prefs->pan.invert_arrow_lr =
        gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->cb_lr));
    state.prefs->pan.invert_arrow_ud =
        gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->cb_ud));
    state.prefs->pan.invert_wheel_ud =
        gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->cb_wud));
    state.prefs->pan.invert_wheel_lr =
        gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->cb_wlr));

    const char *fbd = gtk_editable_get_text (GTK_EDITABLE (dw->entry));
    g_free (state.prefs->fallback_save_dir);
    state.prefs->fallback_save_dir = (fbd && *fbd) ? g_strdup (fbd) : NULL;

    /* Slideshow interval -- find via object data on dialog */
    GtkWidget *ss_e = GTK_WIDGET (g_object_get_data (G_OBJECT (dlg), "ss_entry"));
    if (ss_e) {
        const char *ssi = gtk_editable_get_text (GTK_EDITABLE (ss_e));
        g_free (state.prefs->slideshow_interval);
        state.prefs->slideshow_interval = (ssi && *ssi) ? g_strdup (ssi) : g_strdup ("3");
    }

    if (dw->ctx_cb)
        state.prefs->ctx_dynamic =
            gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->ctx_cb));

    if (dw->view_actual_rb)
        state.prefs->default_view_mode =
            gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->view_actual_rb))
                ? DV_VIEW_MODE_ACTUAL : DV_VIEW_MODE_FIT;

    if (dw->splash_cb)
        state.prefs->show_loading_splash =
            gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->splash_cb));

    if (dw->delay_scale)
        state.prefs->loading_splash_delay =
            gtk_range_get_value (GTK_RANGE (dw->delay_scale));

    if (dw->fs_cb)
        state.prefs->fullscreen_hide_ui =
            gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->fs_cb));

    if (dw->media_cb)
        state.prefs->browse_media =
            gtk_check_button_get_active (GTK_CHECK_BUTTON (dw->media_cb));

    dv_prefs_save (state.prefs);
    sync_fullscreen_chrome ();
    dv_widget_set_pan_prefs (DV_WIDGET (state.view), &state.prefs->pan);
    gtk_window_destroy (GTK_WINDOW (dlg));
}

static void
on_prefs_cancel (GtkButton *btn, gpointer dlg)
{
    (void) btn;
    gtk_window_destroy (GTK_WINDOW (dlg));
}



static void
on_open_folder_response (GObject *source, GAsyncResult *result, gpointer data)
{
    (void) data;
    GtkFileDialog *dlg = GTK_FILE_DIALOG (source);
    GError        *err = NULL;
    GFile         *f   = gtk_file_dialog_select_folder_finish (dlg, result, &err);
    if (f) {
        char *path = g_file_get_path (f);
        if (path) { load_file (path); g_free (path); }
        g_object_unref (f);
    }
    g_clear_error (&err);
}

static void
action_open_folder (GSimpleAction *a, GVariant *p, gpointer data)
{
    (void) a; (void) p; (void) data;
    GtkFileDialog *dlg = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dlg, "Open Folder");
    gtk_file_dialog_set_modal (dlg, TRUE);

    char *last_dir = dv_prefs_load_last_dir ();
    if (last_dir) {
        GFile *f = g_file_new_for_path (last_dir);
        gtk_file_dialog_set_initial_folder (dlg, f);
        g_object_unref (f);
        g_free (last_dir);
    }

    gtk_file_dialog_select_folder (dlg, GTK_WINDOW (state.window), NULL,
                                   on_open_folder_response, NULL);
}

static void
on_zoom_in (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (!state.view) return;
    dv_widget_zoom_centre (DV_WIDGET (state.view), 1.25);
}

static void
on_zoom_out (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (!state.view) return;
    dv_widget_zoom_centre (DV_WIDGET (state.view), 0.8);
}

static void on_zoom_in_btn    (GtkButton *b, gpointer d);
static void on_zoom_out_btn   (GtkButton *b, gpointer d);
static void on_zoom_reset_btn (GtkButton *b, gpointer d);
static void on_zoom_reset      (GSimpleAction *a, GVariant *p, gpointer d);
static void on_reset_transform (GtkButton *b, gpointer d)
{
    (void) b; (void) d;
    if (!state.view) return;
    dv_widget_reset_transform (DV_WIDGET (state.view));
    update_statusbar ();
}


/* Toggle between 1:1 and fit-to-window.
 * Delegates to the widget's own "zoom-actual" action -- the single,
 * proven-correct implementation (also used by the right-click menu).
 * No logic is duplicated here (Mantra #1). */
static void
on_zoom_actual (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (!state.view) return;
    GActionGroup *wag = dv_widget_get_action_group (DV_WIDGET (state.view));
    g_action_group_activate_action (wag, "zoom-actual", NULL);
    update_statusbar ();
}

static void
on_zoom_reset (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    if (!state.view) return;
    dv_widget_reset_zoom (DV_WIDGET (state.view));
    update_statusbar ();
}

static __attribute__((used)) void on_zoom_actual_btn (GtkButton *b, gpointer d) {
    (void)b;(void)d;
    if (state.a_zoom_actual)
        g_action_activate (G_ACTION (state.a_zoom_actual), NULL);
}
static void
on_splash_cb_toggled (GtkCheckButton *cb, gpointer user_data)
{
    GtkWidget *delay_scale = user_data;
    gtk_widget_set_sensitive (delay_scale, gtk_check_button_get_active (cb));
}
static __attribute__((used)) void on_zoom_in_btn    (GtkButton *b, gpointer d) { (void)b;(void)d; if (state.view) { dv_widget_zoom_centre (DV_WIDGET (state.view), 1.25); update_statusbar (); } }
static __attribute__((used)) void on_zoom_out_btn   (GtkButton *b, gpointer d) { (void)b;(void)d; if (state.view) { dv_widget_zoom_centre (DV_WIDGET (state.view), 0.80); update_statusbar (); } }
static __attribute__((used)) void on_zoom_reset_btn (GtkButton *b, gpointer d) { (void)b;(void)d; if (state.view) { dv_widget_reset_zoom  (DV_WIDGET (state.view));        update_statusbar (); } }

static void
on_help_wrapper (GObject *obj, gpointer data)
{
    (void) obj; (void) data;
    on_help (NULL, NULL, NULL);
}

static void
on_help (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;

    static GtkWidget *help_win = NULL;

    /* Only one help window at a time */
    if (help_win) {
        gtk_window_present (GTK_WINDOW (help_win));
        return;
    }

    help_win = gtk_window_new ();
    gtk_window_set_title       (GTK_WINDOW (help_win), "DwarvenView Help");
    gtk_window_set_default_size (GTK_WINDOW (help_win), 560, 520);
    gtk_window_set_transient_for (GTK_WINDOW (help_win),
                                  GTK_WINDOW (state.window));
    gtk_window_set_modal (GTK_WINDOW (help_win), FALSE);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (help_win), TRUE);

    /* Clear static on close */
    g_signal_connect_swapped (help_win, "destroy",
                              G_CALLBACK (g_nullify_pointer), &help_win);

    dv_dialog_keys_attach (help_win, NULL, NULL, NULL);

    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtk_widget_set_margin_top    (scroll, 12);
    gtk_widget_set_margin_bottom (scroll, 12);
    gtk_widget_set_margin_start  (scroll, 16);
    gtk_widget_set_margin_end    (scroll, 16);

    GtkWidget *label = gtk_label_new (
        "\n"
        "DwarvenView " PACKAGE_VERSION " -- Quick Reference\n"
        "\n"
        "NAVIGATION\n"
        "  Page Up / Page Down      Previous / Next image\n"
        "  Ctrl+O                   Open file\n"
        "  Ctrl+Shift+O             Open folder\n"
        "  Ctrl+I                   Properties (EXIF and file details)\n"
        "  Drag and drop            Open image or folder\n"
        "\n"
        "ZOOM\n"
        "  Ctrl+Scroll              Zoom in/out (mouse-centred)\n"
        "  Ctrl++ / Ctrl+-          Zoom in / out\n"
        "  Ctrl+0                   Reset zoom to fit\n"
        "  Ctrl+1                   Actual size (100%)\n"
        "  Middle-button drag       Blade Runner style crop-and-zoom\n"
        "                           (drag a box to zoom into that region)\n"
        "\n"
        "PAN\n"
        "  Arrow keys               Pan the image\n"
        "  Left-button drag         Pan the image\n"
        "  Right-click              Context menu\n"
        "\n"
        "TRANSFORM\n"
        "  R                        Rotate 90 degrees clockwise\n"
        "  Shift+R                  Rotate 90 degrees counter-clockwise\n"
        "  L                        Rotate 90 degrees counter-clockwise\n"
        "  Shift+L                  Rotate 90 degrees clockwise\n"
        "  H                        Flip horizontal\n"
        "  V                        Flip vertical\n"
        "\n"
        "SLIDESHOW\n"
        "  Space                    Toggle slideshow on/off\n"
        "\n"
        "EXPORT\n"
        "  Right-click > Convert to Export current view as PNG/JPEG/WEBP/etc.\n"
        "  Right-click > Send to    Open in another application\n"
        "  Right-click > Wallpaper  Set as desktop wallpaper\n"
        "\n"
        "  Exports capture exactly what is visible in the viewport.\n"
        "  Scaling uses spline36 + unsharp for maximum quality.\n"
        "\n"
        "FULLSCREEN\n"
        "  F11 or Escape            Toggle / leave fullscreen\n"
        "\n"
        "PREFERENCES\n"
        "  File > Preferences       Viewing (loading splash, fullscreen image only),\n"
        "                           browsing (slideshow interval, audio and video),\n"
        "                           controls, saving\n"
        "\n"
        "WINDOW\n"
        "  Resize the window        Image recentres and rescales to fit\n"
        "\n"
        "TROUBLESHOOTING\n"
        "  If DwarvenView crashes or fails on an image: Help > About DwarvenView,\n"
        "  then tick \"Log image decode calls\". Takes effect immediately.\n"
        "\n"
    );
    gtk_label_set_selectable (GTK_LABEL (label), FALSE);
    gtk_label_set_xalign     (GTK_LABEL (label), 0.0f);
    gtk_widget_add_css_class (label, "monospace");

    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), label);

    /* Close button */
    GtkWidget *btn_close = gtk_button_new_with_mnemonic ("_Close");
    gtk_widget_add_css_class (btn_close, "suggested-action");
    gtk_widget_set_halign (btn_close, GTK_ALIGN_END);
    gtk_widget_set_margin_top    (btn_close, 0);
    gtk_widget_set_margin_bottom (btn_close, 12);
    gtk_widget_set_margin_end    (btn_close, 16);
    g_signal_connect_swapped (btn_close, "clicked",
                              G_CALLBACK (gtk_window_destroy), help_win);

    gtk_box_append (GTK_BOX (outer), scroll);
    gtk_box_append (GTK_BOX (outer), btn_close);
    gtk_window_set_child (GTK_WINDOW (help_win), outer);

    gtk_window_present (GTK_WINDOW (help_win));
}


static void
on_debug_log_toggled (GtkCheckButton *chk, gpointer d)
{
    (void) d;
    dv_debug_set_logging_enabled (gtk_check_button_get_active (chk));
}

/*
 * Custom window, not GtkAboutDialog: GTK4's GtkAboutDialog has no way to
 * add a widget of its own (no add_child()/child-widget API), and the
 * Diagnostics checkbox below needs to live somewhere. Same reasoning
 * DwarvenThemer4GTK's own About window gives for being a plain GtkWindow
 * rather than a stock about dialog.
 */
static void
on_about (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;

    static GtkWidget *about_win = NULL;
    if (about_win) {
        gtk_window_present (GTK_WINDOW (about_win));
        return;
    }

    about_win = gtk_window_new ();
    gtk_window_set_title         (GTK_WINDOW (about_win), "About DwarvenView");
    gtk_window_set_default_size  (GTK_WINDOW (about_win), 400, -1);
    gtk_window_set_resizable     (GTK_WINDOW (about_win), FALSE);
    gtk_window_set_modal         (GTK_WINDOW (about_win), TRUE);
    gtk_window_set_transient_for (GTK_WINDOW (about_win), GTK_WINDOW (state.window));
    gtk_window_set_destroy_with_parent (GTK_WINDOW (about_win), TRUE);
    g_signal_connect_swapped (about_win, "destroy",
                              G_CALLBACK (g_nullify_pointer), &about_win);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top    (box, 16);
    gtk_widget_set_margin_bottom (box, 16);
    gtk_widget_set_margin_start  (box, 16);
    gtk_widget_set_margin_end    (box, 16);
    gtk_window_set_child (GTK_WINDOW (about_win), box);

    GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon   = gtk_image_new_from_icon_name ("dwarvenview");
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
    gtk_box_append (GTK_BOX (header), icon);
    GtkWidget *name_col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name_lbl = gtk_label_new ("DwarvenView");
    gtk_label_set_xalign (GTK_LABEL (name_lbl), 0.0f);
    gtk_widget_add_css_class (name_lbl, "title-2");
    gtk_box_append (GTK_BOX (name_col), name_lbl);
    char      *ver_text = g_strdup_printf ("Version %s", PACKAGE_VERSION);
    GtkWidget *ver_lbl  = gtk_label_new (ver_text);
    g_free (ver_text);
    gtk_label_set_xalign (GTK_LABEL (ver_lbl), 0.0f);
    gtk_box_append (GTK_BOX (name_col), ver_lbl);
    gtk_box_append (GTK_BOX (header), name_col);
    gtk_box_append (GTK_BOX (box), header);

    GtkWidget *comments = gtk_label_new (
        "Fast, minimal GTK4 image viewer. Part of DwarvenSuite.");
    gtk_label_set_xalign (GTK_LABEL (comments), 0.0f);
    gtk_label_set_wrap (GTK_LABEL (comments), TRUE);
    gtk_box_append (GTK_BOX (box), comments);

    GtkWidget *license = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (license),
        "This program comes with absolutely no warranty.\n"
        "See the <a href=\"https://www.gnu.org/licenses/old-licenses/gpl-2.0.html\">"
        "GNU General Public License, version 2 or later</a> for details.");
    gtk_label_set_xalign (GTK_LABEL (license), 0.0f);
    gtk_label_set_wrap (GTK_LABEL (license), TRUE);
    gtk_box_append (GTK_BOX (box), license);

    gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *diag_lbl = gtk_label_new ("Diagnostics");
    gtk_label_set_xalign (GTK_LABEL (diag_lbl), 0.0f);
    gtk_widget_add_css_class (diag_lbl, "heading");
    gtk_box_append (GTK_BOX (box), diag_lbl);

    char      *log_path  = dv_debug_log_path ();
    char      *chk_label = g_strdup_printf ("Log image decode calls to %s", log_path);
    GtkWidget *chk = gtk_check_button_new_with_label (chk_label);
    g_free (chk_label);
    gtk_label_set_wrap (GTK_LABEL (gtk_widget_get_first_child (chk)), TRUE);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (chk), dv_debug_logging_enabled ());
    gtk_widget_set_tooltip_text (chk,
        "For investigating crashes and decode failures on specific images. "
        "Takes effect immediately. Turning this off deletes the existing "
        "log file right away, so nothing lingers once you're done. Logs "
        "every libavcodec/librsvg/GdkPixbuf call around loading an image, "
        "immediately before and after -- if DwarvenView crashes, the log's "
        "last line is whatever call was in flight.");
    g_signal_connect (chk, "toggled", G_CALLBACK (on_debug_log_toggled), NULL);
    g_free (log_path);
    gtk_box_append (GTK_BOX (box), chk);

    GtkWidget *btn_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign (btn_row, GTK_ALIGN_END);
    GtkWidget *btn_close = gtk_button_new_with_label ("Close");
    g_signal_connect_swapped (btn_close, "clicked",
                              G_CALLBACK (gtk_window_destroy), about_win);
    gtk_box_append (GTK_BOX (btn_row), btn_close);
    gtk_box_append (GTK_BOX (box), btn_row);

    /* Escape/Enter close it; focus starts on the one control so Space toggles it */
    dv_dialog_keys_attach (about_win, NULL, NULL, NULL);
    gtk_window_set_focus (GTK_WINDOW (about_win), chk);

    gtk_window_present (GTK_WINDOW (about_win));
}

/* ── Slideshow ──────────────────────────────────────────────────────────── */

/*
 * Parse slideshow interval string to milliseconds.
 * Accepts: "3" -> 3000ms, "0.5" -> 500ms, "1/30" -> 33ms, "100" -> 100000ms
 * Minimum: 16ms (60fps). Maximum: 3,600,000ms (1 hour).
 */
static guint
slideshow_parse_interval_ms (const char *s)
{
    if (!s || !*s) return 3000;

    double ms = 0;
    /* Check for fraction format "N/D" */
    char *slash = strchr (s, '/');
    if (slash) {
        double num = atof (s);
        double den = atof (slash + 1);
        if (den > 0) ms = (num / den) * 1000.0;
    } else {
        double secs = atof (s);
        ms = secs * 1000.0;
    }

    if (ms < 16)        ms = 16;       /* cap at 60fps */
    if (ms > 3600000)   ms = 3600000;  /* cap at 1 hour */
    return (guint) ms;
}

static gboolean
slideshow_tick (gpointer data)
{
    (void) data;
    if (!state.slideshow_timer) return G_SOURCE_REMOVE;
    state.slideshow_advancing = TRUE;
    go_next ();
    state.slideshow_advancing = FALSE;
    return G_SOURCE_CONTINUE;
}

static void
toggle_fullscreen (void)
{
    GtkWindow *win = GTK_WINDOW (state.window);
    if (gtk_window_is_fullscreen (win))
        gtk_window_unfullscreen (win);
    else
        gtk_window_fullscreen (win);
}

/* Fullscreen shows the controls unless the user chose "image only". */
static void
sync_fullscreen_chrome (void)
{
    gboolean hide = gtk_window_is_fullscreen (GTK_WINDOW (state.window)) &&
                    state.prefs && state.prefs->fullscreen_hide_ui;
    gtk_widget_set_visible (state.menubar,   !hide);
    gtk_widget_set_visible (state.toolbar,   !hide);
    gtk_widget_set_visible (state.statusbar, !hide);
}

static void
on_fullscreen_notify (GObject *win, GParamSpec *ps, gpointer action)
{
    (void) ps;
    gboolean is_fs = gtk_window_is_fullscreen (GTK_WINDOW (win));
    /* Sync stateful action state so menu shows checkmark */
    g_simple_action_set_state (G_SIMPLE_ACTION (action),
                               g_variant_new_boolean (is_fs));
    sync_fullscreen_chrome ();
}

static void
on_fullscreen_action (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    toggle_fullscreen ();
}

static void
on_slideshow_action (GSimpleAction *a, GVariant *p, gpointer d)
{
    (void) a; (void) p; (void) d;
    on_slideshow_toggle (NULL, NULL);
}

static void
slideshow_stop (void)
{
    if (state.slideshow_timer) {
        g_source_remove (state.slideshow_timer);
        state.slideshow_timer = 0;
    }
    if (state.tb_slideshow)
        gtk_button_set_icon_name (GTK_BUTTON (state.tb_slideshow),
                                  "media-playback-start-symbolic");
}

static void
slideshow_start (void)
{
    slideshow_stop ();
    const char *interval_str = state.prefs && state.prefs->slideshow_interval
        ? state.prefs->slideshow_interval : "3";
    guint ms = slideshow_parse_interval_ms (interval_str);
    state.slideshow_timer = g_timeout_add (ms, slideshow_tick, NULL);
    if (state.tb_slideshow)
        gtk_button_set_icon_name (GTK_BUTTON (state.tb_slideshow),
                                  "media-playback-pause-symbolic");
}

static void
on_slideshow_toggle (GtkButton *b, gpointer d)
{
    (void) b; (void) d;
    if (state.slideshow_timer)
        slideshow_stop ();
    else
        slideshow_start ();
}

static gboolean
on_window_close (GtkWindow *w, gpointer d)
{
    (void) w; (void) d;
    slideshow_stop ();
    g_application_quit (G_APPLICATION (state.app));
    return FALSE;   /* allow close */
}

static void
show_convert_menu (GtkButton *btn, gpointer data)
{
    (void) data;
    GMenu *menu = g_menu_new ();

    for (int i = 0; i < DV_EXPORT_NFORMATS; i++) {
        char      *label = g_strdup_printf ("Convert to %s",
                                            DV_EXPORT_FORMATS[i].label);
        GMenuItem *item  = g_menu_item_new (label, NULL);
        g_menu_item_set_action_and_target (item, "widget.export",
                                           "i", (gint32) i);
        g_menu_append_item (menu, item);
        g_object_unref (item);
        g_free (label);
    }

    GtkWidget *popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    g_object_unref (menu);

    /* Parent to DvWidget so the "widget" action group is in scope.
     * Point the popover at the toolbar button using a rectangle. */
    gtk_widget_set_parent (popover, state.view);

    /* Compute button position relative to the DvWidget */
    graphene_point_t pt  = GRAPHENE_POINT_INIT (0, 0);
    graphene_point_t out = GRAPHENE_POINT_INIT (0, 0);
    if (!gtk_widget_compute_point (GTK_WIDGET (btn), state.view, &pt, &out)) {
        out.x = 0; out.y = 0;   /* fallback if widgets not in same tree */
    }
    int bw = gtk_widget_get_width  (GTK_WIDGET (btn));
    int bh = gtk_widget_get_height (GTK_WIDGET (btn));

    GdkRectangle rect = { (int)out.x, (int)out.y, bw, bh };
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
    gtk_popover_set_position    (GTK_POPOVER (popover), GTK_POS_BOTTOM);

    gtk_popover_popup (GTK_POPOVER (popover));
}


static void
on_rotate_cw (GtkButton *b, gpointer d)
{
    (void) b; (void) d;
    dv_widget_rotate_cw (DV_WIDGET (state.view));
}
static void
on_rotate_ccw (GtkButton *b, gpointer d)
{
    (void) b; (void) d;
    dv_widget_rotate_ccw (DV_WIDGET (state.view));
}
static void
on_flip_h (GtkButton *b, gpointer d)
{
    (void) b; (void) d;
    dv_widget_flip_h (DV_WIDGET (state.view));
}
static void
on_flip_v (GtkButton *b, gpointer d)
{
    (void) b; (void) d;
    dv_widget_flip_v (DV_WIDGET (state.view));
}

/* ── Drag and drop ──────────────────────────────────────────────────────── */

static gboolean
on_drop (GtkDropTarget *target, const GValue *value,
         double x, double y, gpointer data)
{
    (void) target; (void) x; (void) y; (void) data;
    if (!G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST)) return FALSE;

    GSList *files = g_value_get_boxed (value);
    if (!files) return FALSE;

    /* Load first dropped file */
    GFile *f    = G_FILE (files->data);
    char  *path = g_file_get_path (f);
    if (path) {
        load_file (path);
        g_free (path);
    }
    return TRUE;
}

static gint
cmp_app_name (gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp (
        g_app_info_get_display_name (G_APP_INFO (a)),
        g_app_info_get_display_name (G_APP_INFO (b)));
}

static void
build_ui (GtkApplication *app)
{
    state.window = gtk_application_window_new (app);
    gtk_window_set_title (GTK_WINDOW (state.window), "DwarvenView");
    gtk_window_set_default_size (GTK_WINDOW (state.window), 1024, 768);

    /* ── Menu bar via GMenuModel + GtkPopoverMenuBar ── */
    /* Actions wired to window action group */
    GSimpleActionGroup *win_ag = g_simple_action_group_new ();

    GSimpleAction *a_open       = g_simple_action_new ("open",        NULL);
    GSimpleAction *a_open_folder = g_simple_action_new ("open-folder", NULL);
    state.a_props = g_simple_action_new ("properties", NULL);
    GSimpleAction *a_quit        = g_simple_action_new ("quit",        NULL);
    state.a_prev = g_simple_action_new ("prev",  NULL);
    state.a_next = g_simple_action_new ("next",  NULL);
    GSimpleAction *a_prev = state.a_prev;
    GSimpleAction *a_next = state.a_next;
    GSimpleAction *a_fs    = g_simple_action_new_stateful ("fullscreen", NULL, g_variant_new_boolean (FALSE));
    GSimpleAction *a_prefs   = g_simple_action_new ("prefs",    NULL);
    state.a_slideshow = g_simple_action_new ("slideshow",  NULL);
    GSimpleAction *a_slideshow = state.a_slideshow;
    /* Plain action, not stateful -- a stateful boolean renders as a
     * checkbox in GMenu, which we don't want. The real state lives in
     * DvWidget's view_mode; the menu label is swapped live instead. */
    state.a_zoom_actual = g_simple_action_new ("zoom-actual", NULL);
    GSimpleAction *a_zoom_actual = state.a_zoom_actual;
    state.a_zoom_in  = g_simple_action_new ("zoom-in",    NULL);
    GSimpleAction *a_zoom_in   = state.a_zoom_in;
    state.a_zoom_out = g_simple_action_new ("zoom-out",   NULL);
    GSimpleAction *a_zoom_out  = state.a_zoom_out;
    state.a_zoom_rst = g_simple_action_new ("zoom-reset", NULL);
    GSimpleAction *a_zoom_rst  = state.a_zoom_rst;
    GSimpleAction *a_help      = g_simple_action_new ("help",       NULL);
    GSimpleAction *a_about     = g_simple_action_new ("about",      NULL);

    g_signal_connect (a_open,        "activate", G_CALLBACK (action_open),        NULL);
    g_signal_connect (a_open_folder, "activate", G_CALLBACK (action_open_folder),  NULL);
    g_signal_connect (state.a_props, "activate", G_CALLBACK (action_properties),   NULL);
    g_signal_connect_swapped (a_quit,  "activate",
        G_CALLBACK (g_application_quit), state.app);
    g_signal_connect_swapped (a_prev,  "activate", G_CALLBACK (go_prev),     NULL);
    g_signal_connect_swapped (a_next,  "activate", G_CALLBACK (go_next),     NULL);
    g_signal_connect          (a_fs,    "activate", G_CALLBACK (on_fullscreen_action), NULL);
    g_signal_connect_swapped (a_prefs,    "activate", G_CALLBACK (show_prefs_dialog), NULL);
    g_signal_connect          (a_slideshow,"activate", G_CALLBACK (on_slideshow_action), NULL);
    g_signal_connect          (a_zoom_actual, "activate", G_CALLBACK (on_zoom_actual),    NULL);
    g_signal_connect          (a_zoom_in,  "activate", G_CALLBACK (on_zoom_in),         NULL);
    g_signal_connect          (a_zoom_out, "activate", G_CALLBACK (on_zoom_out),        NULL);
    g_signal_connect          (a_zoom_rst, "activate", G_CALLBACK (on_zoom_reset),      NULL);
    g_signal_connect          (a_help,     "activate", G_CALLBACK (on_help),            NULL);
    g_signal_connect          (a_about,    "activate", G_CALLBACK (on_about),           NULL);

    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_open));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_open_folder));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (state.a_props));
    g_simple_action_set_enabled (state.a_props, FALSE);   /* until an image is loaded */
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_quit));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_prev));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_next));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_fs));
    /* Sync fullscreen action state when window fullscreen state changes */
    g_signal_connect (state.window, "notify::fullscreened",
                      G_CALLBACK (on_fullscreen_notify), a_fs);
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_prefs));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_slideshow));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_zoom_actual));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_zoom_in));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_zoom_out));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_zoom_rst));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_help));
    g_action_map_add_action (G_ACTION_MAP (win_ag), G_ACTION (a_about));
    /* Disable nav/transform/convert until an image is loaded */
    g_simple_action_set_enabled (state.a_prev, FALSE);
    g_simple_action_set_enabled (state.a_next, FALSE);
    g_simple_action_set_enabled (state.a_slideshow, FALSE);
    g_simple_action_set_enabled (state.a_zoom_in,    FALSE);
    g_simple_action_set_enabled (state.a_zoom_actual, FALSE);
    g_simple_action_set_enabled (state.a_zoom_out,   FALSE);
    g_simple_action_set_enabled (state.a_zoom_rst,   FALSE);
    if (state.a_reset_xform) g_simple_action_set_enabled (state.a_reset_xform, FALSE);
    gtk_widget_insert_action_group (state.window, "dv", G_ACTION_GROUP (win_ag));

    GMenu *menumodel = g_menu_new ();

    /* File */
    GMenu *file_m = g_menu_new ();
    /* Open section */
    GMenu *file_open_m = g_menu_new ();
    g_menu_append (file_open_m, "_Open File...",    "dv.open");
    g_menu_append (file_open_m, "Open _Folder...",  "dv.open-folder");
    g_menu_append_section (file_m, NULL, G_MENU_MODEL (file_open_m));
    /* Properties section */
    GMenu *file_props_m = g_menu_new ();
    g_menu_append (file_props_m, "Propert_ies...", "dv.properties");
    g_menu_append_section (file_m, NULL, G_MENU_MODEL (file_props_m));
    /* Preferences section */
    GMenu *file_prefs_m = g_menu_new ();
    g_menu_append (file_prefs_m, "_Preferences...", "dv.prefs");
    g_menu_append_section (file_m, NULL, G_MENU_MODEL (file_prefs_m));
    /* Exit section */
    GMenu *file_exit_m = g_menu_new ();
    g_menu_append (file_exit_m, "_Exit DwarvenView", "dv.quit");
    g_menu_append_section (file_m, NULL, G_MENU_MODEL (file_exit_m));
    g_menu_append_submenu (menumodel, "_File", G_MENU_MODEL (file_m));

    /* View */
    GMenu *view_m    = g_menu_new ();
    /* Fullscreen first */
    GMenu *view_fs_m = g_menu_new ();
    g_menu_append (view_fs_m, "_Fullscreen", "dv.fullscreen");   /* stateful */
    g_menu_append_section (view_m, NULL, G_MENU_MODEL (view_fs_m));
    g_object_unref (view_fs_m);
    /* Navigation + slideshow */
    GMenu *view_nav_m = g_menu_new ();
    g_menu_append (view_nav_m, "_Previous Image",    "dv.prev");
    g_menu_append (view_nav_m, "_Next Image",        "dv.next");
    g_menu_append (view_nav_m, "_Slideshow (Space)", "dv.slideshow");
    g_menu_append_section (view_m, NULL, G_MENU_MODEL (view_nav_m));
    g_object_unref (view_nav_m);
    g_menu_append_submenu (menumodel, "_View", G_MENU_MODEL (view_m));

    /* Transform */
    GMenu *xform_m    = g_menu_new ();
    GMenu *xform_rot  = g_menu_new ();
    GMenu *xform_zoom = g_menu_new ();
    g_menu_append (xform_rot, "Rotate 90° _Clockwise",         "widget.rotate-cw");
    g_menu_append (xform_rot, "Rotate 90° Co_unter-clockwise", "widget.rotate-ccw");
    g_menu_append (xform_rot, "Flip _Horizontal",              "widget.flip-h");
    g_menu_append (xform_rot, "Flip _Vertical",                "widget.flip-v");
    g_menu_append (xform_rot, "_Reset Transform",              "widget.reset-transform");
    g_menu_append (xform_zoom, "Zoom _In",                     "dv.zoom-in");
    g_menu_append (xform_zoom, "Zoom _Out",                    "dv.zoom-out");
    g_menu_append (xform_zoom, "Reset _Zoom",                  "dv.zoom-reset");
    g_menu_append (xform_zoom, "_Actual Size (1:1)",           "dv.zoom-actual");
    /* Keep our own ref -- label at index 3 is swapped live from update_statusbar */
    state.xform_zoom_m = g_object_ref (xform_zoom);
    g_menu_append_section (xform_m, NULL, G_MENU_MODEL (xform_rot));
    g_menu_append_section (xform_m, NULL, G_MENU_MODEL (xform_zoom));
    g_menu_append_submenu (menumodel, "_Transform", G_MENU_MODEL (xform_m));

    /* Export */
    GMenu *export_m   = g_menu_new ();
    /* Wallpaper section */
    GMenu *wallpaper_m = g_menu_new ();
    g_menu_append (wallpaper_m, "Set as _Wallpaper", "widget.set-wallpaper");
    g_menu_append_section (export_m, NULL, G_MENU_MODEL (wallpaper_m));

    /* Send to -- populated from XDG app list */
    GMenu *sendto_m = g_menu_new ();
    {
        GList *apps = g_app_info_get_all_for_type ("image/png");
        apps = g_list_sort (apps, cmp_app_name);
        for (GList *l = apps; l; l = l->next) {
            GAppInfo   *app  = G_APP_INFO (l->data);
            const char *id   = g_app_info_get_id (app);
            const char *name = g_app_info_get_display_name (app);
            if (id && strstr (id, "dwarvenview")) continue;
            if (!id || !name) continue;
            GMenuItem *item = g_menu_item_new (name, NULL);
            g_menu_item_set_action_and_target (item, "widget.send-to", "s", id);
            g_menu_append_item (sendto_m, item);
            g_object_unref (item);
        }
        g_list_free_full (apps, g_object_unref);
    }

    state.export_convert_m = g_menu_new ();
    GMenu *convert_m = state.export_convert_m;
    /* Populated without SVG initially -- rebuilt on image load */
    for (int i = 0; i < DV_EXPORT_NFORMATS; i++) {
        if (strcmp (DV_EXPORT_FORMATS[i].ext, "svg") == 0) continue;
        GMenuItem *item = g_menu_item_new (DV_EXPORT_FORMATS[i].label, NULL);
        g_menu_item_set_action_and_target (item, "widget.export", "i", (gint32) i);
        g_menu_append_item (convert_m, item);
        g_object_unref (item);
    }
    g_menu_append_section (export_m, "Convert to", G_MENU_MODEL (convert_m));
    if (g_menu_model_get_n_items (G_MENU_MODEL (sendto_m)) > 0)
        g_menu_append_section (export_m, "Send to",  G_MENU_MODEL (sendto_m));
    g_menu_append_submenu (menumodel, "_Export", G_MENU_MODEL (export_m));

    /* Help */
    GMenu *help_m = g_menu_new ();
    g_menu_append (help_m, "_Help",              "dv.help");
    g_menu_append (help_m, "_About DwarvenView", "dv.about");
    g_menu_append_submenu (menumodel, "_Help", G_MENU_MODEL (help_m));

    g_object_unref (file_m); g_object_unref (view_m);
    g_object_unref (xform_m); g_object_unref (xform_rot); g_object_unref (xform_zoom);
    g_object_unref (export_m); g_object_unref (sendto_m); g_object_unref (convert_m);
    g_object_unref (wallpaper_m);
    g_object_unref (help_m);

    GtkWidget *menubar = gtk_popover_menu_bar_new_from_model (
        G_MENU_MODEL (menumodel));

    g_object_unref (file_m);
    g_object_unref (view_m);
    g_object_unref (menumodel);

    /* ── Toolbar ── (nav + open buttons below menu bar) */
    GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start  (toolbar, 4);
    gtk_widget_set_margin_end    (toolbar, 4);
    gtk_widget_set_margin_top    (toolbar, 2);
    gtk_widget_set_margin_bottom (toolbar, 2);

    /* Order: Open | Prev | Next | <stretch> | Convert-to | Prefs */
    GtkWidget *open_btn     = gtk_button_new_from_icon_name ("document-open-symbolic");
    GtkWidget *prev_btn     = gtk_button_new_from_icon_name ("go-previous-symbolic");
    GtkWidget *next_btn     = gtk_button_new_from_icon_name ("go-next-symbolic");
    state.tb_prev    = prev_btn;
    state.tb_next    = next_btn;
    GtkWidget *convert_btn  = gtk_button_new_from_icon_name ("document-save-as-symbolic");
    state.tb_convert = convert_btn;
    GtkWidget *prefs_btn    = gtk_button_new_from_icon_name ("preferences-system-symbolic");

    gtk_widget_set_tooltip_text (open_btn,    "Open image (Ctrl+O)");
    gtk_widget_set_sensitive (prev_btn, FALSE);
    gtk_widget_set_sensitive (next_btn, FALSE);
    gtk_widget_set_tooltip_text (open_btn, "Open file or folder (Ctrl+O)");
    gtk_widget_set_tooltip_text (prev_btn, "Previous image (Page Up)");
    gtk_widget_set_tooltip_text (next_btn, "Next image (Page Down)");
    gtk_widget_set_tooltip_text (convert_btn, "Convert to...");
    gtk_widget_set_tooltip_text (prefs_btn,   "Preferences");

    g_signal_connect          (open_btn,    "clicked", G_CALLBACK (action_open),       NULL);
    g_signal_connect_swapped  (prev_btn,    "clicked", G_CALLBACK (go_prev),           NULL);
    g_signal_connect_swapped  (next_btn,    "clicked", G_CALLBACK (go_next),           NULL);
    g_signal_connect          (convert_btn, "clicked", G_CALLBACK (show_convert_menu), NULL);
    g_signal_connect_swapped  (prefs_btn,   "clicked", G_CALLBACK (show_prefs_dialog), NULL);

    /* Stretch spacer between nav and right-side buttons */
    GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (spacer, TRUE);

    gtk_box_append (GTK_BOX (toolbar), open_btn);
    gtk_box_append (GTK_BOX (toolbar), prev_btn);
    gtk_box_append (GTK_BOX (toolbar), next_btn);

    state.tb_slideshow = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
    gtk_widget_set_tooltip_text (state.tb_slideshow, "Slideshow (Space)");
    gtk_widget_set_sensitive    (state.tb_slideshow, FALSE);
    g_signal_connect (state.tb_slideshow, "clicked",
                      G_CALLBACK (on_slideshow_toggle), NULL);
    gtk_box_append (GTK_BOX (toolbar), state.tb_slideshow);

    /* Fullscreen toggle button */
    GtkWidget *fs_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (fs_btn), "view-fullscreen-symbolic");
    gtk_widget_set_tooltip_text (fs_btn, "Fullscreen (F11)");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (fs_btn), "dv.fullscreen");
    gtk_box_append (GTK_BOX (toolbar), fs_btn);
    GtkWidget *xform_rst_btn = gtk_button_new_from_icon_name ("edit-undo-symbolic");
    state.tb_xform_rst = xform_rst_btn;
    gtk_widget_set_tooltip_text (xform_rst_btn, "Reset Transform");
    gtk_widget_set_sensitive    (xform_rst_btn, FALSE);
    g_signal_connect (xform_rst_btn, "clicked", G_CALLBACK (on_reset_transform), NULL);
    GtkWidget *rot_cw_btn  = gtk_button_new_from_icon_name ("object-rotate-right-symbolic");
    state.tb_rot_cw  = rot_cw_btn;
    GtkWidget *rot_ccw_btn = gtk_button_new_from_icon_name ("object-rotate-left-symbolic");
    state.tb_rot_ccw = rot_ccw_btn;
    GtkWidget *flip_h_btn  = gtk_button_new_from_icon_name ("object-flip-horizontal-symbolic");
    state.tb_flip_h  = flip_h_btn;
    GtkWidget *flip_v_btn  = gtk_button_new_from_icon_name ("object-flip-vertical-symbolic");
    state.tb_flip_v  = flip_v_btn;
    gtk_widget_set_sensitive (prev_btn,    FALSE);
    gtk_widget_set_sensitive (next_btn,    FALSE);
    gtk_widget_set_sensitive (rot_cw_btn,  FALSE);
    gtk_widget_set_sensitive (rot_ccw_btn, FALSE);
    gtk_widget_set_sensitive (flip_h_btn,  FALSE);
    gtk_widget_set_sensitive (flip_v_btn,  FALSE);
    gtk_widget_set_sensitive (convert_btn, FALSE);
    gtk_widget_set_tooltip_text (rot_cw_btn,  "Rotate 90° Clockwise (R or Shift+L)");
    gtk_widget_set_tooltip_text (rot_ccw_btn, "Rotate 90° Counter-clockwise (Shift+R or L)");
    gtk_widget_set_tooltip_text (flip_h_btn,  "Flip Horizontal (H)");
    gtk_widget_set_tooltip_text (flip_v_btn,  "Flip Vertical (V)");
    g_signal_connect (rot_cw_btn,  "clicked", G_CALLBACK (on_rotate_cw),  NULL);
    g_signal_connect (rot_ccw_btn, "clicked", G_CALLBACK (on_rotate_ccw), NULL);
    g_signal_connect (flip_h_btn,  "clicked", G_CALLBACK (on_flip_h),     NULL);
    g_signal_connect (flip_v_btn,  "clicked", G_CALLBACK (on_flip_v),     NULL);

    GtkWidget *zoom_in_btn  = gtk_button_new_from_icon_name ("zoom-in-symbolic");
    GtkWidget *zoom_out_btn = gtk_button_new_from_icon_name ("zoom-out-symbolic");
    GtkWidget *zoom_rst_btn = gtk_button_new_from_icon_name ("zoom-fit-best-symbolic");
    state.tb_zoom_in  = zoom_in_btn;
    state.tb_zoom_out = zoom_out_btn;
    state.tb_zoom_rst = zoom_rst_btn;
    gtk_widget_set_sensitive (zoom_in_btn,  FALSE);
    gtk_widget_set_sensitive (zoom_out_btn, FALSE);
    gtk_widget_set_sensitive (zoom_rst_btn, FALSE);
    gtk_widget_set_tooltip_text (zoom_in_btn,  "Zoom In (+)");
    gtk_widget_set_tooltip_text (zoom_out_btn, "Zoom Out (-)");
    gtk_widget_set_tooltip_text (zoom_rst_btn, "Reset Zoom");
    g_signal_connect (zoom_in_btn,  "clicked", G_CALLBACK (on_zoom_in_btn),    NULL);
    g_signal_connect (zoom_out_btn, "clicked", G_CALLBACK (on_zoom_out_btn),   NULL);
    g_signal_connect (zoom_rst_btn, "clicked", G_CALLBACK (on_zoom_reset_btn), NULL);
    GtkWidget *sep1 = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    GtkWidget *sep2 = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_start (sep1, 4);
    gtk_widget_set_margin_end   (sep1, 4);
    gtk_widget_set_margin_start (sep2, 4);
    gtk_widget_set_margin_end   (sep2, 4);

    GtkWidget *sep3 = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    GtkWidget *sep4 = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_start (sep3, 4); gtk_widget_set_margin_end (sep3, 4);
    gtk_widget_set_margin_start (sep4, 4); gtk_widget_set_margin_end (sep4, 4);

    gtk_box_append (GTK_BOX (toolbar), sep1);
    gtk_box_append (GTK_BOX (toolbar), rot_cw_btn);
    gtk_box_append (GTK_BOX (toolbar), rot_ccw_btn);
    gtk_box_append (GTK_BOX (toolbar), flip_h_btn);
    gtk_box_append (GTK_BOX (toolbar), flip_v_btn);
    gtk_box_append (GTK_BOX (toolbar), xform_rst_btn);
    gtk_box_append (GTK_BOX (toolbar), sep2);
    GtkWidget *zoom_act_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (zoom_act_btn), "zoom-original-symbolic");
    gtk_widget_set_tooltip_text (zoom_act_btn, "Actual Size 1:1 (Ctrl+1)");
    gtk_widget_set_sensitive (zoom_act_btn, FALSE);
    state.tb_zoom_actual = zoom_act_btn;
    /* Direct signal -- avoids stateful action/toggle button interaction bugs */
    g_signal_connect (zoom_act_btn, "clicked", G_CALLBACK (on_zoom_actual_btn), NULL);
    gtk_box_append (GTK_BOX (toolbar), zoom_in_btn);
    gtk_box_append (GTK_BOX (toolbar), zoom_out_btn);
    gtk_box_append (GTK_BOX (toolbar), zoom_rst_btn);
    gtk_box_append (GTK_BOX (toolbar), zoom_act_btn);
    gtk_box_append (GTK_BOX (toolbar), sep3);
    gtk_box_append (GTK_BOX (toolbar), convert_btn);
    gtk_box_append (GTK_BOX (toolbar), spacer);
    gtk_box_append (GTK_BOX (toolbar), prefs_btn);
    GtkWidget *help_btn = gtk_button_new_from_icon_name ("help-browser-symbolic");
    gtk_widget_set_tooltip_text (help_btn, "Help (F1)");
    g_signal_connect_swapped (help_btn, "clicked", G_CALLBACK (on_help), NULL);
    gtk_box_append (GTK_BOX (toolbar), help_btn);

    /* ── View widget + status bar ── */
    state.view = dv_widget_new ();

    /* Also insert the widget action group on the window so GtkPopoverMenuBar
     * can resolve "widget." actions (it walks up to window, not down into children) */
    gtk_widget_insert_action_group (state.window, "widget",
        dv_widget_get_action_group (DV_WIDGET (state.view)));

    state.statusbar = gtk_label_new ("No image");
    gtk_widget_set_halign (state.statusbar, GTK_ALIGN_START);
    gtk_label_set_selectable (GTK_LABEL (state.statusbar), FALSE);
    gtk_widget_set_margin_start  (state.statusbar, 8);
    gtk_widget_set_margin_end    (state.statusbar, 8);
    gtk_widget_set_margin_top    (state.statusbar, 2);
    gtk_widget_set_margin_bottom (state.statusbar, 2);

    state.menubar = menubar;
    state.toolbar = toolbar;
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (vbox), menubar);
    gtk_box_append (GTK_BOX (vbox), toolbar);
    gtk_box_append (GTK_BOX (vbox), state.view);
    gtk_box_append (GTK_BOX (vbox), state.statusbar);
    gtk_window_set_child (GTK_WINDOW (state.window), vbox);

    /* Apply prefs to widget */
    dv_widget_set_pan_prefs (DV_WIDGET (state.view), &state.prefs->pan);
    dv_widget_set_prefs     (DV_WIDGET (state.view), state.prefs);

    /* Connect status update signals */
    g_signal_connect (state.view, "zoom-changed",   G_CALLBACK (on_zoom_changed),   NULL);
    g_signal_connect (state.view, "load-complete",  G_CALLBACK (on_load_complete),  NULL);
    g_signal_connect_swapped (state.view, "transform-changed",
                              G_CALLBACK (update_statusbar), NULL);
    g_signal_connect (state.view, "help-requested",
                      G_CALLBACK (on_help_wrapper), NULL);
    g_signal_connect_swapped (state.view, "prefs-requested",
                              G_CALLBACK (show_prefs_dialog), NULL);

    /* Window keyboard handler */
    GtkEventController *key = gtk_event_controller_key_new ();
    g_signal_connect (key, "key-pressed", G_CALLBACK (on_window_key), NULL);
    gtk_widget_add_controller (state.window, key);

    /* Drag and drop -- accept files dropped onto the window */
    GtkDropTarget *drop = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect (drop, "drop", G_CALLBACK (on_drop), NULL);
    gtk_widget_add_controller (state.window, GTK_EVENT_CONTROLLER (drop));

    /* Quit app when window closes (stop slideshow first) */
    g_signal_connect (state.window, "close-request",
        G_CALLBACK (on_window_close), NULL);

    gtk_window_present (GTK_WINDOW (state.window));
}

/* ── Application signals ────────────────────────────────────────────────── */

static void
on_activate (GtkApplication *app, gpointer data)
{
    (void) data;
    state.app   = app;
    state.prefs = dv_prefs_load ();
    av_log_set_level (AV_LOG_ERROR);   /* suppress libav probing noise */
    build_ui (app);
    dv_install_icon ();
    gtk_window_set_icon_name (GTK_WINDOW (state.window), "dwarvenview");
    dwarven4gtk3restore_init ();
    dwarven4gtk3restore_watch ();
}

static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint,
         gpointer data)
{
    (void) app; (void) hint; (void) data;
    on_activate (GTK_APPLICATION (app), NULL);
    if (n_files > 0) {
        char *path = g_file_get_path (files[0]);
        if (path) load_file (path);
        g_free (path);
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int
main (int argc, char **argv)
{
    GtkApplication *app = gtk_application_new (APP_ID,
        G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
    g_signal_connect (app, "open",     G_CALLBACK (on_open),     NULL);

    int status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);
    return status;
}
