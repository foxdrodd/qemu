#!/usr/bin/env python3
"""Serve build-wasm/web with the cross-origin isolation headers that
SharedArrayBuffer (and therefore Emscripten pthreads) requires.

    contrib/dreamcast-web/serve.py [--port 8086] [--bind 127.0.0.1] [dir]
"""
import argparse
import functools
import http.server
import os


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".cdi": "application/octet-stream",
        ".iso": "application/octet-stream",
    }

    def end_headers(self):
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
