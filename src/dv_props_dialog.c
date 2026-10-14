#include "dv_props_dialog.h"
#include "dv_dialog_keys.h"
#include "dv_props.h"

#include <string.h>

/* The single open window, or NULL. Cleared by the "destroy" signal. */
static GtkWidget *props_win = NULL;

/* Bumped on every fetch started for this window; a result tagged with an
 * older generation is discarded. This is what makes "follow the image"
 * safe under rapid navigation (Page Down held down, say): if the user has
 * already moved on to image 3 by the time image 2's tool finishes, image
 * 2's result must not overwrite what is now on screen. */
static guint fetch_gen = 0;

/* Widgets the fetch callback needs, stored on the window as object data. */
#define KEY_CONTENT "dv-props-content"
#define KEY_SOURCE  "dv-props-source"
#define KEY_HINT    "dv-props-hint"
#define KEY_COPY    "dv-props-copy"
#define KEY_TEXT    "dv-props-text"

/* ── Content ──────────────────────────────────────────────────────────────── */

static GtkWidget *
make_key_label (const char *key)
{
    GtkWidget *l = gtk_label_new (key);
    gtk_label_set_xalign (GTK_LABEL (l), 0.0f);
    gtk_label_set_yalign (GTK_LABEL (l), 0.0f);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 28);
    gtk_widget_add_css_class (l, "dim-label");
    return l;
}

static GtkWidget *
make_value_label (const char *value)
{
    GtkWidget *l = gtk_label_new (value);
    gtk_label_set_xalign (GTK_LABEL (l), 0.0f);
    gtk_label_set_yalign (GTK_LABEL (l), 0.0f);
    gtk_label_set_selectable (GTK_LABEL (l), TRUE);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_width_chars (GTK_LABEL (l), 24);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 72);
    gtk_widget_set_hexpand (l, TRUE);
    return l;
}

static void
clear_box (GtkWidget *box)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child (box)))
        gtk_box_remove (GTK_BOX (box), c);
}

/* Reset the window to the "Reading properties..." state. Used both for the
 * first open and every time the displayed image changes. */
static void
show_waiting (GtkWidget *win)
{
    GtkWidget *content = g_object_get_data (G_OBJECT (win), KEY_CONTENT);
    GtkWidget *source  = g_object_get_data (G_OBJECT (win), KEY_SOURCE);
    GtkWidget *hint    = g_object_get_data (G_OBJECT (win), KEY_HINT);
    GtkWidget *copy    = g_object_get_data (G_OBJECT (win), KEY_COPY);

    clear_box (content);
    GtkWidget *wait = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *spin = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (spin));
    gtk_box_append (GTK_BOX (wait), spin);
    gtk_box_append (GTK_BOX (wait), gtk_label_new ("Reading properties\xe2\x80\xa6"));
    gtk_box_append (GTK_BOX (content), wait);

    gtk_label_set_text (GTK_LABEL (source), "");
    gtk_label_set_text (GTK_LABEL (hint), "");
    gtk_widget_set_visible (hint, FALSE);
    gtk_widget_set_sensitive (copy, FALSE);
    g_object_set_data (G_OBJECT (win), KEY_TEXT, NULL);
}

static void
populate (GtkWidget *win, DvProps *p)
{
    GtkWidget *content = g_object_get_data (G_OBJECT (win), KEY_CONTENT);
    GtkWidget *source  = g_object_get_data (G_OBJECT (win), KEY_SOURCE);
    GtkWidget *hint    = g_object_get_data (G_OBJECT (win), KEY_HINT);
    GtkWidget *copy    = g_object_get_data (G_OBJECT (win), KEY_COPY);

    clear_box (content);

    /* One size group so every group's key column lines up. */
    GtkSizeGroup *keys = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

    for (guint i = 0; i < p->groups->len; i++) {
        DvPropGroup *g = g_ptr_array_index (p->groups, i);

        char      *markup = g_markup_printf_escaped ("<b>%s</b>", g->name);
        GtkWidget *head   = gtk_label_new (NULL);
        gtk_label_set_markup (GTK_LABEL (head), markup);
        gtk_label_set_xalign (GTK_LABEL (head), 0.0f);
        gtk_widget_set_margin_top (head, i ? 14 : 0);
        gtk_widget_set_margin_bottom (head, 4);
        gtk_box_append (GTK_BOX (content), head);
        g_free (markup);

        GtkWidget *grid = gtk_grid_new ();
        gtk_grid_set_column_spacing (GTK_GRID (grid), 16);
        gtk_grid_set_row_spacing (GTK_GRID (grid), 3);
        for (guint j = 0; j < g->rows->len; j++) {
            DvPropRow *r = g_ptr_array_index (g->rows, j);
            GtkWidget *k = make_key_label (r->key);
            gtk_size_group_add_widget (keys, k);
            gtk_grid_attach (GTK_GRID (grid), k, 0, (int) j, 1, 1);
            gtk_grid_attach (GTK_GRID (grid), make_value_label (r->value), 1, (int) j, 1, 1);
        }
        gtk_box_append (GTK_BOX (content), grid);
    }
    g_object_unref (keys);          /* widgets hold their own reference */

    char *src = g_markup_printf_escaped ("Source: <b>%s</b>", p->source);
    gtk_label_set_markup (GTK_LABEL (source), src);
    g_free (src);

    if (p->hint) {
        gtk_label_set_text (GTK_LABEL (hint), p->hint);
        gtk_widget_set_visible (hint, TRUE);
    }

    g_object_set_data_full (G_OBJECT (win), KEY_TEXT, dv_props_to_text (p), g_free);
    gtk_widget_set_sensitive (copy, TRUE);
}

/* ── Fetch ────────────────────────────────────────────────────────────────── */

typedef struct {
    GWeakRef win;
    guint    gen;
} FetchCtx;

static void
on_fetched (GObject *src, GAsyncResult *res, gpointer data)
{
    (void) src;
    FetchCtx *ctx = data;
    DvProps  *p   = dv_props_fetch_finish (res);

    /* Stale: either the window closed while the tool was running, or a
     * newer image has been loaded since this fetch was started. */
    GtkWidget *win = g_weak_ref_get (&ctx->win);
    if (win) {
        if (p && ctx->gen == fetch_gen)
            populate (win, p);
        g_object_unref (win);
    }
    dv_props_free (p);
    g_weak_ref_clear (&ctx->win);
    g_free (ctx);
}

/* Point the (already open) window at PATH: title, reset to "reading", kick
 * a new fetch. Shared by the first open and by following image navigation. */
static void
begin_fetch (GtkWidget *win, const char *path)
{
    char *base  = g_path_get_basename (path);
    char *title = g_strdup_printf ("Properties: %s", base);
    gtk_window_set_title (GTK_WINDOW (win), title);
    g_free (base);
    g_free (title);

    show_waiting (win);

    fetch_gen++;
    FetchCtx *ctx = g_new0 (FetchCtx, 1);
    g_weak_ref_init (&ctx->win, win);
    ctx->gen = fetch_gen;
    dv_props_fetch_async (path, on_fetched, ctx);
}

/* ── Window ───────────────────────────────────────────────────────────────── */

static void
on_copy_clicked (GtkButton *b, gpointer win)
{
    (void) b;
    const char *text = g_object_get_data (G_OBJECT (win), KEY_TEXT);
    if (text)
        gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (win)), text);
}

static void
on_close_clicked (GtkButton *b, gpointer win)
{
    (void) b;
    gtk_window_destroy (GTK_WINDOW (win));
}

static void
on_destroy (GtkWidget *w, gpointer unused)
{
    (void) unused;
    if (props_win == w)
        props_win = NULL;
    /* Bump the generation so any fetch still in flight for this window is
     * dropped by on_fetched() even in the (impossible in practice, but
     * cheap to guard) case its weak ref outlives this callback ordering. */
    fetch_gen++;
}

/* Build the (empty) window chrome once: header, scroll area, footer. Content
 * is filled in by begin_fetch()/populate(), not here. */
static GtkWidget *
build_window (GtkWindow *parent)
{
    GtkWidget *win = gtk_window_new ();
    gtk_window_set_default_size (GTK_WINDOW (win), 620, 580);
    if (parent)
        gtk_window_set_transient_for (GTK_WINDOW (win), parent);

    props_win = win;
    g_signal_connect (win, "destroy", G_CALLBACK (on_destroy), NULL);

    GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child (GTK_WINDOW (win), root);

    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top (content, 14);
    gtk_widget_set_margin_bottom (content, 14);
    gtk_widget_set_margin_start (content, 16);
    gtk_widget_set_margin_end (content, 16);

    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), content);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtk_box_append (GTK_BOX (root), scroll);
    gtk_box_append (GTK_BOX (root), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* Footer: optional hint, then source + buttons. */
    GtkWidget *foot = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top (foot, 8);
    gtk_widget_set_margin_bottom (foot, 8);
    gtk_widget_set_margin_start (foot, 16);
    gtk_widget_set_margin_end (foot, 16);

    GtkWidget *hint = gtk_label_new (NULL);
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0f);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_widget_set_visible (hint, FALSE);
    gtk_box_append (GTK_BOX (foot), hint);

    GtkWidget *bar    = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *source = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (source), 0.0f);
    gtk_widget_set_hexpand (source, TRUE);
    gtk_widget_add_css_class (source, "dim-label");
    GtkWidget *copy  = gtk_button_new_with_label ("Copy all");
    GtkWidget *close = gtk_button_new_with_label ("Close");
    gtk_widget_set_sensitive (copy, FALSE);
    gtk_box_append (GTK_BOX (bar), source);
    gtk_box_append (GTK_BOX (bar), copy);
    gtk_box_append (GTK_BOX (bar), close);
    gtk_box_append (GTK_BOX (foot), bar);
    gtk_box_append (GTK_BOX (root), foot);

    g_object_set_data (G_OBJECT (win), KEY_CONTENT, content);
    g_object_set_data (G_OBJECT (win), KEY_SOURCE, source);
    g_object_set_data (G_OBJECT (win), KEY_HINT, hint);
    g_object_set_data (G_OBJECT (win), KEY_COPY, copy);

    g_signal_connect (copy,  "clicked", G_CALLBACK (on_copy_clicked),  win);
    g_signal_connect (close, "clicked", G_CALLBACK (on_close_clicked), win);

    dv_dialog_keys_attach (win, NULL, NULL, NULL);

    /* Keep focus off the selectable labels, which would otherwise start with
     * their text selected. */
    gtk_window_set_focus (GTK_WINDOW (win), close);
    return win;
}

void
dv_props_dialog_show (GtkWindow *parent, const char *path)
{
    if (!path)
        return;

    /* Already open: refresh it in place (title, content, fetch) rather than
     * destroying and recreating -- the same thing navigation triggers via
     * dv_props_dialog_notify_path(), just invoked explicitly here. Keeps the
     * window's screen position instead of losing it on every reopen. */
    if (!props_win)
        build_window (parent);

    begin_fetch (props_win, path);
    gtk_window_present (GTK_WINDOW (props_win));
}

void
dv_props_dialog_notify_path (const char *path)
{
    /* No-op with no window open: navigating images costs nothing extra
     * when nobody is looking at Properties. */
    if (!props_win || !path)
        return;
    begin_fetch (props_win, path);
}
