gtk4     = dependency('gtk4', version : '>= 4.10')
rsvg     = dependency('librsvg-2.0')
cairo    = dependency('cairo')
glib     = dependency('glib-2.0', version : '>= 2.74')
gobject  = dependency('gobject-2.0')
giounix  = dependency('gio-unix-2.0')
avcodec  = dependency('libavcodec')
avformat = dependency('libavformat')
avutil   = dependency('libavutil')
swscale  = dependency('libswscale')
pixbuf   = dependency('gdk-pixbuf-2.0')
webpdemux = dependency('libwebpdemux')   # animated WebP, which libav cannot decode
m        = meson.get_compiler('c').find_library('m', required : false)

dwarvenview_sources = files(
  'main.c',
  'dv_widget.c',
  'dv_engine.c',
  'dv_surface_tex.c',
  'dv_webp_anim.c',
  'dv_themeicon.c',
  'dv_viewport.c',
  'dv_export.c',
  'dv_wallpaper.c',
  'dv_prefs.c',
  'dv_props.c',
  'dv_props_dialog.c',
  'dv_dialog_keys.c',
  'dv_debug.c',
  'dv_viewable.c',
)

dwarvenview_deps = [gtk4, rsvg, cairo, glib, gobject, giounix,
                    avcodec, avformat, avutil, swscale, pixbuf, webpdemux, m]
