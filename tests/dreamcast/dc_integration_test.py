#!/usr/bin/env python3
"""
dc_integration_test.py - end-to-end integration test for the QEMU "-M dreamcast"
machine against real Dreamcast Linux.

Boots the distro under qemu-system-sh4 across a small scenario matrix and asserts
on guest-observable state (dmesg, /proc, /sys, devices, framebuffer, audio).
Prints a PASS/FAIL/SKIP summary and exits non-zero if anything failed, so it can
gate a `ninja` rebuild or a `rebuildkernel.sh`.

Reuses the dcboot.py harness (from the dreamcast-linux skill). Point $DC_HARNESS
at its scripts/ dir if it lives elsewhere.

    python3 dc_integration_test.py                     # core + peripherals (1-5)
    python3 dc_integration_test.py --with-audio --with-lcd
    python3 dc_integration_test.py --quick --tap       # boot smoke only, TAP out

Config resolves from env ($DC_QEMU, $DC_KERNEL) and the distro build/ dir; see
--help for overrides.
"""
import argparse
import os
import struct
import sys
import tempfile
import time
import traceback
import wave

# --- locate and import the dcboot harness -----------------------------------
HARNESS = os.environ.get(
    "DC_HARNESS", "/home/flo/.claude/skills/dreamcast-linux/scripts")
sys.path.insert(0, HARNESS)
try:
    from dcboot import DCBoot
except ImportError:
    sys.exit("cannot import dcboot from %r; set $DC_HARNESS to its scripts/ dir"
             % HARNESS)

DISTRO = os.environ.get(
    "DC_DISTRO", "/home/flo/devel/t2-hacking/dreamcast/dreamcast-linux")
BUILD = os.path.join(DISTRO, "build")

# --- tiny test runner --------------------------------------------------------
PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"
_C = {PASS: "\033[32m", FAIL: "\033[31m", SKIP: "\033[33m", "z": "\033[0m"}


class Results:
    def __init__(self, color=True):
        self.rows = []          # (name, status, msg, dur)
        self.color = color and sys.stdout.isatty()

    def add(self, name, status, msg="", dur=0.0):
        self.rows.append((name, status, msg, dur))
        if self.color:
            tag = "%s%s%s" % (_C[status], status, _C["z"])
        else:
            tag = status
        line = "  %s  %s" % (tag, name)
        if msg:
            line += "  — %s" % msg
        if dur:
            line += "  (%.1fs)" % dur
        print(line, flush=True)

    def check(self, name, fn):
        """Run one assertion (fn may return False or raise) inside a session."""
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
            self.add(name, FAIL, "%s: %s" % (type(e).__name__, e),
                     time.time() - t0)

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


# ============================================================================
# scenarios
# ============================================================================
def scenario_core(cfg, res):
    """Boot -kernel + ISO (with net); assert kernel/dmesg/rootfs/AICA/net/fb."""
    if not os.path.exists(cfg.iso):
        res.add("core boot", FAIL, "iso missing: %s" % cfg.iso)
        return
    dc = DCBoot(kernel=cfg.kernel, drives=[cfg.iso], net=True, qemu=cfg.qemu)
    try:
        try:
            dc.boot(timeout=cfg.boot_timeout)
        except Exception as e:
            res.add("core boot: reaches shell", FAIL, str(e))
            return
        res.add("core boot: reaches shell", PASS)

        res.check("kernel arch is sh4",
                  lambda: has(dc.run("uname -m"), "sh4"))
        res.check("kernel version non-empty", lambda: need(
            dc.run("uname -r").strip()[:1].isdigit(), "uname -r not a version"))
        res.check("no panic/oops in dmesg", lambda: _dmesg_clean(dc))
        res.check("rootfs populated (/bin non-empty)", lambda: need(
            dc.run("ls -1 /bin | wc -l").strip() not in ("", "0"),
            "/bin empty"))
        res.check("root filesystem mounted", lambda: has(
            dc.run("cat /proc/mounts"), " / "))
        res.check("AICA sound card enumerated", lambda: has(
            dc.run("cat /proc/asound/cards"), "AICA"))

        # network (slirp) — same boot
        res.check("network: slirp lease 10.0.2.15", lambda: has(
            dc.run("ip addr 2>/dev/null || ifconfig"), "10.0.2.15"))
        res.check("network: ping gateway 10.0.2.2", lambda: _ping_ok(dc))

        # framebuffer — same boot
        res.check("PVR framebuffer non-blank", lambda: _fb_ok(dc, cfg))
    finally:
        dc.close()


def _dmesg_clean(dc):
    out = dc.run("dmesg", timeout=30)
    for bad in ("Kernel panic", "Oops", "Unable to handle kernel"):
        need(bad not in out, "dmesg contains %r" % bad)
    return True


def _ping_ok(dc):
    out = dc.run("ping -c1 -W2 10.0.2.2", timeout=15)
    need("1 packets received" in out or "1 received" in out,
         "gateway unreachable: %r" % out[:200])
    return True


def _fb_ok(dc, cfg):
    ppm = os.path.join(cfg.artdir, "pvr.ppm")
    need(dc.screendump(ppm), "screendump produced no file")
    w, h, px = parse_ppm(ppm)
    need(w > 0 and h > 0, "bad PPM dimensions %dx%d" % (w, h))
    need(ppm_distinct_pixels(px) > 1, "framebuffer is a solid/blank frame")
    return True


def scenario_selfboot(cfg, res):
    """Boot the CDI's own descrambled 1ST_READ.BIN (no -kernel)."""
    if not os.path.exists(cfg.cdi):
        res.add("self-boot CDI", FAIL, "cdi missing: %s" % cfg.cdi)
        return
    dc = DCBoot(kernel=None, drives=[cfg.cdi], qemu=cfg.qemu)
    try:
        try:
            dc.boot(timeout=cfg.boot_timeout)
        except Exception as e:
            res.add("self-boot CDI: reaches shell", FAIL, str(e))
            return
        res.add("self-boot CDI: reaches shell", PASS)
        res.check("self-boot: kernel arch is sh4",
                  lambda: has(dc.run("uname -m"), "sh4"))
    finally:
        dc.close()


def scenario_vmu(cfg, res):
    """Writable VMU image → /dev/mtd0, vmufat mount + file round-trip."""
    if not os.path.exists(cfg.iso):
        res.add("VMU round-trip", FAIL, "iso missing")
        return
    vmu = os.path.join(cfg.artdir, "vmu.bin")
    with open(vmu, "wb") as f:                      # 128 KB blank card
        f.truncate(128 * 1024)
    dc = DCBoot(kernel=cfg.kernel, drives=[cfg.iso], vmu=vmu, qemu=cfg.qemu)
    try:
        try:
            dc.boot(timeout=cfg.boot_timeout)
        except Exception as e:
            res.add("VMU: boot with card", FAIL, str(e))
            return
        res.check("VMU: /dev/mtd0 present",
                  lambda: has(dc.run("ls /dev/mtd0 2>&1"), "/dev/mtd0"))

        def roundtrip():
            dc.run("mkdir -p /mnt/vmu")
            out = dc.run("mount -t vmufat /dev/mtd0 /mnt/vmu 2>&1")
            need("rror" not in out and "denied" not in out,
                 "vmufat mount failed: %r" % out)
            dc.run("echo DCTEST42 > /mnt/vmu/itest.txt")
            dc.run("sync")
            back = dc.run("cat /mnt/vmu/itest.txt")
            dc.run("umount /mnt/vmu")
            need("DCTEST42" in back, "readback mismatch: %r" % back)
            return True
        res.check("VMU: vmufat write/read round-trip", roundtrip)
    finally:
        dc.close()


def scenario_lcd(cfg, res):
    """VMU LCD second display → screendump device 'vmu'.

    The VMU device (and thus its LCD console) only exists when a VMU *drive* is
    attached — vmu-lcd=on alone creates nothing. So we must attach a card too.
    """
    if not os.path.exists(cfg.iso):
        res.add("VMU LCD screendump", FAIL, "iso missing")
        return
    vmu = os.path.join(cfg.artdir, "vmu-lcd.bin")
    with open(vmu, "wb") as f:
        f.truncate(128 * 1024)
    dc = DCBoot(kernel=cfg.kernel, drives=[cfg.iso], vmu=vmu, vmu_lcd=True,
                qemu=cfg.qemu)
    try:
        try:
            dc.boot(timeout=cfg.boot_timeout)
        except Exception as e:
            res.add("VMU LCD: boot vmu-lcd=on", FAIL, str(e))
            return

        # The LCD is a 48x32 mono panel exposed as /dev/vmu_lcd0 (192 bytes).
        res.check("VMU LCD: /dev/vmu_lcd0 present", lambda: has(
            dc.run("ls /dev/vmu_lcd0 2>&1"), "/dev/vmu_lcd0"))

        def shot():
            # Draw a deterministic non-uniform pattern so the frame is
            # guaranteed non-blank (proves guest write -> maple LCD BWRITE ->
            # console surface -> screendump end to end), independent of whatever
            # the distro itself paints at boot.
            dc.run("dd if=/dev/urandom of=/dev/vmu_lcd0 bs=192 count=1 "
                   "2>/dev/null")
            time.sleep(1)
            ppm = os.path.join(cfg.artdir, "vmu-lcd.ppm")
            need(dc.screendump(ppm, "vmu"), "no LCD screendump file "
                 "(is the VMU device present / id 'vmu' resolvable?)")
            w, h, px = parse_ppm(ppm)
            need(w > 0 and h > 0, "bad LCD PPM %dx%d" % (w, h))
            need(ppm_distinct_pixels(px) > 1, "LCD frame is blank/solid")
            return True
        res.check("VMU LCD: renders a non-blank image", shot)
    finally:
        dc.close()


def scenario_audio(cfg, res):
    """AICA end-to-end: record to WAV, assert non-silent output."""
    if not os.path.exists(cfg.iso):
        res.add("AICA audio", FAIL, "iso missing")
        return
    wav = os.path.join(cfg.artdir, "aica.wav")
    dc = DCBoot(kernel=cfg.kernel, drives=[cfg.iso], wav=wav, qemu=cfg.qemu)
    try:
        try:
            dc.boot(timeout=cfg.boot_timeout)
        except Exception as e:
            res.add("AICA: boot with audiodev", FAIL, str(e))
            return
        # master volume defaults to 0 (kzalloc) — must raise it while a stream
        # is (about to be) open, then play a tone.
        dc.run("amixer -c AICA cset numid=1 255 >/dev/null 2>&1")
        dc.run("speaker-test -t sine -f 440 -c2 -l1 >/dev/null 2>&1 &")
        time.sleep(4)
        dc.run("amixer -c AICA cset numid=1 255 >/dev/null 2>&1")
        time.sleep(2)
    finally:
        dc.quit()                                   # finalizes the WAV
    peak = wav_peak(wav)
    res.check("AICA: WAV recorded", lambda: need(peak >= 0, "no readable WAV"))
    res.check("AICA: output is non-silent (peak>1000)",
              lambda: need(peak > 1000, "silent WAV (peak=%d)" % peak))


def scenario_gdb(cfg, res):
    """gdbstub kernel debugging: attach gdb, break in a driver, confirm it hits.

    Proves the debug workbench end to end — the gdbstub is reachable, sh4
    symbols resolve against vmlinux, and a hardware breakpoint on a live kernel
    function (the VSYNC-driven maple poll) actually fires while the guest runs.
    """
    if not os.path.exists(cfg.kernel):
        res.add("gdb breakpoint", FAIL, "kernel/symbols missing: %s" % cfg.kernel)
        return
    dc = DCBoot(kernel=cfg.kernel, drives=[cfg.iso], symbols=cfg.kernel,
                qemu=cfg.qemu)
    try:
        try:
            dc.start_gdb(freeze=True)
        except Exception as e:
            res.add("gdb: launch with gdbstub", FAIL, str(e))
            return
        res.add("gdb: launch paused with gdbstub", PASS)

        def brk():
            hit, out = dc.gdb_check("maple_vblank_handler",
                                    timeout=cfg.boot_timeout)
            need(hit, "breakpoint never hit; gdb said:\n%s" % out[-400:])
            return True
        res.check("gdb: hardware breakpoint hits in maple driver", brk)
    finally:
        dc.close()


# ============================================================================
def build_config():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default=os.environ.get(
        "DC_QEMU", "/media/flo/nvme0-ssd/qemu/build/qemu-system-sh4"))
    ap.add_argument("--kernel", default=os.environ.get(
        "DC_KERNEL", os.path.join(
            DISTRO, ".dreamcast/src/linux-7.1.3/vmlinux")))
    ap.add_argument("--iso", default=os.path.join(
        BUILD, "linux-7.1.3-with-userland-musl.iso"))
    ap.add_argument("--cdi", default=os.path.join(
        BUILD, "linux-7.1.3-with-userland-musl.cdi"))
    ap.add_argument("--with-audio", action="store_true",
                    help="run the AICA sound test (needs the sound branch)")
    ap.add_argument("--with-lcd", action="store_true",
                    help="run the VMU LCD screendump test")
    ap.add_argument("--with-gdb", action="store_true",
                    help="run the gdbstub kernel-debugging test (needs "
                         "gdb-multiarch)")
    ap.add_argument("--quick", action="store_true",
                    help="only the boot smoke tests (core + self-boot)")
    ap.add_argument("--tap", action="store_true", help="emit TAP13 output")
    ap.add_argument("--keep-artifacts", action="store_true")
    ap.add_argument("--boot-timeout", type=int, default=180)
    cfg = ap.parse_args()
    return cfg


def main():
    cfg = build_config()
    if not os.path.exists(cfg.qemu):
        sys.exit("qemu binary not found: %s (set --qemu/$DC_QEMU)" % cfg.qemu)
    cfg.artdir = tempfile.mkdtemp(prefix="dc-itest-")
    res = Results(color=not cfg.tap)

    print("Dreamcast Linux integration test")
    print("  qemu:   %s" % cfg.qemu)
    print("  kernel: %s" % cfg.kernel)
    print("  iso:    %s" % cfg.iso)
    print("  cdi:    %s\n" % cfg.cdi)

    scenarios = [scenario_core, scenario_selfboot]
    if not cfg.quick:                       # --quick = boot smoke only
        scenarios.append(scenario_vmu)
    if cfg.with_lcd:                         # explicit opt-ins always run
        scenarios.append(scenario_lcd)
    if cfg.with_audio:
        scenarios.append(scenario_audio)
    if cfg.with_gdb:
        scenarios.append(scenario_gdb)

    for sc in scenarios:
        try:
            sc(cfg, res)
        except Exception:
            res.add(sc.__name__, FAIL, "harness error")
            traceback.print_exc()

    code = res.tap() if cfg.tap else res.summary()

    if cfg.keep_artifacts:
        print("artifacts kept in %s" % cfg.artdir)
    else:
        import shutil
        shutil.rmtree(cfg.artdir, ignore_errors=True)
    sys.exit(code)


if __name__ == "__main__":
    main()
