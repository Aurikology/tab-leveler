// Runs the parity programme through leveler.wasm and compares it with the native output.
// Usage: node tests/parity.mjs <leveler.wasm> <native.f32>
import { readFileSync } from "node:fs";

const [wasmPath, nativePath] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {
  wasi_snapshot_preview1: { proc_exit() {}, fd_write() { return 0; }, fd_close() { return 0; }, fd_seek() { return 0; } },
  env: { emscripten_notify_memory_growth() {} },
});
const x = instance.exports;
x._initialize?.();

const rate = 48000, block = 128, total = 10 * rate;
const h = x.tl_create(rate, 2, block, -18);
const native = new Float32Array(readFileSync(nativePath).buffer.slice(0));

let seed = 12345 >>> 0, t = 0, off = 0, maxDiff = 0, peak = 0;
const sample = () => {
  const sec = t / rate;
  let v;
  if (sec < 4) v = 0.03 * Math.sin((2 * Math.PI * 440 * t) / rate);
  else if (sec < 7) v = 0.7 * Math.sin((2 * Math.PI * 1000 * t) / rate);
  else {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    v = ((seed >>> 8) / 16777216) * 2 - 1;
    v *= 0.5;
  }
  t++;
  return Math.fround(v);
};

const t0 = performance.now();
for (let done = 0; done < total; done += block) {
  // buffers are re-read every block: memory may grow and detach old views
  const L = new Float32Array(x.memory.buffer, x.tl_buffer(h, 0), block);
  const R = new Float32Array(x.memory.buffer, x.tl_buffer(h, 1), block);
  for (let i = 0; i < block; i++) { const s = sample(); L[i] = s; R[i] = -s; }
  x.tl_process(h, block);
  for (let i = 0; i < block; i++) {
    maxDiff = Math.max(maxDiff, Math.abs(L[i] - native[off + i]), Math.abs(R[i] - native[off + block + i]));
    peak = Math.max(peak, Math.abs(L[i]));
  }
  off += 2 * block;
}
const ms = performance.now() - t0;
const realtime = (total / rate) / (ms / 1000);
console.log(`frames ${total}, max |wasm - native| = ${maxDiff.toExponential(3)}, output peak ${peak.toFixed(4)}`);
console.log(`latency ${x.tl_latency_frames(h)} frames, processed 10 s of stereo in ${ms.toFixed(1)} ms (${realtime.toFixed(0)}x real time)`);
x.tl_destroy(h);
const ok = maxDiff <= 1e-6 && peak <= 0.8913 + 1e-6;
console.log(ok ? "PASS  WASM matches native" : "FAIL  WASM differs from native");
process.exit(ok ? 0 : 1);
