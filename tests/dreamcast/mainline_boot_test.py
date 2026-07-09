#!/usr/bin/env python3
"""
mainline_boot_test.py - lightweight boot-regression test for the QEMU
"-M dreamcast" machine against a *plain upstream* Linux kernel (linux-next)
and a minimal Buildroot busybox rootfs — no dreamcast-linux fork, no custom
cross-toolchain, no CDI/self-boot.

Answers one question, cheaply and continuously: did a QEMU change (or an
upstream kernel change) break booting Linux / break a driver on -M dreamcast?
Checks dmesg for panics/oops, the GD-ROM+ISO9660 root mount, and the PVR
framebuffer. Deliberately does *not* cover AICA/VMU/gdb — those stay in
dc_integration_test.py against the full dreamcast-linux distro.

The busybox rootfs's own init (tests/dreamcast/rootfs-overlay/etc/init.d/
S99dc-ci-check) prints one marker line ("CI_RESULT: PASS"/"FAIL") on the
console after its own self-checks — no login/getty round-trip needed, so
this doesn't require an interactive shell session.

    python3 mainline_boot_test.py --kernel vmlinux --iso rootfs.iso
    python3 mainline_boot_test.py --tap    # TAP13 output for CI consumption

Config resolves from env ($DC_QEMU, $DC_KERNEL, $DC_ISO); see --help.
"""
import argparse
import os
import sys
import tempfile
import time
import traceback

HARNESS = os.environ.get("DC_HARNESS", os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HARNESS)
try:
    from dcboot import DCBoot
except ImportError:
    sys.exit("cannot import dcboot from %r; set $DC_HARNESS to its scripts/ dir"
             % HARNESS)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dc_test_utils import need, has, parse_ppm, ppm_distinct_pixels

# --- tiny test runner (same shape as dc_integration_test.py) ----------------
PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"
_C = {PASS: "\033[32m", FAIL: "\033[31m", SKIP: "\033[33m", "z": "\033[0m"}


class Results:
    def __init__(self, color=True):
        self.rows = []
        self.color = color and sys.stdout.isatty()

    def add(self, name, status, msg="", dur=0.0):
        self.rows.append((name, status, msg, dur))
        tag = ("%s%s%s" % (_C[status], status, _C["z"])) if self.color else status
        line = "  %s  %s" % (tag, name)
        if msg:
            line += "  — %s" % msg
        if dur:
            line += "  (%.1fs)" % dur
        print(line, flush=True)

    def check(self, name, fn):
        t0 = time.time()
        try:
            r = fn()
            if r is False:
                self.add(name, FAIL, "returned False", time.time() - t0)
            else:
                self.add(name, PASS, "", time.time() - t0)
        except AssertionError as e:
            self.add(name, FAIL, str(e), time.time() - t0)
        except Exception as e:
            self.add(name, FAIL, "%s: %s" % (type(e).__name__, e), time.time() - t0)

    def n(self, status):
        return sum(1 for r in self.rows if r[1] == status)

    def summary(self):
        print("\n%d passed, %d failed, %d skipped" %
              (self.n(PASS), self.n(FAIL), self.n(SKIP)))
        return 1 if self.n(FAIL) else 0

    def tap(self):
        print("TAP version 13")
        print("1..%d" % len(self.rows))
        for i, (name, status, msg, _d) in enumerate(self.rows, 1):
            if status == SKIP:
                print("ok %d - %s # SKIP %s" % (i, name, msg))
            elif status == PASS:
                print("ok %d - %s" % (i, name))
            else:
                print("not ok %d - %s%s" % (i, name, (" # " + msg) if msg else ""))
        return 1 if self.n(FAIL) else 0

    def write_github_summary(self, cfg):
        """Append a markdown results table to $GITHUB_STEP_SUMMARY, if set —
        no-op locally (that env var is only present inside a GitHub Actions
        job)."""
        path = os.environ.get("GITHUB_STEP_SUMMARY")
        if not path:
            return
        icon = {PASS: "✅", FAIL: "❌", SKIP: "⚠️"}
        lines = ["## Dreamcast mainline boot-regression test", ""]
        if cfg.kernel_sha:
            lines.append("**linux-next:** `%s`  " % cfg.kernel_sha)
        if getattr(cfg, "uname", None):
            lines.append("**uname -a:** `%s`  " % cfg.uname)
        lines += ["", "| Check | Result | Details |", "| --- | --- | --- |"]
        for name, status, msg, _d in self.rows:
            lines.append("| %s | %s %s | %s |" %
                         (name, icon[status], status, msg.replace("|", "\\|") if msg else ""))
        lines += ["", "**%d passed, %d failed, %d skipped**" %
                  (self.n(PASS), self.n(FAIL), self.n(SKIP))]
        with open(path, "a") as f:
            f.write("\n".join(lines) + "\n")


# ============================================================================
def scenario_boot(cfg, res):
    """Boot mainline vmlinux + busybox ISO9660 rootfs; check dmesg/root/fb."""
    if not os.path.exists(cfg.kernel):
        res.add("mainline boot", FAIL, "kernel missing: %s" % cfg.kernel)
        return
    if not os.path.exists(cfg.iso):
        res.add("mainline boot", FAIL, "rootfs iso missing: %s" % cfg.iso)
        return

    # The rootfs's own init prints this once its self-checks are done — treat
    # it as the "prompt" DCBoot.boot() waits for. shell="" makes the second
    # (post-prompt) wait a no-op: there's no interactive shell/getty to reach.
    dc = DCBoot(kernel=cfg.kernel, drives=[cfg.iso], net=cfg.net, qemu=cfg.qemu,
               prompt="CI_RESULT:", shell="")
    try:
        try:
            dc.boot(timeout=cfg.boot_timeout)
        except Exception as e:
            # Two different logs, two different failure modes: dc.buf is the
            # *guest* boot console (filled by the reader thread regardless of
            # whether the wait succeeded) — empty means QEMU never sent any
            # guest output. dc.log is QEMU's *own* stdout/stderr (crashes,
            # "unsupported machine option", etc.) — that's the one that
            # explains *why* if dc.buf came back empty.
            console = bytes(dc.buf).decode("latin1", "replace")
            res.add("mainline boot: reaches init self-check", FAIL, str(e))
            print("\n----- captured serial console (%d bytes) -----" % len(console))
            print(console if console else "(nothing received on the serial socket)")
            print("----- end console -----\n", flush=True)
            qlog = getattr(dc, "log", None)
            qlog_path = getattr(qlog, "name", None)
            if qlog_path:
                try:
                    with open(qlog_path, "rb") as f:
                        qtext = f.read().decode("latin1", "replace")
                except OSError:
                    qtext = None
                print("----- qemu's own stdout/stderr (%s) -----" % qlog_path)
                print(qtext if qtext else "(empty or unreadable)")
                print("----- end qemu log -----\n", flush=True)
            return
        res.add("mainline boot: reaches init self-check", PASS)

        console = bytes(dc.buf).decode("latin1")
        print("\n----- captured serial console (%d bytes) -----" % len(console))
        print(console)
        print("----- end console -----\n", flush=True)
        cfg.uname = _extract_uname(console)

        res.check("init self-check reports PASS",
                  lambda: has(console, "CI_RESULT: PASS"))
        res.check("no panic/oops in boot console", lambda: _console_clean(console))
        res.check("root mounted from ISO9660 GD-ROM",
                  lambda: has(console, "iso9660"))

        res.check("PVR framebuffer non-blank", lambda: _fb_ok(dc, cfg))

        if "8139" in console or "rtl8139" in console.lower():
            res.check("BBA (rtl8139) driver probed",
                      lambda: has(console.lower(), "eth0"))
        else:
            res.add("BBA (rtl8139) driver probed", SKIP,
                    "no 8139 driver message on console")
    finally:
        dc.close()


def _extract_uname(console):
    """Pull the line the rootfs's S99dc-ci-check printed after "uname -a"."""
    marker = "=== uname -a ==="
    idx = console.find(marker)
    if idx == -1:
        return None
    nl = console.find("\n", idx + len(marker))
    if nl == -1:
        return None
    line = console[nl + 1:].split("\n", 1)[0].strip()
    return line or None


def _console_clean(console):
    for bad in ("Kernel panic", "Oops", "Unable to handle kernel"):
        need(bad not in console, "console contains %r" % bad)
    return True


def _fb_ok(dc, cfg):
    ppm = os.path.join(cfg.artdir, "pvr.ppm")
    need(dc.screendump(ppm), "screendump produced no file")
    w, h, px = parse_ppm(ppm)
    need(w > 0 and h > 0, "bad PPM dimensions %dx%d" % (w, h))
    need(ppm_distinct_pixels(px) > 1, "framebuffer is a solid/blank frame")
    return True


# ============================================================================
def build_config():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default=os.environ.get(
        "DC_QEMU", "/media/flo/nvme0-ssd/qemu/build/qemu-system-sh4"))
    ap.add_argument("--kernel", default=os.environ.get("DC_KERNEL", "vmlinux"))
    ap.add_argument("--iso", default=os.environ.get("DC_ISO", "rootfs.iso"))
    ap.add_argument("--no-net", dest="net", action="store_false",
                    help="don't attach -nic user (BBA check will be SKIPped)")
    ap.add_argument("--tap", action="store_true", help="emit TAP13 output")
    ap.add_argument("--keep-artifacts", action="store_true")
    ap.add_argument("--boot-timeout", type=int, default=120)
    ap.add_argument("--kernel-sha", default=os.environ.get("DC_KERNEL_SHA", ""),
                    help="linux-next commit this --kernel was built from "
                         "(informational only, just printed in the log)")
    return ap.parse_args()


def main():
    cfg = build_config()
    if not os.path.exists(cfg.qemu):
        sys.exit("qemu binary not found: %s (set --qemu/$DC_QEMU)" % cfg.qemu)
    cfg.artdir = tempfile.mkdtemp(prefix="dc-mainline-")
    res = Results(color=not cfg.tap)

    print("Dreamcast mainline boot-regression test")
    print("  qemu:   %s" % cfg.qemu)
    print("  kernel: %s" % cfg.kernel)
    if cfg.kernel_sha:
        print("  linux-next @ %s" % cfg.kernel_sha)
    print("  iso:    %s\n" % cfg.iso)

    try:
        scenario_boot(cfg, res)
    except Exception:
        res.add("scenario_boot", FAIL, "harness error")
        traceback.print_exc()

    code = res.tap() if cfg.tap else res.summary()
    res.write_github_summary(cfg)

    if cfg.keep_artifacts:
        print("artifacts kept in %s" % cfg.artdir)
    else:
        import shutil
        shutil.rmtree(cfg.artdir, ignore_errors=True)
    sys.exit(code)


if __name__ == "__main__":
    main()
