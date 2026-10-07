#include "dv_viewable.h"
#include "dv_debug.h"

#include <libavformat/avformat.h>
#include <stdio.h>
#include <string.h>

/* Image signatures, read from the first bytes of the file. */
static gboolean
has_image_signature (const char *path)
{
    FILE *f = fopen (path, "rb");
    if (!f) return FALSE;

    guint8 magic[12] = {0};            /* a short file stays zero padded */
    size_t got = fread (magic, 1, sizeof magic, f);
    fclose (f);
    if (got == 0) return FALSE;

    /* Xcursor */
    if (memcmp (magic, "Xcur", 4) == 0) return TRUE;
    /* PNG */
    if (magic[0]==0x89 && magic[1]=='P' && magic[2]=='N' && magic[3]=='G') return TRUE;
    /* JPEG */
    if (magic[0]==0xff && magic[1]==0xd8) return TRUE;
    /* GIF */
    if (memcmp (magic, "GIF8", 4) == 0) return TRUE;
    /* WebP (RIFF....WEBP) -- needs 8 bytes */
    if (memcmp (magic, "RIFF", 4) == 0 &&
        memcmp (magic + 4, "WEBP", 4) == 0) return TRUE;
    /* BMP */
    if (magic[0]=='B' && magic[1]=='M') return TRUE;
    /* TIFF LE/BE */
    if ((magic[0]=='I' && magic[1]=='I' && magic[2]=='*' && magic[3]==0) ||
        (magic[0]=='M' && magic[1]=='M' && magic[2]==0 && magic[3]=='*')) return TRUE;
    /* AVIF/HEIF: an ftyp box whose brand is an image brand. Other brands (isom, mp42, M4A , qt)
     * are audio or video, which are media, not images. */
    if (memcmp (magic + 4, "ftyp", 4) == 0) {
        static const char *const IMAGE_BRANDS[] = { "avif", "avis", "heic", "heix", "heim", "heis",
                                                    "hevc", "hevm", "hevs", "mif1", "msf1", NULL };
        for (int i = 0; IMAGE_BRANDS[i]; i++)
            if (memcmp (magic + 8, IMAGE_BRANDS[i], 4) == 0) return TRUE;
        return FALSE;
    }

    /* SVG: XML with an <svg tag -- read 512 bytes to find it.
     * Rejects other XML formats (.dms, .html, etc). */
    if (magic[0] == '<' ||
        (magic[0]==0xef && magic[1]==0xbb && magic[2]==0xbf)) {
        FILE *sf = fopen (path, "rb");
        if (!sf) return FALSE;
        char buf[512] = {0};
        int  n        = (int) fread (buf, 1, 511, sf);
        fclose (sf);
        for (int i = 0; i < n - 3; i++) {
            if (buf[i]   == '<' &&
                (buf[i+1]=='s'||buf[i+1]=='S') &&
                (buf[i+2]=='v'||buf[i+2]=='V') &&
                (buf[i+3]=='g'||buf[i+3]=='G')) return TRUE;
        }
        return FALSE;
    }

    return FALSE;
}

/* Archives and documents hold other files; they are not themselves pictures, films or songs.
 * libav does not know that: it scans stored (uncompressed) data for a media signature, so a zip,
 * a tar or a cbz that holds an mpeg looks like an mpeg. Only the formats whose contents stay
 * readable are listed; compressed ones (gz, xz, zstd) cannot fool libav. */
gboolean
dv_file_is_archive (const char *path)
{
    FILE *f = fopen (path, "rb");
    if (!f) return FALSE;
    guint8 b[512] = {0};
    size_t got = fread (b, 1, sizeof b, f);
    fclose (f);
    if (got < 4) return FALSE;

    static const struct { const char *magic; size_t len; } SIGS[] = {
        { "PK\x03\x04", 4 }, { "PK\x05\x06", 4 }, { "PK\x07\x08", 4 },   /* zip, cbz, epub, docx, jar */
        { "Rar!\x1a\x07", 6 },                                          /* rar, cbr */
        { "7z\xbc\xaf\x27\x1c", 6 },                                    /* 7z, cb7 */
        { "%PDF-", 5 },                                                /* pdf */
        { "\xd0\xcf\x11\xe0", 4 },                                    /* old office documents */
        { "!<arch>\n", 8 },                                            /* ar, deb */
    };
    for (guint i = 0; i < G_N_ELEMENTS (SIGS); i++)
        if (got >= SIGS[i].len && memcmp (b, SIGS[i].magic, SIGS[i].len) == 0) return TRUE;
    return got >= 262 && memcmp (b + 257, "ustar", 5) == 0;               /* tar */
}

/* libav will read almost anything as something. These demuxers are guesses at a bare stream or at
 * text art, and they claim PDFs, zip-based comics (cbz, epub), and arbitrary binaries. None of them is
 * audio or video worth browsing to. */
static gboolean
is_lookalike_format (const AVInputFormat *f)
{
    static const char *const LOOKALIKES[] = { "bin", "tty", "ansi", "xbin", "idf", "artworx",
                                              "mjpeg", "image2", NULL };
    for (int i = 0; LOOKALIKES[i]; i++)
        if (strcmp (f->name, LOOKALIKES[i]) == 0) return TRUE;
    return g_str_has_suffix (f->name, "_pipe");
}

static gboolean
has_av_stream (const AVFormatContext *f)
{
    for (unsigned i = 0; i < f->nb_streams; i++) {
        enum AVMediaType t = f->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO) return TRUE;
    }
    return FALSE;
}

/* Does libav find audio or video in the file? A film, a song, or a song with cover art (libav
 * reports the cover as a video stream). Only a confident match counts: a low probe score or a
 * look-alike format is not media. Most containers list their streams as soon as they are opened;
 * flv and mpeg do not, so for those one small bounded look (never the whole file) is taken. */
static gboolean
has_media_stream (const char *path)
{
    char            *abs   = g_canonicalize_filename (path, NULL);  /* never a libav protocol name */
    AVFormatContext *fmt   = NULL;
    gboolean         found = FALSE;

    dv_debug_log ("browsable: avformat_open_input %s", abs);
    int rc = avformat_open_input (&fmt, abs, NULL, NULL);
    dv_debug_log ("browsable: avformat_open_input returned %d", rc);

    if (rc >= 0) {
        if (fmt->probe_score > AVPROBE_SCORE_RETRY && !is_lookalike_format (fmt->iformat)) {
            found = has_av_stream (fmt);
            if (!found && fmt->nb_streams == 0) {
                fmt->probesize = 256 * 1024;
                fmt->max_analyze_duration = 500000;
                if (avformat_find_stream_info (fmt, NULL) >= 0)
                    found = has_av_stream (fmt);
            }
        }
        dv_debug_log ("browsable: format %s score %d, media %s", fmt->iformat->name,
                      fmt->probe_score, found ? "found" : "not found");
        avformat_close_input (&fmt);
    }
    g_free (abs);
    return found;
}

gboolean
dv_file_is_browsable (const char *path, gboolean include_media)
{
    if (!g_file_test (path, G_FILE_TEST_IS_REGULAR)) return FALSE;
    if (has_image_signature (path)) return TRUE;
    return include_media && !dv_file_is_archive (path) && has_media_stream (path);
}
