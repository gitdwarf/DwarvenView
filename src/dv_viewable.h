#pragma once
/*
 * dv_viewable -- which files do next, previous and the folder walk stop on?
 *
 * Extensions are never trusted. An image is recognised by its leading bytes.
 * With include_media, anything libav finds audio or video in also counts (a film,
 * a song, a song with cover art). The test only opens the file; it never reads
 * through it, so its cost does not depend on the file size.
 *
 * Archives and documents (zip, cbz, rar, 7z, tar, pdf) are never taken, whatever libav makes of them.
 *
 * This is only about browsing. A file named directly (command line, drag and drop,
 * the Open dialog) is always shown whatever this says.
 */

#include <glib.h>

gboolean dv_file_is_browsable (const char *path, gboolean include_media);

/* TRUE for a zip (cbz, epub), rar, 7z, tar, ar, PDF or old office file, by signature. These hold other
 * files and are never browsed to, and never decoded as if their contents were the file. */
gboolean dv_file_is_archive (const char *path);
