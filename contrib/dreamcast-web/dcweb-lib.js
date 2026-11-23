// Emscripten JS library (--js-library) for the Dreamcast web front end.
//
// 1. poll(): Emscripten's implementation ignores the timeout and returns at
//    once, so QEMU's main loop would spin and proxy a syscall to the browser
//    thread on every iteration.  Wrap it: check the fds (on the main thread,
//    where the FS lives) and, if none is ready, sleep in the calling worker
//    for up to POLL_SLICE_MS before re-checking.  Event-notifier wakeups are
//    thus delayed by at most one slice.
//
// 2. TTY: stdio is the guest serial console (-serial stdio).  Output goes to
//    Module.qemuSerialOut(bytes); input is taken from Module.qemuSerialIn
//    (an array of byte values the page appends to).  Nothing here blocks.

const Lib = LibraryManager.library;

function renameFuncKeys(name, newName) {
  for (const k of Object.keys(Lib)) {
    if (k === name || k.startsWith(`${name}__`)) {
      Lib[newName + k.slice(name.length)] = Lib[k];
      delete Lib[k];
    }
  }
}

// The original poll keeps its "__proxy: sync" and runs on the main thread.
renameFuncKeys('__syscall_poll', '$dcweb_poll_fds');

Object.assign(Lib, {
  $dcweb_sleep_cell: null,

  __syscall_poll__sig: 'ipii',
  __syscall_poll__deps: ['$dcweb_poll_fds', '$dcweb_sleep_cell'],
  __syscall_poll: (fds, nfds, timeout) => {
    const POLL_SLICE_MS = 1;
    let n = dcweb_poll_fds(fds, nfds, 0);
    if (n !== 0 || timeout === 0 || !ENVIRONMENT_IS_PTHREAD) {
      return n;
    }
    dcweb_sleep_cell ||= new Int32Array(new SharedArrayBuffer(4));
    const deadline = timeout > 0 ? performance.now() + timeout : Infinity;
    for (;;) {
      const left = deadline - performance.now();
      Atomics.wait(dcweb_sleep_cell, 0, 0, Math.max(0, Math.min(POLL_SLICE_MS, left)));
      n = dcweb_poll_fds(fds, nfds, 0);
      if (n !== 0 || performance.now() >= deadline) {
        return n;
      }
    }
  },
});

Lib.$TTY__deps.push('$FS');
Object.assign(Lib.$TTY.stream_ops, {
  read: (stream, buffer, offset, length) => {
    const q = Module['qemuSerialIn'];
    if (!q || !q.length) {
      throw new FS.ErrnoError({{{ cDefs.EAGAIN }}});
    }
    const n = Math.min(length, q.length);
    buffer.set(q.splice(0, n), offset);
    return n;
  },
  write: (stream, buffer, offset, length) => {
    if (buffer === HEAP8) {
      buffer = HEAPU8;
    }
    const out = Module['qemuSerialOut'];
    if (out) {
      out(buffer.slice(offset, offset + length));
    }
    return length;
  },
  poll: (stream, timeout) => {
    const q = Module['qemuSerialIn'];
    return (q && q.length ? {{{ cDefs.POLLIN }}} : 0) | {{{ cDefs.POLLOUT }}};
  },
});
