#pragma once

#include <glib.h>
#include "dv_engine.h"
#include "dv_viewport.h"
#include "dv_prefs.h"

/* Output format descriptor */
typedef struct {
    const char *ext;        /* file extension, e.g. "png"  */
    const char *label;      /* menu label,     e.g. "PNG"  */
    gboolean    animated;   /* TRUE: ffmpeg can write animated form  */
    gboolean    still_only; /* TRUE: per-frame output for animations */
} DvExportFormat;

/* Curated format list */
extern const DvExportFormat DV_EXPORT_FORMATS[];
extern const int            DV_EXPORT_NFORMATS;

/* Result of a conversion */
typedef struct {
    gboolean  ok;
    char    **output_paths;   /* NULL-terminated list of output files */
    char     *error_msg;      /* set on failure                       */
    char     *warning_msg;    /* set when output was clamped          */
} DvExportResult;

void dv_export_result_free (DvExportResult *r);

/*
 * Convert source_path to the given format.
 *
 * Animated -> animated format  : one animated output file
 * Animated -> still_only format: one file per frame, _001 _002 ... suffix
 * SVG                          : rendered via librsvg at viewport zoom
 * All other raster             : ffmpeg
 *
 * Output path resolved via dv_prefs_resolve_output rules.
 * Returns heap-allocated result; caller must dv_export_result_free().
 */
DvExportResult *dv_export (const char           *source_path,
                           const DvExportFormat  *fmt,
                           DvEngine              *engine,
                           const DvViewport      *vp,
                           const DvPrefs         *prefs);
