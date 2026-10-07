#pragma once
/*
 * Properties window: shows the metadata gathered by dv_props for one file.
 * Opens immediately with a "reading" indicator and fills in when the worker
 * thread finishes, so a slow tool never freezes the UI. One window at a time;
 * showing it again replaces the previous one.
 */
#include <gtk/gtk.h>

G_BEGIN_DECLS

void dv_props_dialog_show (GtkWindow *parent, const char *path);

/* Called by DvWidget on every load. No-op unless the window is currently
 * open, in which case it refreshes in place for the new path -- this is
 * what makes Properties follow image navigation instead of staying a
 * snapshot of whichever image was on screen when it was opened. */
void dv_props_dialog_notify_path (const char *path);

G_END_DECLS
