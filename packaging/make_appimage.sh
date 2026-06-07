#!/usr/bin/env bash
# Build a Qt6 Linux AppImage for GP32emu.
# Requires Qt6 qmake, a compiler, linuxdeploy, and linuxdeploy-plugin-qt. The
# linuxdeploy tools are downloaded into tools/appimage on first use unless
# LINUXDEPLOY and LINUXDEPLOY_PLUGIN_QT point to existing executables.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
ARCH="$(uname -m)"
VERSION="${VERSION:-$(date +%Y.%m.%d)}"
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"
TOOLS="$ROOT/tools/appimage"
APP=GP32emu
export APPIMAGE_EXTRACT_AND_RUN=1

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

find_qmake() {
    if [ -n "${QMAKE:-}" ] && command -v "$QMAKE" >/dev/null 2>&1; then echo "$QMAKE"; return; fi
    for q in qmake6 /usr/lib/qt6/bin/qmake6 /usr/lib64/qt6/bin/qmake6 qmake; do
        if command -v "$q" >/dev/null 2>&1; then
            ver="$($q -query QT_VERSION 2>/dev/null || true)"
            case "$ver" in 6.*) echo "$q"; return;; esac
        fi
    done
    die "no Qt6 qmake found; set QMAKE=/path/to/qmake6"
}

QMAKE="$(find_qmake)"
say "Qt $($QMAKE -query QT_VERSION) via $QMAKE"
rm -rf "$BUILD"; mkdir -p "$BUILD"
( cd "$BUILD" && "$QMAKE" "$ROOT/GP32emu.pro" CONFIG+=release && make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" )
[ -x "$BUILD/$APP" ] || die "build did not produce $BUILD/$APP"

rm -rf "$APPDIR"
install -Dm755 "$BUILD/$APP" "$APPDIR/usr/bin/$APP"
install -Dm644 "$HERE/gp32emu.desktop" "$APPDIR/usr/share/applications/gp32emu.desktop"
install -Dm644 "$ROOT/resources/gp32emu.png" "$APPDIR/usr/share/icons/hicolor/32x32/apps/gp32emu.png"
cp "$ROOT/resources/gp32emu.png" "$APPDIR/gp32emu.png"

mkdir -p "$TOOLS"
fetch() {
    local url="$1" dest="$2"
    [ -x "$dest" ] && return 0
    say "Downloading $(basename "$dest")"
    if command -v curl >/dev/null 2>&1; then curl -fL# "$url" -o "$dest"
    elif command -v wget >/dev/null 2>&1; then wget -q --show-progress "$url" -O "$dest"
    else die "need curl or wget to download $url"; fi
    chmod +x "$dest"
}

LINUXDEPLOY="${LINUXDEPLOY:-$TOOLS/linuxdeploy-$ARCH.AppImage}"
LINUXDEPLOY_PLUGIN_QT="${LINUXDEPLOY_PLUGIN_QT:-$TOOLS/linuxdeploy-plugin-qt-$ARCH.AppImage}"
if [ ! -x "$LINUXDEPLOY" ]; then fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" "$LINUXDEPLOY"; fi
if [ ! -x "$LINUXDEPLOY_PLUGIN_QT" ]; then fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" "$LINUXDEPLOY_PLUGIN_QT"; fi

export PATH="$(dirname "$LINUXDEPLOY_PLUGIN_QT"):$PATH"
export QMAKE
export OUTPUT="$APP-$VERSION-$ARCH.AppImage"
export VERSION
say "Bundling Qt6 and packing AppImage"
"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/$APP" \
    --desktop-file "$APPDIR/usr/share/applications/gp32emu.desktop" \
    --icon-file "$ROOT/resources/gp32emu.png" \
    --plugin qt \
    --output appimage

[ -f "$ROOT/$OUTPUT" ] || mv -f "$OUTPUT" "$ROOT/$OUTPUT"
chmod +x "$ROOT/$OUTPUT"
say "Done: $ROOT/$OUTPUT"
