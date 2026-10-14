# Changelog

All notable changes to DwarvenView. Newest first. Dates are YYYY-MM-DD.

## 0.5.58 - 2026-10-15
### Added
- This changelog. It is installed with the README and LICENSE in the SlackBuild package.

## 0.5.57 - 2026-10-14
### Changed
- Help window: one blank line above the text and two below it.

## 0.5.56 - 2026-10-14
### Added
- Escape leaves fullscreen. It first cancels a crop box and steps a zoom back, as before; once there is nothing left to undo it leaves fullscreen.
- Preferences, Viewing, "Fullscreen: Image only": hides the menu, toolbar and status bar while fullscreen. Off by default. Keyboard shortcuts keep working.
### Changed
- The Preferences window is a little taller to fit the new option.

## 0.5.55 - 2026-10-14
### Changed
- One keyboard rule for every dialog: Enter accepts (like OK), Escape cancels (like the window's X). About, Help and Properties close on either key; Preferences saves on Enter and discards on Escape.
### Fixed
- Enter in Preferences with the Cancel button focused saved instead of cancelling.

## 0.5.54 - 2026-10-14
### Added
- Help window: a Troubleshooting note pointing at Help, About, "Log image decode calls".

## 0.5.53 - 2026-10-12
### Fixed
- About window: Escape did not close it, Enter selected the text, and Space did not toggle the debug-log checkbox. Focus now starts on the checkbox.

## 0.5.52 - 2026-10-07
### Fixed
- An SVG that is valid XML but draws nothing, or a corrupt SVG, showed a blank screen or text on black. Both now show the placeholder graphic and "Not a valid image file".

## 0.5.51 - 2026-10-07
### Fixed
- After viewing any SVG, the placeholder graphic (both "Loading..." and "Not a valid image file") never appeared again until restart.

## 0.5.50 - 2026-10-07
### Fixed
- With the loading splash turned off, or delayed, a file that is not an image showed only the text on black. A failed load now always shows the placeholder graphic.

## 0.5.49 - 2026-10-07
### Fixed
- With "Also browse audio and video files" on, an archive holding a film (zip, cbz, tar) was opened as if it were a video. Archives and documents are never shown as pictures.

## 0.5.48 - 2026-10-07
### Added
- Ctrl+Left and Ctrl+Right browse to the previous and next file, like Page Up and Page Down.
### Changed
- Preferences are split into tabs (Viewing, Browsing, Controls, Saving) so the window fits a 720p screen.
### Fixed
- With "Also browse audio and video files" on, PDFs and cbz files were treated as media. They are not.

## 0.5.47 - 2026-10-06
### Added
- Animated WebP plays. It needs libwebpdemux (part of libwebp); ffmpeg itself cannot decode animated WebP.
### Fixed
- Animations ran at a fixed speed. GIF, APNG and WebP now use the delay each frame asks for.

## 0.5.46 - 2026-10-06
### Added
- Preferences, Browsing, "Also browse audio and video files" (off by default). Off: Open and next/previous take images only. On: they take audio and video too.
- Audio with no cover art opens showing your icon theme's music icon instead of failing. A file named directly is always shown.
### Changed
- Open File's file filter respects the new option.

## 0.5.45 - 2026-10-06
### Fixed
- An mp3 with cover art opened but next/previous skipped it.
- A large video file (160 MB mkv) hung DwarvenView: every frame was decoded into memory like a GIF. A video now shows one frame, taken from about 10% in, and opens at once whatever its size.

## 0.5.44 - 2026-10-05
### Added
- README: build.sh described as a convenience alternative to configure and make, and how to run the single self-contained executable without installing.

## 0.5.43 - 2026-10-05
### Changed
- The tests ship as a separate archive (dwarvenview-tests-VERSION.zip), not in the source archive.
- README licence wording now says GPL-2.0-or-later, matching the About window and the metainfo file.

## 0.5.42 - 2026-10-05
### Added
- Properties: exiv2 as a metadata source (order: exiftool, exiv2, mediainfo, ffprobe, built-in).
- AppStream metainfo file (data/dwarvenview.metainfo.xml.in).
- A test suite (meson test with -Dtests=true) and a README Troubleshooting section.
### Security
- The last use of popen (wallpaper encoder check) is gone; ffmpeg is run with an argument list, never through a shell.

## 0.5.41 - 2026-10-04
### Changed
- The minimum versions are now measured and enforced: GTK 4.10 and GLib 2.74.
- README directory layout rewritten from the real tree.

## 0.5.40 - 2026-10-04
### Fixed
- Engine thread lifecycle: a load in progress no longer shares or resets a cancel flag with a newer load, and loading threads can no longer touch the engine after it is freed (a use-after-free).

## 0.5.39 - 2026-10-04
### Fixed
- Valid SVG files showed "Not a valid image file".
- The HDR flag carried over from one image to the next.

## 0.5.38 - 2026-10-03
### Fixed
- Crash when moving between certain images (a JPEG after a PNG, or the reverse): the colour conversion wrote 16 to 24 bytes past the end of its buffer.
- Size arithmetic in the image engine now uses size_t.

## 0.5.31 to 0.5.37 - before 2026-10-03
### Added
- Properties window (Ctrl+I, File menu, right-click menu); it follows the image as you browse.
- Crash-durable debug log (Help, About, "Log image decode calls").
- Keys: R, Shift+R, L, Shift+L (rotate), H, V (flip), Ctrl+Shift+O (open folder).
### Changed
- The About window is a plain window so it can hold the debug-log checkbox.
- Open File filter defaults to image files, with "All files" one click away.
### Fixed
- A file that failed every decoder left the window stuck on "Loading..." forever; it now says "Not a valid image file".
- GIF to GIF export.
- The About window's logo.
### Security
- Export no longer builds shell command lines: filenames with shell characters could run commands. All ffmpeg calls use argument lists.
- Predictable temporary file names are gone (a symlink could make an export overwrite or delete other files).
