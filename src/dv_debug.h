#pragma once
/*
 * dv_debug -- crash-durable debug logging.
 *
 * Same idea as DwarvenThemer4GTK's push_x11.py debug log: OFF by default,
 * a checkbox in About turns it on immediately (no restart, no Apply step),
 * and every line is fsync()'d before the call returns, so if the process
 * dies mid-operation (a raw libav or librsvg call on an untrusted file is
 * exactly the kind of thing that can segfault) the log already has the
 * last thing that was happening on disk. Turning it off deletes the file
 * immediately, so nothing stale lingers once you are done chasing a bug.
 *
 * Disabled is a true no-op: dv_debug_log() checks one flag and returns,
 * no file is ever opened or written.
 */

#include <glib.h>

G_BEGIN_DECLS

gboolean dv_debug_logging_enabled (void);
void     dv_debug_set_logging_enabled (gboolean enabled);

/* Path of the log file, for display in the UI. Caller must g_free(). */
char    *dv_debug_log_path (void);

void     dv_debug_log (const char *fmt, ...) G_GNUC_PRINTF (1, 2);

G_END_DECLS
