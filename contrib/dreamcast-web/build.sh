#!/bin/sh
# Build qemu-system-sh4 (with -M dreamcast) for the browser and assemble a
# servable web directory.
#
#   contrib/dreamcast-web/build.sh [disc.cdi|disc.iso]   (symlinked as web/disc.*)
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
# Extra compile / link flags on top of configs/meson/emscripten.txt.
# Measured (busybox CDI, shell-loop benchmark, Chromium): link-time -O3 is the
# big one (~1.9x: it lets wasm-opt clean up after the Asyncify transform);
# compile -O3 -g0, no stack protector and mimalloc add ~10%.  -msimd128 breaks
# wasm-opt's memory64 lowering; LTO was slightly slower and bigger.
WASM_CFLAGS=${WASM_CFLAGS--O3 -g0 -fno-stack-protector}
WASM_LDFLAGS=${WASM_LDFLAGS--O3 -sMALLOC=mimalloc}
WASM_LTO=${WASM_LTO:-false}
DISC=${1:-}

mkdir -p "$BUILD"

# Configure once; ninja picks up source changes afterwards.
#
# configs/meson/emscripten.txt is the last cross file and overrides
# c_link_args wholesale, so --extra-ldflags would be lost: set the link args
# with "meson configure" instead (its list plus our JS library).  libslirp
# comes from the bundled subproject and wasm can only link it statically,
# which configure has no switch for.
docker run --rm -v "$SRC:$SRC" -w "$BUILD" -e JOBS="$JOBS" \
    -e WASM_CFLAGS="$WASM_CFLAGS" -e WASM_LDFLAGS="$WASM_LDFLAGS" -e WASM_LTO="$WASM_LTO" "$IMAGE" sh -ec "
  trap 'chown -R $(id -u):$(id -g) $BUILD' EXIT
  if [ ! -f build.ninja ]; then
    emconfigure ../configure --static --cpu=wasm64 --wasm64-32bit-address-limit \
      --target-list=sh4-softmmu --disable-tools --disable-docs \
      --enable-tcg-interpreter --disable-fdt --disable-slirp
  fi
  eval \$(python3 -c \"
import configparser, shlex
c = configparser.ConfigParser()
c.read('../configs/meson/emscripten.txt')
opts = c['built-in options']
cargs = eval(opts['c_args']) + shlex.split('\$WASM_CFLAGS')
link = eval(opts['c_link_args']) + shlex.split('\$WASM_LDFLAGS')
link.append('--js-library=$WEB_SRC/dcweb-lib.js')
print('CARGS=' + shlex.quote(repr(cargs)) + ' LINK=' + shlex.quote(repr(link)))\")
  pyvenv/bin/meson configure -Ddefault_library=static -Dslirp=enabled \\
    -Db_lto=\$WASM_LTO \\
    -Dc_args=\"\$CARGS\" -Dcpp_args=\"\$CARGS\" \\
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
  # Symlinked, not copied: the page streams it with Range requests, so the
  # 700+ MiB userland discs are fine.  (Use rsync -L to deploy elsewhere.)
  rm -f "$OUT"/disc.cdi "$OUT"/disc.iso
  ln -s "$(realpath "$DISC")" "$OUT/disc.${DISC##*.}"
fi
ls -la "$OUT"
