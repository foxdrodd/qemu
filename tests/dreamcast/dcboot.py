#!/usr/bin/env python3
"""
dcboot.py - headless boot/test harness for the QEMU Dreamcast machine.

Handles the fragile plumbing once: console on the 2nd serial (ttySC1), QMP for
control/screendump/input, waiting for the login prompt, one-command-at-a-time
serial I/O, and clean QMP shutdown (so a `wav` audiodev is finalized).

Use as a CLI:

    dcboot.py --kernel VMLINUX --drive ROOTFS.iso --cmd "uname -a" --cmd "ip a"
    dcboot.py --drive linux.cdi --net --screenshot /tmp/fb.ppm      # self-booting CDI
    dcboot.py --kernel VMLINUX --drive r.iso --vmu vmu.bin --wav /tmp/out.wav \\
              --cmd "amixer -c AICA cset numid=1 255" --seconds 5

or import it:

    from dcboot import DCBoot
    dc = DCBoot(kernel="vmlinux", drives=["rootfs.iso"])
    dc.boot()
    print(dc.run("dmesg | grep -i eth0"))
    dc.screendump("/tmp/fb.ppm")          # PVR framebuffer
    dc.screendump("/tmp/lcd.ppm", "vmu")  # VMU LCD (needs vmu-lcd=on)
    dc.quit()
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

DEF_QEMU = os.environ.get(
    "DC_QEMU", "/media/flo/nvme0-ssd/qemu/build/qemu-system-sh4")
DEF_KERNEL = os.environ.get(
    "DC_KERNEL",
    "/home/flo/devel/t2-hacking/dreamcast/dreamcast-linux/.dreamcast/src/"
    "linux-7.1.3/vmlinux")
# gdb that understands sh4 (Ubuntu's plain `gdb` does not; gdb-multiarch does).
DEF_GDB = os.environ.get("DC_GDB", "gdb-multiarch")

# QEMU's own firmware/ROM search-path auto-detection (finding pc-bios/ next
# to the binary) relies on being run from within its configured build tree.
# It breaks when qemu-system-sh4 is shipped around as a bare artifact (e.g.
# a CI job that only downloads the binary, not the whole build/ dir) — the
# rtl8139 NIC's default option ROM ("efi-rtl8139.rom") then fails to load
# and QEMU refuses to start. dcboot.py is vendored inside this repo, so
# pc-bios/ is always at a fixed relative offset regardless of where the
# binary itself lives — pass it explicitly via -L rather than rely on
# QEMU's auto-detection.
_PC_BIOS_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "pc-bios"))


class DCBoot:
    def __init__(self, kernel=None, drives=None, vmu=None, net=False,
                 wav=None, audiodev=None, vmu_lcd=False, mem=16,
                 machine="dreamcast", qemu=DEF_QEMU, extra=None,
                 symbols=None, gdb_bin=DEF_GDB,
                 prompt="Please press Enter", shell="root@dreamcast"):
        self.qemu = qemu
        self.kernel = kernel
        self.drives = list(drives or [])
        self.vmu = vmu
        self.net = net
        self.wav = wav
        self.audiodev = audiodev
        self.vmu_lcd = vmu_lcd
        self.mem = mem
        self.machine = machine
        self.extra = list(extra or [])
        self.symbols = symbols or kernel   # ELF with symbols for gdb
        self.gdb_bin = gdb_bin
        self.gdb = None                    # tcp port once a gdbstub is running
        self.freeze = False                # -S: hold the CPU at reset
        self.prompt = prompt
        self.shell = shell
        tag = "%d" % os.getpid()
        self.ser_path = "/tmp/dcboot-ser-%s.sock" % tag
        self.qmp_path = "/tmp/dcboot-qmp-%s.sock" % tag
        self.mon_path = "/tmp/dcboot-mon-%s.sock" % tag
        self.con_path = "/tmp/dcboot-con-%s.log" % tag   # console log (gdb mode)
        self.buf = bytearray()
        self.p = None
        self.ser = None
        self.qmpf = None
        self.mon = None
        self._seq = 0

    # ---- process lifecycle ----
    def _argv(self):
        mach = self.machine
        if self.vmu_lcd:
            mach += ",vmu-lcd=on"
        if self.wav or self.audiodev:
            mach += ",audiodev=snd"
        av = [self.qemu, "-M", mach, "-m", str(self.mem),
              "-display", "none", "-no-reboot", "-serial", "null"]
        if os.path.isdir(_PC_BIOS_DIR):
            av += ["-L", _PC_BIOS_DIR]
        if self.gdb:
            # gdb drives the machine; nobody connects the console socket, so
            # send ttySC1 to a log file the user can tail instead.
            av += ["-serial", "file:%s" % self.con_path,
                   "-gdb", "tcp::%d" % self.gdb]
            if self.freeze:
                av += ["-S"]
        else:
            av += ["-serial", "unix:%s,server=on,wait=on" % self.ser_path]
        av += ["-qmp", "unix:%s,server=on,wait=off" % self.qmp_path,
               "-monitor", "unix:%s,server=on,wait=off" % self.mon_path]
        if self.kernel:
            av += ["-kernel", self.kernel]
        for d in self.drives:  # unit 0 = GD-ROM, then further read-only discs
            av += ["-drive", "if=none,file=%s,format=raw,readonly=on" % d]
        if self.vmu:           # writable 128 KB memory card (next if=none unit)
            av += ["-drive", "if=none,file=%s,format=raw" % self.vmu]
        if self.wav:
            av += ["-audiodev", "wav,id=snd,path=%s" % self.wav]
        elif self.audiodev:
            av += ["-audiodev", "%s,id=snd" % self.audiodev]
        if self.net:
            av += ["-nic", "user"]
        return av + self.extra

    def boot(self, timeout=150):
        for f in (self.ser_path, self.qmp_path, self.mon_path):
            try:
                os.unlink(f)
            except OSError:
                pass
        self.log = open("/tmp/dcboot-%d.log" % os.getpid(), "wb")
        self.p = subprocess.Popen(self._argv(), stdout=self.log,
                                  stderr=subprocess.STDOUT)
        self.ser = self._wsock(self.ser_path)
        threading.Thread(target=self._reader, daemon=True).start()
        if not self._wait(self.prompt, timeout):
            raise TimeoutError("no login prompt; see %s" % self.log.name)
        time.sleep(1.5)
        self._send("")                       # activate console
        self._wait(self.shell, 15)
        time.sleep(0.5)
        self._send("stty -echo 2>/dev/null")  # clean run() output (no cmd echo)
        time.sleep(0.4)
        return self

    def _wsock(self, path, t=30):
        for _ in range(t * 10):
            if os.path.exists(path):
                try:
                    s = socket.socket(socket.AF_UNIX)
                    s.connect(path)
                    return s
                except OSError:
                    pass
            if self.p.poll() is not None:
                raise RuntimeError("qemu exited; see %s" % self.log.name)
            time.sleep(0.1)
        raise TimeoutError("socket never appeared: %s" % path)

    def _reader(self):
        while True:
            try:
                d = self.ser.recv(4096)
            except OSError:
                break
            if not d:
                break
            self.buf.extend(d)

    def _send(self, line):
        self.ser.sendall((line + "\n").encode())

    def _wait(self, pat, t=60):
        end = time.time() + t
        pat = pat.encode()
        while time.time() < end:
            if pat in self.buf:
                return True
            if self.p is not None and self.p.poll() is not None:
                # Don't burn the rest of the timeout polling a dead process —
                # surface the exit immediately, it's the real diagnostic.
                raise RuntimeError(
                    "qemu exited early (code %s) while waiting for %r; see %s"
                    % (self.p.poll(), pat, getattr(self.log, "name", "?")))
            time.sleep(0.2)
        return False

    # ---- guest interaction ----
    def run(self, cmd, timeout=25):
        """Send one shell command, return its output (best-effort cleaned)."""
        self._seq += 1
        mark = "__EOC%d__" % self._seq
        start = len(self.buf)
        self._send(cmd)
        self._send("echo " + mark)           # sent separately: avoids long lines
        self._wait(mark, timeout)
        time.sleep(0.2)
        chunk = bytes(self.buf[start:]).decode("latin1")
        # cut at the marker's echoed *output* (last occurrence)
        cut = chunk.rfind(mark)
        if cut > 0:
            cut = chunk.rfind("\n", 0, cut)
            chunk = chunk[:cut]
        lines = chunk.splitlines()
        # drop any marker line and the echoed command line (echo may be on or off)
        out = [ln for ln in lines
               if mark not in ln and ln.strip() != cmd.strip()]
        return "\n".join(out).strip("\r\n")

    def send_raw(self, line):
        self._send(line)

    def wait_for(self, pat, timeout=60):
        return self._wait(pat, timeout)

    # ---- gdb / kernel debugging ----
    @staticmethod
    def _free_port():
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        p = s.getsockname()[1]
        s.close()
        return p

    def start_gdb(self, freeze=True):
        """Launch QEMU with a gdbstub (no login-prompt wait). Console -> a log
        file. Returns the TCP port. Follow with gdb_check()/gdb_interactive()."""
        if not shutil.which(self.gdb_bin):
            raise RuntimeError(
                "%s not found (apt install gdb-multiarch)" % self.gdb_bin)
        self.gdb = self._free_port()
        self.freeze = freeze
        for f in (self.qmp_path, self.mon_path, self.con_path):
            try:
                os.unlink(f)
            except OSError:
                pass
        self.log = open("/tmp/dcboot-%d.log" % os.getpid(), "wb")
        self.p = subprocess.Popen(self._argv(), stdout=self.log,
                                  stderr=subprocess.STDOUT)
        end = time.time() + 15
        while time.time() < end:
            try:
                socket.create_connection(("127.0.0.1", self.gdb), 0.5).close()
                return self.gdb
            except OSError:
                if self.p.poll() is not None:
                    raise RuntimeError("qemu exited; see %s" % self.log.name)
                time.sleep(0.1)
        raise TimeoutError("gdbstub port %d never opened" % self.gdb)

    def _gdb_argv(self, extra, batch=True):
        av = [self.gdb_bin, "-nx", "-q"]
        if batch:
            av += ["-batch"]
        av += ["-ex", "set pagination off",
               "-ex", "set debuginfod enabled off",
               "-ex", "set architecture sh4"]
        if self.symbols:
            av += ["-ex", "file %s" % self.symbols]
        av += ["-ex", "target remote :%d" % self.gdb]
        return av + extra

    def gdb_check(self, symbol, timeout=90, hw=True):
        """Set a breakpoint on `symbol`, continue, and report whether it hit.
        Returns (hit: bool, output: str). vmlinux has symbols but no DWARF, so
        use function symbols (maple_vblank_handler, ...) not source lines."""
        br = "hbreak" if hw else "break"
        extra = ["-ex", "%s %s" % (br, symbol),
                 "-ex", "continue",
                 "-ex", 'printf "=== HIT %s @ %%p ===\\n", $pc' % symbol,
                 "-ex", "backtrace 6",
                 "-ex", "detach", "-ex", "quit"]
        r = subprocess.run(self._gdb_argv(extra), capture_output=True,
                           text=True, timeout=timeout)
        out = r.stdout + (("\n" + r.stderr) if r.stderr.strip() else "")
        hit = ("=== HIT %s" % symbol) in r.stdout
        return hit, out

    def gdb_interactive(self, symbol=None):
        """Drop into an interactive gdb already connected + symbols loaded (an
        optional preset breakpoint on `symbol`). Reaps QEMU when gdb exits."""
        extra = ["-ex", "set confirm off"]
        if symbol:
            extra += ["-ex", "hbreak %s" % symbol]
        print("[dcboot] gdbstub :%d  symbols: %s" % (self.gdb, self.symbols))
        print("[dcboot] console log: %s  (tail -f to watch the boot)"
              % self.con_path)
        try:
            subprocess.call(self._gdb_argv(extra, batch=False))
        finally:
            self._teardown()

    # ---- QMP / monitor ----
    def _qmp(self, obj):
        if self.qmpf is None:
            q = self._wsock(self.qmp_path)
            self.qmpf = q.makefile("rwb", buffering=0)
            self.qmpf.readline()             # greeting
            self.qmpf.write(b'{"execute":"qmp_capabilities"}\n')
            self.qmpf.readline()
        self.qmpf.write((json.dumps(obj) + "\n").encode())
        while True:
            line = self.qmpf.readline()
            if not line:
                return None
            d = json.loads(line.decode())
            if "return" in d or "error" in d:
                return d

    def qmp(self, execute, **args):
        return self._qmp({"execute": execute, "arguments": args} if args
                         else {"execute": execute})

    def screendump(self, path, device=None):
        """Capture a console to a .ppm. device=None -> PVR; 'vmu' -> VMU LCD."""
        if self.mon is None:
            self.mon = self._wsock(self.mon_path)
            time.sleep(0.4)
            self.mon.recv(65536)
        cmd = "screendump %s" % path
        if device:
            cmd += " %s" % device
        self.mon.sendall((cmd + "\n").encode())
        time.sleep(1.5)
        return os.path.exists(path) and os.path.getsize(path) > 0

    # ---- shutdown ----
    def quit(self):
        try:
            self._qmp({"execute": "quit"})
        except Exception:
            pass
        self._teardown()

    def close(self):
        self._teardown()

    def _teardown(self):
        if self.p and self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(6)
            except subprocess.TimeoutExpired:
                self.p.kill()
        for f in (self.ser_path, self.qmp_path, self.mon_path):
            try:
                os.unlink(f)
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self._teardown()


def main():
    ap = argparse.ArgumentParser(description="Headless Dreamcast-Linux boot/test")
    ap.add_argument("--kernel", nargs="?", const=DEF_KERNEL,
                    help="vmlinux (omit to boot the disc's own 1ST_READ.BIN; "
                         "bare --kernel uses $DC_KERNEL)")
    ap.add_argument("--drive", action="append", default=[],
                    help="read-only disc (.iso/.cdi); repeatable, first = GD-ROM")
    ap.add_argument("--vmu", help="writable 128 KB VMU image")
    ap.add_argument("--vmu-lcd", action="store_true", help="enable the VMU LCD")
    ap.add_argument("--net", action="store_true", help="add -nic user")
    ap.add_argument("--wav", help="record AICA output to this WAV")
    ap.add_argument("--audiodev", help="raw audiodev driver, e.g. pa/alsa/none")
    ap.add_argument("--cmd", action="append", default=[],
                    help="guest shell command (repeatable)")
    ap.add_argument("--screenshot", help="PPM screendump of the PVR framebuffer")
    ap.add_argument("--seconds", type=float, default=0,
                    help="idle this long before shutdown (let audio/UI settle)")
    ap.add_argument("--mem", type=int, default=16)
    ap.add_argument("--qemu", default=DEF_QEMU)
    ap.add_argument("--boot-timeout", type=int, default=150)
    ap.add_argument("--symbols", help="ELF with symbols for gdb (default: --kernel)")
    ap.add_argument("--gdb", action="store_true",
                    help="launch paused under gdb (kernel debugging workbench)")
    ap.add_argument("--gdb-break", metavar="SYM",
                    help="scripted: break on SYM, continue, report if it hit "
                         "(e.g. maple_vblank_handler); exit 0 hit / 1 miss")
    args = ap.parse_args()

    dc = DCBoot(kernel=args.kernel, drives=args.drive, vmu=args.vmu,
                vmu_lcd=args.vmu_lcd, net=args.net, wav=args.wav,
                audiodev=args.audiodev, mem=args.mem, qemu=args.qemu,
                symbols=args.symbols or args.kernel)

    # gdb modes: launch paused with a gdbstub instead of booting to a shell.
    if args.gdb or args.gdb_break:
        if not dc.symbols:
            ap.error("--gdb needs --kernel or --symbols (for the symbol table)")
        dc.start_gdb(freeze=True)
        if args.gdb_break:
            hit, out = dc.gdb_check(args.gdb_break)
            print(out)
            print("RESULT:", "PASS breakpoint hit" if hit else "FAIL")
            dc.close()
            sys.exit(0 if hit else 1)
        dc.gdb_interactive()
        return

    try:
        dc.boot(timeout=args.boot_timeout)
        print("[dcboot] reached shell")
        for c in args.cmd:
            print("\n$ %s" % c)
            print(dc.run(c))
        if args.screenshot:
            ok = dc.screendump(args.screenshot)
            print("\n[dcboot] screendump %s: %s" %
                  (args.screenshot, "ok" if ok else "FAILED"))
        if args.seconds:
            time.sleep(args.seconds)
    finally:
        dc.quit()


if __name__ == "__main__":
    main()
