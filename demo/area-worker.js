/**
 * The expensive half of Area law, off the page's main thread.
 *
 * Not the plugin: the plugin solves on its render thread and a frame waits for
 * it. In a browser the render thread IS the page, and a lattice with the soft
 * edge up costs the port 0.6 to 2.4 s a solve (measured in Node on an M-series
 * Mac, 2026-09-24) -- a solve per slider event would freeze the page. So the
 * lattice-with-softness case is solved here and the page draws with the last
 * level it has until the answer arrives, and says so under the picture.
 *
 * The maths is the same module the page uses, waveform.js.
 */
import { derive, prepare, softArea, areaLevel } from './waveform.js';

let pending = null;
let busy = false;

function run() {
  if (busy || pending === null) return;
  busy = true;
  const job = pending;
  pending = null;
  // Yield once so a burst of messages collapses to the newest before the work.
  setTimeout(() => {
    const d = derive(job.frame);
    const pre = prepare(job.frame, d);
    const level = job.areaLaw ? areaLevel(job.frame, d, pre, job.position, job.softW) : job.level;
    const area = softArea(job.frame, d, pre, level, job.softW);
    postMessage({ key: job.key, level, area });
    busy = false;
    run();
  }, 0);
}

onmessage = (event) => {
  pending = event.data;
  run();
};
