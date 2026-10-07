#pragma once
/*
 * dv_props -- file properties and EXIF/media metadata.
 *
 * DwarvenView does not parse metadata itself. It drives the best tool that
 * is installed and degrades gracefully, the same way image decoding falls
 * back from librsvg to ffmpeg to gdk-pixbuf:
 *
 *   1. exiftool   (EXIF, IPTC, XMP, maker notes, nearly every format)
 *   2. exiv2      (EXIF, IPTC, XMP in images)
 *   3. mediainfo  (containers, codecs, streams)
 *   4. ffprobe    (ships with ffmpeg, which DwarvenView already needs)
 *   5. built-in   (GIO file info + gdk-pixbuf header sniff, always works)
 *
 * The first backend that produces something useful wins. "Useful" is judged
 * per backend, not by exit code: exiftool and mediainfo both exit 0 on files
 * they could not really read.
 */

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
    char *key;
    char *value;
} DvPropRow;

typedef struct {
    char      *name;
    GPtrArray *rows;      /* DvPropRow* */
} DvPropGroup;

typedef struct {
    char       *path;     /* absolute path that was inspected */
    const char *source;   /* which backend produced this (static string) */
    char       *hint;     /* optional advice for the user, or NULL */
    GPtrArray  *groups;   /* DvPropGroup*, in display order */
} DvProps;

/* Gather properties on a worker thread. Never blocks the main loop.
 * A watchdog stops slow tools after a timeout and falls back to the
 * built-in backend. Finish with dv_props_fetch_finish(). */
void      dv_props_fetch_async  (const char         *path,
                                 GAsyncReadyCallback cb,
                                 gpointer            user_data);
DvProps  *dv_props_fetch_finish (GAsyncResult       *res);

/* Blocking version, for tests and non-GUI callers. No timeout. */
DvProps  *dv_props_fetch_sync   (const char *path);

/* Plain-text rendering ("[Group]" headings, "key: value" rows) for Copy. */
char     *dv_props_to_text      (const DvProps *props);

void      dv_props_free         (DvProps *props);

G_END_DECLS
