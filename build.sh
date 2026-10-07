#!/bin/bash
# DwarvenView build script
# Usage:
#   ./build.sh              -- release build
#   ./build.sh --debug      -- debug build
#   ./build.sh --clean      -- wipe build dir and rebuild
#   meson install -C build --destdir=/tmp/pkg  -- package install

set -e

BUILDTYPE="release"
CLEAN=0

for arg in "$@"; do
  case "$arg" in
    --debug)  BUILDTYPE="debug" ;;
    --clean)  CLEAN=1 ;;
  esac
done

# Slackware multilib PKG_CONFIG path
export PKG_CONFIG_PATH="/usr/lib64/pkgconfig:${PKG_CONFIG_PATH}"

if [ "$CLEAN" = "1" ] || [ ! -d build ]; then
  rm -rf build
  meson setup build \
    --buildtype="$BUILDTYPE" \
    --prefix=/usr
fi

meson compile -C build

echo ""
echo "Build complete: build/dwarvenview  (${BUILDTYPE})"
echo ""
echo "To install:"
echo "  meson install -C build                    # system install"
echo "  meson install -C build --destdir=/tmp/pkg # package staging"
