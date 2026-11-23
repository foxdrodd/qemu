#!/bin/sh
# Build qemu-system-sh4 (with -M dreamcast) for the browser and assemble a
# servable web directory.
#
#   contrib/dreamcast-web/build.sh [disc.cdi|disc.iso]
#
# Needs the "qemu-emsdk-wasm" image, built from
# tests/docker/dockerfiles/emsdk-wasm64-cross.docker (zlib URL fixed to the
# GitHub release tarball).  Output: build-wasm/web/  (serve with serve.py).
set -e

SRC=$(cd "$(dirname "$0")/../.." && pwd)
WEB_SRC="$SRC/contrib/dreamcast-web"
BUILD="$SRC/build-wasm"
OUT="$BUILD/web"
IMAGE=${IMAGE:-qemu-emsdk-wasm}
JOBS=${JOBS:-12}
DISC=${1:-}

mkdir -p "$BUILD"

# Configure once; ninja picks up source changes afterwards.
#
# configs/meson/emscripten.txt is the last cross file and overrides
# c_link_args wholesale, so --extra-ldflags would be lost: set the link args
# with "meson configure" instead (its list plus our JS library).  libslirp
# comes from the bundled subproject and wasm can only link it statically,
# which configure has no switch for.
docker run --rm -v "$SRC:$SRC" -w "$BUILD" -e JOBS="$JOBS" "$IMAGE" sh -ec "
  trap 'chown -R $(id -u):$(id -g) $BUILD' EXIT
  if [ ! -f build.ninja ]; then
    emconfigure ../configure --static --cpu=wasm64 --wasm64-32bit-address-limit \
      --target-list=sh4-softmmu --disable-tools --disable-docs \
      --enable-tcg-interpreter --disable-fdt --disable-slirp
  fi
  LINK=\$(python3 -c \"
import configparser
c = configparser.ConfigParser()
c.read('../configs/meson/emscripten.txt')
args = eval(c['built-in options']['c_link_args'])
args.append('--js-library=$WEB_SRC/dcweb-lib.js')
print(repr(args))\")
  pyvenv/bin/meson configure -Ddefault_library=static -Dslirp=enabled \\
    -Dc_link_args=\"\$LINK\" -Dcpp_link_args=\"\$LINK\" . >/dev/null
  ninja -j\$JOBS qemu-system-sh4.js
"

mkdir -p "$OUT"
cp "$BUILD/qemu-system-sh4.js" "$BUILD/qemu-system-sh4.wasm" "$OUT/"
cp "$WEB_SRC/index.html" "$WEB_SRC/dcweb.js" "$WEB_SRC/keymap.js" "$OUT/"
cp -r "$WEB_SRC/vendor" "$OUT/"
# Option ROM the rtl8139 (BBA) device insists on loading.
mkdir -p "$OUT/rom"
cp "$SRC/pc-bios/efi-rtl8139.rom" "$OUT/rom/"
if [ -n "$DISC" ]; then
  cp "$DISC" "$OUT/disc.${DISC##*.}"
fi
ls -la "$OUT"
