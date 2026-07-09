#!/usr/bin/env python3
"""
dc_test_utils.py - shared assertion/parsing helpers for the Dreamcast QEMU
test scripts (dc_integration_test.py, mainline_boot_test.py).
"""
import struct
import wave


# --- assertion helpers -------------------------------------------------------
def need(cond, msg):
    if not cond:
        raise AssertionError(msg)


def has(out, sub, msg=None):
    need(sub in out, msg or ("missing %r in: %r" % (sub, out[:200])))


# --- host-side artifact parsers ---------------------------------------------
def parse_ppm(path):
    """Return (w, h, pixels_bytes) for a binary P6 PPM. Raises on malformed."""
    with open(path, "rb") as f:
        data = f.read()
    need(data[:2] == b"P6", "not a P6 PPM (got %r)" % data[:2])
    idx, vals = 2, []
    while len(vals) < 3:
        while idx < len(data) and data[idx] in b" \t\r\n":
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":       # comment
            while idx < len(data) and data[idx] not in b"\r\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1                                                    # one ws after maxval
    return vals[0], vals[1], data[idx:]


def ppm_distinct_pixels(px, step=997):
    """Sample every `step` pixels; return count of distinct RGB triples."""
    seen = set()
    for i in range(0, len(px) - 3, step * 3):
        seen.add(px[i:i + 3])
        if len(seen) > 1:
            break
    return len(seen)


def wav_peak(path):
    """Peak absolute S16 sample amplitude in a WAV, or -1 if unreadable."""
    try:
        wf = wave.open(path, "rb")
    except (wave.Error, EOFError, FileNotFoundError):
        return -1
    with wf:
        if wf.getsampwidth() != 2:
            return -1
        raw = wf.readframes(wf.getnframes())
    if not raw:
        return 0
    n = len(raw) // 2
    return max(abs(v) for v in struct.unpack("<%dh" % n, raw[:n * 2]))
