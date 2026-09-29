// Runs the freestanding ASM3D test module under Node.js with a minimal
// implementation of the "a3" platform imports and the shared JS file
// system (web/a3fs.js).
import { readFileSync } from 'node:fs';
import { A3FS } from '../web/a3fs.js';

const path = process.argv[2];
const bytes = readFileSync(path);
let memory;
const dec = new TextDecoder();
const str = (p, n) => dec.decode(new Uint8Array(memory.buffer, p, n));
const levels = ['TRACE', 'DEBUG', 'INFO', 'WARN', 'ERROR', 'FATAL'];
const fs = new A3FS();
const imports = {
  a3: {
    log: (lvl, p, n) => (lvl >= 3 ? console.error : console.log)(`[wasm] ${str(p, n)}`),
    now_ms: () => performance.now(),
    wall_clock: () => Date.now() / 1000,
    abort: (p, n) => { throw new Error('abort: ' + str(p, n)); },
  },
  fs: fs.imports(() => memory),
  audio: { open: () => 0, close: () => {} },   // no sound device under Node
};
const { instance } = await WebAssembly.instantiate(bytes, imports);
memory = instance.exports.memory;
const failures = instance.exports.a3_run_tests();
console.log(`[wasm] module size: ${(bytes.length / 1024).toFixed(1)} KiB`);
process.exit(failures ? 1 : 0);
