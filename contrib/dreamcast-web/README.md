# Dreamcast Linux in the browser

`qemu-system-sh4 -M dreamcast` compiled to WebAssembly (Emscripten, TCG
interpreter), with a small web front end:

| Guest side | Browser side |
|---|---|
| PVR2 framebuffer, VMU LCD | `<canvas>` via `ui/wasm-canvas.c` |
| maple keyboard / mouse | canvas key/mouse events → `qemu_canvas_input()` |
| `ttySC1` (2nd `-serial`, stdio) | xterm.js via the TTY in `dcweb-lib.js` |
| BBA (rtl8139) | slirp user networking (DHCP, no internet from a browser) |
| GD-ROM | `.cdi`/`.iso` fetched into MEMFS |

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
`dcweb-lib.js`, and assembles `build-wasm/web/`. The disc is copied to
`web/disc.cdi`. Without one the page asks for a local file.

## Serving

The page uses SharedArrayBuffer (Emscripten pthreads), so it must be served
with `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` (`serve.py` sets them). Hosts
that can't set headers (e.g. GitHub Pages) need a service-worker shim such as
coi-serviceworker.

URL parameters: `?disc=<url>`, `?nic=none`, `?nolcd`, `?tb=<MiB>` (TB cache,
default 64), `?args=<extra qemu args>`.

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
- Big discs (the 700+ MiB userland CDIs) don't fit the MEMFS approach well.
  They need a lazy, HTTP-range-backed block device.
