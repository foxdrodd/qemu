#!/usr/bin/env python3
"""Serve build-wasm/web with the cross-origin isolation headers that
SharedArrayBuffer (and therefore Emscripten pthreads) requires, and with
HTTP Range support (single byte ranges), which the lazy disc loader needs.

    contrib/dreamcast-web/serve.py [--port 8086] [--bind 127.0.0.1] [dir]
"""
import argparse
import functools
import http.server
import os
import re


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".cdi": "application/octet-stream",
        ".iso": "application/octet-stream",
    }

    _remaining = None

    def send_head(self):
        self._remaining = None
        rng = self.headers.get("Range")
        path = self.translate_path(self.path)
        if not rng or not os.path.isfile(path):
            return super().send_head()
        m = re.fullmatch(r"\s*bytes=(\d*)-(\d*)\s*", rng)
        if not m or not (m.group(1) or m.group(2)):
            return super().send_head()      # ignore what we can't parse
        f = open(path, "rb")
        size = os.fstat(f.fileno()).st_size
        if m.group(1):
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        else:                               # suffix range: last N bytes
            start = max(0, size - int(m.group(2)))
            end = size - 1
        if start >= size or end < start:
            f.close()
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % size)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        f.seek(start)
        self._remaining = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        if self._remaining is None:
            return super().copyfile(source, outputfile)
        while self._remaining > 0:
            buf = source.read(min(1 << 20, self._remaining))
            if not buf:
                break
            outputfile.write(buf)
            self._remaining -= len(buf)

    def end_headers(self):
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8086)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("dir", nargs="?",
                    default=os.path.join(here, "..", "..", "build-wasm", "web"))
    a = ap.parse_args()
    handler = functools.partial(Handler, directory=os.path.abspath(a.dir))
    srv = http.server.ThreadingHTTPServer((a.bind, a.port), handler)
    print("serving %s on http://%s:%d/" % (os.path.abspath(a.dir), a.bind, a.port))
    srv.serve_forever()


if __name__ == "__main__":
    main()
