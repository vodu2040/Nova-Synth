#!/usr/bin/env bash
# Builds tools/desktop_snapshot/run/desktop_snapshot on Linux: needs a configured
# Linux_Build (./build_linux.sh --no-install) for the SDK libraries and the
# generated identity header, plus the cairo / X11 / fontconfig dev packages.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."
B="$ROOT/Linux_Build/build"
mkdir -p "$HERE/run"
c++ -std=gnu++20 -O1 -g -Wno-multichar -DOXO_LINUX_EDITOR=1 -DOXO_SNAPSHOT=1 -DSMTG_RENAME_ASSERT=1 -DRELEASE=1 \
  -I"$ROOT" -I"$B/generated" -I"$ROOT/Linux_Build/Source" -I"$ROOT/third_party/vst3sdk" \
  $(pkg-config --cflags cairo x11 fontconfig) \
  "$HERE/main.cpp" "$ROOT/DesktopEditor/DesktopEditor.cpp" "$ROOT/Linux_Build/Source/LinuxEditor.cpp" \
  "$ROOT/Linux_Build/Source/LinuxTrash.cpp" \
  "$B/lib/Release/libsdk.a" "$B/lib/Release/libsdk_common.a" "$B/lib/Release/libbase.a" "$B/lib/Release/libpluginterfaces.a" \
  $(pkg-config --libs cairo x11 fontconfig) -lz -lpthread -ldl -o "$HERE/run/desktop_snapshot"
echo "built $HERE/run/desktop_snapshot"
