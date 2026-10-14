# DwarvenView

**A fast, clean GTK4 image viewer with crop-and-zoom, high-quality export, and full transform support. Part of the [DwarvenSuite](https://github.com/gitdwarf).**

## Philosophy

- **What you see is what you get** -- export captures exactly what is visible in the viewport
- **No quality compromises** -- spline36 scaling with unsharp on every export
- **No surprises** -- greyed items are unavailable, active items always do what they say
- **Small and fast** -- no Electron, no Python, no bloat. Pure C + GTK4

## What it does

DwarvenView loads images quickly via a progressive resolution ladder, lets you zoom, crop, rotate, flip, and pan -- then exports exactly what you see, at maximum quality, to any common format.

- **Blade Runner style crop-and-zoom** -- middle-click drag to select a region and zoom into it. Pan around with left-drag or the arrow keys (panning covers the whole image, not just the selected region). Export just that region, scaled up with spline36.
- **Mega-image support** -- images that exceed libavcodec's stride limit are automatically decoded via GdkPixbuf fallback. No size limit in practice.
- **Progressive loading** -- placeholder shows immediately, image builds up from low-res to full-res in background. No UI freeze.
- **Audio and video** -- open one directly and DwarvenView shows it: the cover art of an mp3, flac, m4a, mka, wma, ogg or opus; one frame of a video from about 10% in, as thumbnailers do; or your icon theme's music icon for audio with no cover. Nothing is played or read through, so a large file opens at once. Only gif, apng and webp animate, each at the speed the file asks for. Next, previous and Open take images only, unless you turn on **Also browse audio and video files** in Preferences. Archives and documents (zip, cbz, rar, 7z, tar, PDF) are never opened as pictures, even when they hold a film.
- **Window-resize aware** -- image recentres and rescales to fit when you resize the window.

## Features

- **Formats** -- PNG, JPEG, WebP, BMP, TIFF, GIF, SVG, XCursor, and anything ffmpeg can decode
- **Export** -- Convert to PNG/JPEG/WebP/TIFF/BMP/SVG (SVG-to-SVG only); Set as wallpaper; Send to any XDG-registered image app
- **Transforms** -- Rotate 90/180/270, flip horizontal/vertical, full zoom history with undo
- **Slideshow** -- configurable interval (accepts fractions and expressions like `1/30`)
- **Drag and drop** -- drop an image or a folder onto the window to open it
- **Fullscreen** -- F11, Escape (leaves it) or the toolbar toggle button, synced to the menu checkmark. Optionally image only: Preferences can hide the menu, toolbar and status bar while fullscreen.
- **Preferences** -- loading splash, fullscreen image only, pan inversion (L/R, U/D, wheel axes), slideshow interval, right-click menu style (hide or grey unavailable items)
- **Right-click menu** -- context-sensitive, with separators. Fullscreen, zoom/transform, wallpaper/convert/send, properties, help, exit.
- **Properties** -- Ctrl+I (or File / right-click menu) shows file details and full EXIF, XMP and media metadata. DwarvenView does no metadata parsing of its own: it uses the best tool installed, trying exiftool, then exiv2, then mediainfo, then ffprobe, then a built-in summary. The window says which one answered and suggests exiftool if it is missing.
- **Help** -- built-in keyboard reference, accessible from toolbar, menu, and right-click

## Export quality

When you zoom in and export, DwarvenView uses:

```
zscale=W:H:filter=spline36:dither=error_diffusion,unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=0.8
```

spline36 is the highest-quality resampling filter available in ffmpeg/zscale. The unsharp pass restores detail lost during upscaling. This is the same quality tier used in professional video production pipelines.

## Installation

**From source (any distro):**

```bash
git clone https://github.com/gitdwarf/dwarvenview.git
cd dwarvenview
make
sudo make install
```

**Just the executable (no install):**

```bash
./build.sh            # release build
./build.sh --debug    # debug build
./build.sh --clean    # wipe build/ and rebuild
```

A convenience for `./configure && make`: it runs the same meson setup and compile, and also adds `/usr/lib64/pkgconfig` to pkg-config's search path, which Slackware multilib needs. You end up with one self-contained executable, `build/dwarvenview`, that runs where it stands. Copy it anywhere, or leave it in place.

**Staged install (for packaging):**

```bash
make
make install DESTDIR=/tmp/pkg
```

**Slackware (SlackBuild):**

```bash
cd dwarvenview
git archive --format=tar.gz --prefix=dwarvenview-VERSION/ -o slack/dwarvenview-VERSION.tar.gz HEAD
cd slack
sudo ./dwarvenview.SlackBuild
sudo installpkg /tmp/dwarvenview-VERSION-ARCH-1_dwarven.tgz
```

Replace `VERSION` with the value in `dwarvenview.info`, which the SlackBuild reads
(set `VERSION=x.y.z` in the environment to override). The tarball must sit next to
the SlackBuild and its top-level folder must be `dwarvenview-VERSION`. Any release
tarball laid out that way works in place of the `git archive` line.

**Manual meson build:**

```bash
meson setup build --buildtype=release --prefix=/usr
meson compile -C build
meson install -C build
```

## Build dependencies

- GTK4 >= 4.10 (GtkFileDialog and GtkAlertDialog)
- librsvg2
- libavcodec / libavformat / libavutil / libswscale (ffmpeg)
- gdk-pixbuf-2.0
- libwebpdemux (libwebp), for animated WebP, which ffmpeg cannot decode
- cairo
- glib-2.0 >= 2.74 (g_idle_add_once)

## Runtime dependencies

- ffmpeg (CLI, for zscale export pipeline; its `ffprobe` is the last-resort metadata tool)
- GTK4 runtime

Optional, for the Properties window. The best one installed is used automatically:

- exiftool -- full EXIF, IPTC, XMP and maker notes (recommended)
- exiv2 -- EXIF, IPTC and XMP in images
- mediainfo -- container and codec detail

## Usage

```bash
dwarvenview                    # open file picker
dwarvenview image.png          # open a specific file
dwarvenview ~/Photos/          # open a folder
```

## Keyboard shortcuts

| Key | Action |
|---|---|
| Page Up / Page Down | Previous / Next image |
| Arrow keys | Pan |
| Ctrl+O | Open file |
| Ctrl+Shift+O | Open folder |
| Ctrl+I | Properties (EXIF and file details) |
| Ctrl+Scroll | Zoom in/out (mouse-centred) |
| Ctrl++ / Ctrl+- | Zoom in / out |
| Ctrl+0 | Reset zoom to fit |
| Ctrl+1 | Actual size (100%) |
| Middle-button drag | Blade Runner style crop-and-zoom |
| Left-button drag | Pan |
| R | Rotate 90° clockwise |
| Shift+R | Rotate 90° counter-clockwise |
| L | Rotate 90° counter-clockwise |
| Shift+L | Rotate 90° clockwise |
| H | Flip horizontal |
| V | Flip vertical |
| Space | Toggle slideshow |
| F11 | Toggle fullscreen |
| Esc | Leave fullscreen (after undoing any zoom steps) |

## Blade Runner style crop-and-zoom

Middle-click and drag a rectangle over any part of the image. DwarvenView zooms into that exact region. You can then:

- Pan around with left-drag or the arrow keys -- panning follows the whole image, not just the selected region
- Export -- the output is exactly what is visible, scaled up with spline36
- Reset zoom (Ctrl+0) to return to fit-to-window

This is the "Enhance!" workflow from the film -- zoom into a detail of an image, export it at full quality.

## Troubleshooting

If DwarvenView crashes or fails on a particular image, turn on the debug log: Help, About DwarvenView, then tick "Log image decode calls". It takes effect immediately.

- The log is `~/.cache/dwarvenview/debug.log` (under `$XDG_CACHE_HOME` if you set it).
- It records every libavcodec, librsvg and GdkPixbuf call around loading an image, immediately before and after the call.
- Every line is written to disk before the call returns, so after a crash the last line is the call that was running. Send that line with your report.
- It is off by default. Unticking the box deletes the log straight away, so nothing lingers.

## Tests

The tests are not part of this source archive, since most people never need them. They come as a separate download, `dwarvenview-tests-VERSION.zip`. Unpack it from the directory that holds the source folder, so that it adds a `tests/` directory to the source, then:

```bash
meson setup build -Dtests=true
meson test -C build --print-errorlogs
```

`tests/README.md` inside the archive lists what each test covers and what it needs.

## Directory layout

```
dwarvenview/
  src/
    main.c                 Window, menus, actions, file dialogs
    dv_widget.c/.h         The image widget: drawing, input, keys, overlays
    dv_engine.c/.h         Decoding and progressive loading (libav, librsvg, gdk-pixbuf)
    dv_viewport.c/.h       Zoom, pan and fit calculations
    dv_export.c/.h         Export and transforms (via ffmpeg)
    dv_wallpaper.c/.h      Set as wallpaper
    dv_props.c/.h          Properties data: external tool chain with built-in fallback
    dv_props_dialog.c/.h   Properties window
    dv_prefs.c/.h          Saved preferences
    dv_viewable.c/.h       Which files next, previous and Open stop on (images, or audio and video too)
    dv_themeicon.c/.h      A named icon from the current icon theme, as a texture
    dv_webp_anim.c/.h      Animated WebP through libwebp's animation decoder
    dv_surface_tex.c/.h    Cairo surface to GdkTexture (shared by the engine and dv_themeicon)
    dv_debug.c/.h          Optional crash-durable debug log
    dv_icon.h              Embedded SVG icon, registered with the icon theme at startup
    dv_placeholder.h       Embedded placeholder SVG for the large-image loading state
    dwarven4gtk3restore.h  Restores GTK3 behaviours from settings.ini (single-header library)
    dwarvenview-icon.svg   Application icon (installed by data/meson.build)
    meson.build            Source list and dependencies (the target is in the top-level file)
  data/
    dwarvenview.desktop    Desktop entry
    dwarvenview.metainfo.xml.in  AppStream metadata (version and date filled in by meson)
    meson.build            Desktop/icon install rules
  slack/                   Slackware SlackBuild and slack-desc
  .src2pkg                 src2pkg build settings
  meson.build              Top-level build definition
  meson_options.txt        Build options (-Dtests, for the separate test archive)
  configure                Autoconf-style wrapper around meson setup
  Makefile                 Thin wrapper for make/make install
  build.sh                 Convenience build script (see Installation)
  dwarvenview.info         Version and package metadata (single source of truth)
  CHANGELOG.md             What changed in each version
  LICENSE
```

## Part of DwarvenSuite

All tools follow the same philosophy: small, fast, correct, as few dependencies as possible.

## Author

thedwarf -- gitdwarf

## Support / Tip Jar

If you find DwarvenView useful, you can support the project:

[![Donate via PayPal](https://img.shields.io/badge/Donate-PayPal-blue?logo=paypal)](https://www.paypal.com/paypalme/gitdwarf)

## License

GPL-2.0-or-later: the GNU General Public License, version 2 or (at your option) any later version. See the LICENSE file for details.
