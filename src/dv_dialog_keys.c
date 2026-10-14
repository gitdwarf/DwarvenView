#include "dv_dialog_keys.h"

typedef struct { DvDialogKeyFn accept, cancel; gpointer data; } Keys;

static gboolean
on_key (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType mods, gpointer user)
{
    (void) keycode; (void) mods;
    Keys      *k   = user;
    GtkWidget *win = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (ctrl));
    gboolean   enter = keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter;

    if (enter) {
        GtkWidget *f = gtk_window_get_focus (GTK_WINDOW (win));
        if (GTK_IS_BUTTON (f))
            return FALSE;
        if (GTK_IS_LABEL (f) && gtk_label_get_current_uri (GTK_LABEL (f)))
            return FALSE;
        if (k->accept) k->accept (win, k->data); else gtk_window_destroy (GTK_WINDOW (win));
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
        if (k->cancel) k->cancel (win, k->data); else gtk_window_destroy (GTK_WINDOW (win));
        return TRUE;
    }
    return FALSE;
}

void
dv_dialog_keys_attach (GtkWidget *win, DvDialogKeyFn accept, DvDialogKeyFn cancel, gpointer data)
{
    Keys *k = g_new0 (Keys, 1);
    k->accept = accept; k->cancel = cancel; k->data = data;
    GtkEventController *c = gtk_event_controller_key_new ();
    /* Capture phase: a focused check button or entry must not swallow the key first. */
    gtk_event_controller_set_propagation_phase (c, GTK_PHASE_CAPTURE);
    g_signal_connect (c, "key-pressed", G_CALLBACK (on_key), k);
    g_object_set_data_full (G_OBJECT (c), "dv-keys", k, g_free);
    gtk_widget_add_controller (win, c);
}
