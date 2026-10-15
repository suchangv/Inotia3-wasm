/*
 * Node driver for the wasm32 build.
 *
 * The wasm module is freestanding: it has no filesystem and no clock, so this
 * supplies both. It reads libgame.so, copies it into the module's linear
 * memory, and calls spike_main(), which runs the same boot-and-frames sequence
 * the native build runs -- the same code, compiled for a different target, so
 * the instruction counts are directly comparable and the difference between
 * them is the interpreter's throughput.
 *
 * That difference is the point. Instructions retired per frame is already known
 * from the native build (~8,963); what this measures is how many of them a
 * browser engine can retire per second, which is the last unknown between here
 * and "can this be playable".
 *
 *   node tools/run_wasm.mjs [libgame.so] [frames]
 */

import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';

const SO_PATH = process.argv[2] || 'game/libgame.so';
const FRAMES = parseInt(process.argv[3] || '0', 10);

const wasmBytes = readFileSync('build/spike.wasm');
const soBytes = readFileSync(SO_PATH);
console.log(`wasm   : build/spike.wasm (${wasmBytes.length} bytes)`);
console.log(`image  : ${SO_PATH} (${soBytes.length} bytes)`);
console.log(`engine : node ${process.version} / V8\n`);

/* The module logs a lot; collect and print once so the log itself does not
 * dominate the measurement. */
const lines = [];
let pending = '';

const imports = {
    host: {
        log: (ptr, len) => {
            const mem = new Uint8Array(instance.exports.memory.buffer, ptr, len);
            pending += Buffer.from(mem).toString('latin1');
            let nl;
            while ((nl = pending.indexOf('\n')) >= 0) {
                lines.push(pending.slice(0, nl));
                pending = pending.slice(nl + 1);
            }
        },
        // The guest's clock, and the one the frame timings are measured
        // against. performance.now() is milliseconds since process start.
        now: () => performance.now() / 1000,
    },
};

const { instance } = await WebAssembly.instantiate(wasmBytes, imports);
const ex = instance.exports;

/* Copy the image into linear memory.
 *
 * spike_alloc may grow memory, which detaches any ArrayBuffer view taken
 * before it -- so the view is created *after* the call, from the buffer the
 * instance reports at that moment. */
const soPtr = ex.spike_alloc(soBytes.length);
new Uint8Array(ex.memory.buffer, soPtr, soBytes.length).set(soBytes);
ex.spike_set_so(soPtr, soBytes.length);

if (FRAMES > 0) {
    // main() defaults to 30 frames in the wasm build; there is no argv here.
    console.log(`(frames requested: ${FRAMES} -- the wasm build defaults to 30 ` +
                `since it has no command line)\n`);
}

const t0 = performance.now();
ex.spike_main();
const t1 = performance.now();

for (const l of lines) console.log(l);
if (pending) console.log(pending);

console.log(`\n=== wall clock: whole run ${(t1 - t0).toFixed(1)} ms ===`);
