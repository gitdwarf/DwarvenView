/* dv_dialog_keys.h -- one keyboard rule for every dialog:
 * Enter accepts (like the OK button), Escape cancels (like the window's X). */
#pragma once
#include <gtk/gtk.h>

typedef void (*DvDialogKeyFn) (GtkWidget *win, gpointer data);

/* accept / cancel may be NULL, meaning "just close the window". Enter on a focused
 * button or link is left alone so it does what that control says. */
void dv_dialog_keys_attach (GtkWidget *win, DvDialogKeyFn accept, DvDialogKeyFn cancel, gpointer data);
