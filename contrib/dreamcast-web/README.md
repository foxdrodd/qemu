# Dreamcast Linux in the browser

`qemu-system-sh4 -M dreamcast` compiled to WebAssembly (Emscripten, TCG
interpreter), with a small web front end:

| Guest side | Browser side |
|---|---|
| PVR2 framebuffer, VMU LCD | `<canvas>` via `ui/wasm-canvas.c` |
| maple keyboard / mouse | canvas key/mouse events → `qemu_canvas_input()` |
| `ttySC1` (2nd `-serial`, stdio) | xterm.js via the TTY in `dcweb-lib.js` |
| BBA (rtl8139) | slirp user networking (DHCP, no internet from a browser) |
| GD-ROM | `.cdi`/`.iso` streamed on demand (`block/wasm-http.c`), or a local file copied into MEMFS |

## Build

```sh
# once: the emsdk build image (= tests/docker/dockerfiles/emsdk-wasm64-cross.docker
# with the stale zlib.net URL replaced by the GitHub release tarball)
docker build -t qemu-emsdk-wasm -f contrib/dreamcast-web/emsdk-wasm.docker contrib/dreamcast-web

contrib/dreamcast-web/build.sh /path/to/linux-7.3.1-base-busybox.cdi
contrib/dreamcast-web/serve.py          # http://127.0.0.1:8086/
```

`build.sh` configures `build-wasm/` once (sh4-softmmu, `--cpu=wasm64
--wasm64-32bit-address-limit`, TCI, static slirp subproject), links
`dcweb-lib.js`, and assembles `build-wasm/web/`.

Optimization flags (`WASM_CFLAGS`, `WASM_LDFLAGS`, `WASM_LTO` override them).
Measured on the busybox CDI with a 20k-iteration shell loop:

| flags | boot | shell loop | .wasm |
|---|---|---|---|
| none (meson -O2 -g, no link -O) | 35.4 s | 87.8 s | 28.0 MB |
| link `-O3` | 20.4 s | 45.1 s | 14.6 MB |
| + compile `-O3 -g0` | 21.0 s | 43.2 s | 14.7 MB |
| + `-fno-stack-protector`, `-sMALLOC=mimalloc` (**default**) | 19.3 s | 41.1 s | 14.7 MB |
| + LTO | 21.2 s | 44.5 s | 16.9 MB |

Link-time `-O3` matters most: without it `wasm-opt` never optimizes the
Asyncify-instrumented code. `-msimd128` can't be used, because Binaryen's
memory64 lowering rejects SIMD lane/splat loads. The disc is symlinked as
`web/disc.cdi` (use `rsync -L` to deploy). Without one the page asks for a
local file.

## Disc streaming

A disc given by URL (`web/disc.cdi`, or `?disc=<url>`) is not downloaded up
front. `block/wasm-http.c`, an http(s) block protocol compiled only into
Emscripten builds, fetches 256 KiB chunks with synchronous Range requests
from QEMU's worker threads as the GD-ROM reads them. When access is
sequential it fetches up to 4 chunks per request. Chunks stay in an in-memory
cache (default 512 MiB, oldest evicted first; `?cache=<MiB>`). The full
803 MB `linux-7.3.1-with-userland-muslX.cdi` boots to the mksh prompt in about
30 s after at most about 32 MB of fetches.

The server must answer HEAD with `Accept-Ranges: bytes` and honour
single-range requests (`serve.py`, nginx and GitHub Pages do). Otherwise
the page falls back to a full download. So does `?preload`.

## Serving

The page uses SharedArrayBuffer (Emscripten pthreads), so it must be served
with `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` (`serve.py` sets them). Hosts
that can't set headers (e.g. GitHub Pages) need a service-worker shim such as
coi-serviceworker.

URL parameters: `?disc=<url>`, `?preload`, `?cache=<MiB>`, `?nic=none`,
`?nolcd`, `?tb=<MiB>` (TB cache, default 64), `?args=<extra qemu args>`.

## Notes / gotchas

- `configs/meson/emscripten.txt` overrides `c_link_args` wholesale, so
  `--extra-ldflags` is silently dropped; `build.sh` sets the link args with
  `meson configure` instead.
- Emscripten 4.0.10's `poll()` ignores its timeout. `dcweb-lib.js` wraps it
  so QEMU's main loop sleeps (1 ms slices) instead of spinning.
- xterm-pty does not work here: its poll hook waits for *input* with the
  `-1` timeout Emscripten passes, which blocks QEMU's main loop (and with it
  all guest timers) until a key is pressed.
- The rtl8139 device needs `efi-rtl8139.rom`; it's shipped in `web/rom/`
  and found via `-L /rom`.
- The guest draws the VMU LCD rotated (as it sits in a controller); the page
  rotates the canvas 180°.
- `EM_JS` hands 64-bit C pointers to JS as BigInt (`EM_ASM` converts them);
  `wasm-http.c` wraps them in `Number()`.
- The maple keyboard reports key state once per VSYNC, so `wasm-canvas.c`
  spaces key events at least 40 ms apart. Otherwise a press and release
  between two polls are lost.
- Fetched chunks aren't kept across page loads yet. A persistent cache in
  OPFS or IndexedDB would be the next step.
