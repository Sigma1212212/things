// Runs the freestanding ASM3D test module under Node.js with a minimal
// implementation of the "a3" platform imports.
import { readFileSync } from 'node:fs';

const path = process.argv[2];
const bytes = readFileSync(path);
let memory;
const dec = new TextDecoder();
const str = (p, n) => dec.decode(new Uint8Array(memory.buffer, p, n));
const levels = ['TRACE', 'DEBUG', 'INFO', 'WARN', 'ERROR', 'FATAL'];
const store = new Map();
const imports = {
  a3: {
    log: (lvl, p, n) => (lvl >= 3 ? console.error : console.log)(`[wasm] ${str(p, n)}`),
    now_ms: () => performance.now(),
    wall_clock: () => Date.now() / 1000,
    abort: (p, n) => { throw new Error('abort: ' + str(p, n)); },
    storage_write: (kp, kn, dp, dn) => { store.set(str(kp, kn), new Uint8Array(memory.buffer, dp, dn).slice()); return 1; },
    storage_size: (kp, kn) => { const v = store.get(str(kp, kn)); return v ? v.length : -1; },
    storage_read: (kp, kn, dst, cap) => { const v = store.get(str(kp, kn)); if (!v) return -1; const n = Math.min(cap, v.length); new Uint8Array(memory.buffer, dst, n).set(v.subarray(0, n)); return n; },
    storage_delete: (kp, kn) => { store.delete(str(kp, kn)); },
  },
};
const { instance } = await WebAssembly.instantiate(bytes, imports);
memory = instance.exports.memory;
const failures = instance.exports.a3_run_tests();
console.log(`[wasm] module size: ${(bytes.length / 1024).toFixed(1)} KiB`);
process.exit(failures ? 1 : 0);
