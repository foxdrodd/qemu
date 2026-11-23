// Browser front end for qemu-system-sh4 -M dreamcast (Emscripten build).
//
// - ttySC1 (2nd -serial) -> stdio -> dcweb-lib.js TTY -> xterm.js
// - PVR2 framebuffer / VMU LCD -> ui/wasm-canvas.c -> <canvas>
// - keyboard / mouse on the screen canvas -> qemu_canvas_input() -> maple
import { KEYMAP } from "./keymap.js";
import createQemu from "./qemu-system-sh4.js";

const EV_KEY = 1, EV_REL = 2, EV_BTN = 3, EV_WHEEL = 4;

const params = new URLSearchParams(location.search);
const statusEl = document.getElementById("status");
const screen = document.getElementById("screen");
const lcd = document.getElementById("lcd");
const canvases = [screen, lcd];

function status(msg) {
  statusEl.textContent = msg;
}

// ---- serial console --------------------------------------------------------
const term = new Terminal({
  cols: 80, rows: 30, convertEol: false, scrollback: 5000,
  fontFamily: "ui-monospace, 'DejaVu Sans Mono', Menlo, monospace",
  fontSize: 13,
  theme: { background: "#0b0d10" },
});
term.open(document.getElementById("terminal"));
// Bytes typed into the terminal, consumed by the TTY in dcweb-lib.js.
const serialIn = [];
const utf8 = new TextEncoder();
term.onData((d) => serialIn.push(...utf8.encode(d)));

// ---- disc ------------------------------------------------------------------
async function fetchDisc(url) {
  const resp = await fetch(url);
  if (!resp.ok) {
    throw new Error(`${url}: HTTP ${resp.status}`);
  }
  const total = +resp.headers.get("Content-Length") || 0;
  const reader = resp.body.getReader();
  const chunks = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    got += value.length;
    status(total ? `Downloading disc… ${(100 * got / total).toFixed(0)} % ` +
                   `(${(got / 1048576).toFixed(1)} MiB)`
                 : `Downloading disc… ${(got / 1048576).toFixed(1)} MiB`);
  }
  const buf = new Uint8Array(got);
  let off = 0;
  for (const c of chunks) {
    buf.set(c, off);
    off += c.length;
  }
  return buf;
}

function pickLocalDisc() {
  return new Promise((resolve) => {
    const input = document.getElementById("discfile");
    document.getElementById("picker").hidden = false;
    input.onchange = async () => {
      const f = input.files[0];
      if (!f) return;
      document.getElementById("picker").hidden = true;
      status(`Reading ${f.name}…`);
      resolve({ name: f.name, data: new Uint8Array(await f.arrayBuffer()) });
    };
  });
}

// ---- input -------------------------------------------------------------------
let Q = null;

function send(type, a, b) {
  if (Q) Q._qemu_canvas_input(type, a, b);
}

screen.tabIndex = 0;
screen.addEventListener("keydown", (e) => {
  const code = KEYMAP[e.code];
  if (code === undefined) return;
  e.preventDefault();
  if (!e.repeat) send(EV_KEY, code, 1);
});
screen.addEventListener("keyup", (e) => {
  const code = KEYMAP[e.code];
  if (code === undefined) return;
  e.preventDefault();
  send(EV_KEY, code, 0);
});
screen.addEventListener("blur", () => {
  // Release modifiers so nothing stays stuck when focus leaves.
  for (const k of ["ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
                   "AltLeft", "AltRight", "MetaLeft", "MetaRight"]) {
    send(EV_KEY, KEYMAP[k], 0);
  }
});
screen.addEventListener("click", () => {
  screen.focus();
  if (document.getElementById("grab").checked &&
      document.pointerLockElement !== screen) {
    screen.requestPointerLock();
  }
});
screen.addEventListener("mousemove", (e) => {
  if (document.pointerLockElement === screen) {
    send(EV_REL, e.movementX, e.movementY);
  }
});
for (const [ev, down] of [["mousedown", 1], ["mouseup", 0]]) {
  screen.addEventListener(ev, (e) => {
    if (document.pointerLockElement === screen && e.button <= 2) {
      e.preventDefault();
      send(EV_BTN, e.button === 1 ? 1 : e.button === 2 ? 2 : 0, down);
    }
  });
}
screen.addEventListener("wheel", (e) => {
  if (document.pointerLockElement === screen) {
    e.preventDefault();
    send(EV_WHEEL, e.deltaY > 0 ? 1 : -1, 0);
  }
}, { passive: false });
screen.addEventListener("contextmenu", (e) => e.preventDefault());

// ---- boot --------------------------------------------------------------------
async function main() {
  if (!globalThis.crossOriginIsolated) {
    status("This page needs cross-origin isolation (COOP/COEP headers) for " +
           "SharedArrayBuffer. Serve it with serve.py.");
    return;
  }

  let disc;
  const discUrl = params.get("disc") || document.body.dataset.disc;
  if (discUrl) {
    try {
      disc = { name: discUrl, data: await fetchDisc(discUrl) };
    } catch (e) {
      status(`Could not load ${discUrl} (${e.message}); pick a disc image.`);
      disc = await pickLocalDisc();
    }
  } else {
    status("Pick a Dreamcast disc image (.cdi or .iso).");
    disc = await pickLocalDisc();
  }
  const ext = /\.iso$/i.test(disc.name) ? "iso" : "cdi";
  const discPath = `/disc.${ext}`;

  const roms = {};
  for (const r of ["efi-rtl8139.rom"]) {
    const resp = await fetch(`rom/${r}`);
    if (resp.ok) roms[r] = new Uint8Array(await resp.arrayBuffer());
  }

  const args = [
    "-L", "/rom",
    "-M", "dreamcast" + (params.has("nolcd") ? "" : ",vmu-lcd=on"),
    "-m", "16",
    "-accel", "tcg,tb-size=" + (params.get("tb") || "64"),
    "-display", "none",
    "-monitor", "none",
    "-serial", "null",
    "-serial", "stdio",
    "-nic", params.get("nic") || "user",
    "-drive", `if=none,file=${discPath},format=raw,readonly=on`,
  ];
  if (!params.has("nolcd")) {
    args.push("-drive", "if=none,file=/vmu.bin,format=raw");
  }
  const extra = params.get("args");
  if (extra) args.push(...extra.split(" ").filter(Boolean));

  status("Starting QEMU…");
  const t0 = performance.now();
  let firstOutput = true;
  let booted = false;

  Q = await createQemu({
    arguments: args,
    qemuSerialIn: serialIn,
    qemuSerialOut: (bytes) => term.write(bytes),
    preRun: [(mod) => {
      mod.FS.mkdir("/rom");
      for (const [name, data] of Object.entries(roms)) {
        mod.FS.writeFile(`/rom/${name}`, data);
      }
      mod.FS.writeFile(discPath, disc.data);
      disc.data = null;
      if (!params.has("nolcd")) {
        // Blank 128 KiB VMU; the guest sees it as /dev/mtd0.
        mod.FS.writeFile("/vmu.bin", new Uint8Array(128 * 1024));
      }
    }],
    print: (s) => console.log(s),
    printErr: (s) => {
      console.warn(s);
      // QEMU's own errors (bad option, missing file) before the machine runs.
      if (!booted && !/^warning: unsupported syscall/.test(s)) status(s);
    },
    qemuCanvasReady(n) {
      booted = true;
      if (n > 1) lcd.hidden = false;
      status("Running. Serial console on the right (press Enter there for a " +
             "shell); click the screen to use the maple keyboard.");
    },
    qemuCanvasDraw(idx, px, w, h) {
      const c = canvases[idx];
      if (!c) return;
      if (c.width !== w || c.height !== h) {
        c.width = w;
        c.height = h;
      }
      c.getContext("2d").putImageData(new ImageData(px, w, h), 0, 0);
      if (firstOutput && idx === 0) {
        firstOutput = false;
        console.log(`first frame after ${((performance.now() - t0) / 1000).toFixed(1)} s`);
      }
    },
  });
  window.qemu = Q;
}

main().catch((e) => {
  console.error(e);
  status(`Error: ${e.message || e}`);
});
