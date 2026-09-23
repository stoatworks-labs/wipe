/**
 * Wipe's CPU half, ported to JavaScript: Controls.cpp, Waveform.cpp and the
 * modulator phase from Timing.cpp. A hand translation, function for function,
 * with the same constants and the same order of operations. Nothing checks it
 * but a reader -- demo/tools/check_shaders.py checks the shaders only -- so
 * when one of those C++ files changes, change this too.
 *
 * A module of its own, rather than part of plugin.js, because it is loaded
 * twice: by the page, and by area-worker.js, which runs the expensive Area
 * solves off the main thread (see plugin.js for why, and for what that
 * changes).
 *
 * Not ported: MatrixRank / MatrixThreshold. The matrix's Feistel order runs
 * in the shader, as it does in the plugin; the CPU's matrix area is a count
 * of ranks and never needs the permutation.
 */
//===========================================================================
// Controls.cpp — what a 0..1 slider means. Every function returns a float in
// the plugin, so every one is rounded to float32 here too.
//===========================================================================
const f32 = Math.fround;
const clamp01 = (v) => f32(Math.min(1, Math.max(0, v)));
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

const PX_MAX = 64.0;
const MULTIPLE_MAX = 8;
const MATRIX_BASE_COLS = 32;
const MATRIX_BASE_ROWS = 18;

const PAT_HORIZONTAL = 0;
const PAT_VERTICAL = 1;
const PAT_BOX = 2;
const PAT_DIAMOND = 3;
const PAT_CIRCLE = 4;
const PAT_CLOCK = 5;
const PAT_MATRIX = 6;
const PAT_COUNT = 7;
const LAW_AREA = 1;
const LAW_COUNT = 2;

const softnessPxFromParam = (v) => f32(clamp01(v) * PX_MAX);
const borderWidthPxFromParam = (v) => f32(clamp01(v) * PX_MAX);
const borderSoftnessPxFromParam = (v) => f32(clamp01(v) * PX_MAX);
const rotationRadiansFromParam = (v) => f32(clamp01(v) * f32(6.283185307179586));
// 4^(2t-1): 0.25 at 0, 1 at 0.5, 4 at 1.
const aspectFromParam = (v) => f32(Math.pow(4.0, f32(2.0 * clamp01(v) - 1.0)));
const modAmountPxFromParam = (v) => f32(clamp01(v) * PX_MAX);
const modFrequencyFromParam = (v) => f32(0.5 * f32(Math.pow(64.0, clamp01(v))));
const modSpeedHzFromParam = (v) => f32(clamp01(v) * 4.0);
// std::lround rounds half away from zero; the values here are never negative.
const multipleFromParam = (v) => clamp(Math.round(v), 1, MULTIPLE_MAX);
const optionIndex = (v, count) => clamp(Math.round(v), 0, count - 1);

//===========================================================================
// Waveform.cpp — the frame, Edge law, and Area law in closed form.
//===========================================================================
const TWO_PI = 6.283185307179586;
const PI = 3.141592653589793;
const AREA_SOLVE_TOLERANCE = 1e-10;

const cross = (a, b) => a.x * b.y - a.y * b.x;
const dot = (a, b) => a.x * b.x + a.y * b.y;

/** Shoelace. Positive for a counter-clockwise polygon. */
function polyArea(p) {
  let sum = 0.0;
  const n = p.length;
  for (let i = 0; i < n; i += 1) sum += cross(p[i], p[(i + 1) % n]);
  return 0.5 * sum;
}

/** Sutherland-Hodgman against one half-plane: keeps a*x + b*y <= c. */
function clipHalf(input, a, b, c) {
  const out = [];
  const n = input.length;
  if (n === 0) return out;
  for (let i = 0; i < n; i += 1) {
    const cur = input[i];
    const prev = input[(i + n - 1) % n];
    const fc = a * cur.x + b * cur.y - c;
    const fp = a * prev.x + b * prev.y - c;
    const inC = fc <= 0.0;
    const inP = fp <= 0.0;
    if (inC !== inP) {
      const t = fp / (fp - fc);
      out.push({ x: prev.x + t * (cur.x - prev.x), y: prev.y + t * (cur.y - prev.y) });
    }
    if (inC) out.push(cur);
  }
  return out;
}

/** One edge's signed contribution to a convex polygon ∩ disc about the origin. */
function edgeDiscContribution(a, b, r) {
  const d = { x: b.x - a.x, y: b.y - a.y };
  const A = dot(d, d);
  const B = 2.0 * dot(a, d);
  const C = dot(a, a) - r * r;
  const ts = [0.0, 1.0, 1.0, 1.0];
  let count = 1;
  if (A > 0.0) {
    const disc = B * B - 4.0 * A * C;
    if (disc > 0.0) {
      const s = Math.sqrt(disc);
      const t0 = (-B - s) / (2.0 * A);
      const t1 = (-B + s) / (2.0 * A);
      if (t0 > 0.0 && t0 < 1.0) ts[count++] = t0;
      if (t1 > 0.0 && t1 < 1.0) ts[count++] = t1;
    }
  }
  ts[count++] = 1.0;

  let sum = 0.0;
  for (let i = 0; i + 1 < count; i += 1) {
    const p0 = { x: a.x + ts[i] * d.x, y: a.y + ts[i] * d.y };
    const p1 = { x: a.x + ts[i + 1] * d.x, y: a.y + ts[i + 1] * d.y };
    const tm = 0.5 * (ts[i] + ts[i + 1]);
    const m = { x: a.x + tm * d.x, y: a.y + tm * d.y };
    if (dot(m, m) < r * r) sum += 0.5 * cross(p0, p1);
    else sum += 0.5 * r * r * Math.atan2(cross(p0, p1), dot(p0, p1));
  }
  return sum;
}

function discPolyArea(p, centre, r) {
  if (!(r > 0.0) || p.length < 3) return 0.0;
  let sum = 0.0;
  const n = p.length;
  for (let i = 0; i < n; i += 1) {
    const a = { x: p[i].x - centre.x, y: p[i].y - centre.y };
    const b = { x: p[(i + 1) % n].x - centre.x, y: p[(i + 1) % n].y - centre.y };
    sum += edgeDiscContribution(a, b, r);
  }
  return Math.max(0.0, sum);
}

const usesPositioner = (pattern) =>
  pattern === PAT_BOX || pattern === PAT_DIAMOND || pattern === PAT_CIRCLE || pattern === PAT_CLOCK;

function toFrame(d, u, v) {
  const dx = (u - d.centreX) * d.scaleX;
  const dy = v - d.centreY;
  return { x: d.cosR * dx - d.sinR * dy, y: d.sinR * dx + d.cosR * dy };
}

const picturePoly = (d) => [toFrame(d, 0, 0), toFrame(d, 1, 0), toFrame(d, 1, 1), toFrame(d, 0, 1)];

function normOf(pattern, x, y) {
  switch (pattern) {
    case PAT_BOX: return Math.max(Math.abs(x), Math.abs(y));
    case PAT_DIAMOND: return Math.abs(x) + Math.abs(y);
    default: return Math.sqrt(x * x + y * y);
  }
}

function shapeArea(pattern, region, centre, size) {
  if (!(size > 0.0) || region.length < 3) return 0.0;
  if (pattern === PAT_CIRCLE) return discPolyArea(region, centre, size);

  let p = region;
  if (pattern === PAT_BOX) {
    p = clipHalf(p, 1.0, 0.0, centre.x + size);
    p = clipHalf(p, -1.0, 0.0, -(centre.x - size));
    p = clipHalf(p, 0.0, 1.0, centre.y + size);
    p = clipHalf(p, 0.0, -1.0, -(centre.y - size));
  } else {
    p = clipHalf(p, 1.0, 1.0, centre.x + centre.y + size);
    p = clipHalf(p, 1.0, -1.0, centre.x - centre.y + size);
    p = clipHalf(p, -1.0, 1.0, -centre.x + centre.y + size);
    p = clipHalf(p, -1.0, -1.0, -centre.x - centre.y + size);
  }
  return polyArea(p);
}

/** The wedge [a0, a1) clockwise from twelve, a1 - a0 <= pi, ∩ region. */
function wedgeArea(region, a0, a1) {
  const d0 = { x: Math.sin(a0), y: Math.cos(a0) };
  const d1 = { x: Math.sin(a1), y: Math.cos(a1) };
  let p = clipHalf(region, -d0.y, d0.x, 0.0);
  p = clipHalf(p, d1.y, -d1.x, 0.0);
  return polyArea(p);
}

function prepare(frame, d) {
  const p = { picture: picturePoly(d), qxmin: 1e300, qxmax: -1e300, qymin: 1e300, qymax: -1e300, cells: [] };
  for (const v of p.picture) {
    p.qxmin = Math.min(p.qxmin, v.x);
    p.qxmax = Math.max(p.qxmax, v.x);
    p.qymin = Math.min(p.qymin, v.y);
    p.qymax = Math.max(p.qymax, v.y);
  }
  const lattice = (frame.pattern === PAT_BOX || frame.pattern === PAT_DIAMOND || frame.pattern === PAT_CIRCLE)
    && (frame.multipleH > 1 || frame.multipleV > 1);
  if (lattice) {
    const nh = frame.multipleH;
    const nv = frame.multipleV;
    const imin = Math.floor(p.qxmin * nh) - 1;
    const imax = Math.ceil(p.qxmax * nh) + 1;
    const jmin = Math.floor(p.qymin * nv) - 1;
    const jmax = Math.ceil(p.qymax * nv) + 1;
    for (let j = jmin; j <= jmax; j += 1) {
      for (let i = imin; i <= imax; i += 1) {
        let cell = clipHalf(p.picture, 1.0, 0.0, (i + 0.5) / nh);
        cell = clipHalf(cell, -1.0, 0.0, -(i - 0.5) / nh);
        cell = clipHalf(cell, 0.0, 1.0, (j + 0.5) / nv);
        cell = clipHalf(cell, 0.0, -1.0, -(j - 0.5) / nv);
        if (cell.length >= 3 && polyArea(cell) > 0.0) p.cells.push({ region: cell, centre: { x: i / nh, y: j / nv } });
      }
    }
  }
  return p;
}

function hardAreaRaw(frame, d, pre, level) {
  if (level <= d.wMin) return 0.0;
  if (level >= d.wMax) return 1.0;

  const picture = pre.picture;
  const detInv = 1.0 / d.scaleX;

  switch (frame.pattern) {
    case PAT_HORIZONTAL:
    case PAT_VERTICAL: {
      const horizontal = frame.pattern === PAT_HORIZONTAL;
      const n = horizontal ? frame.multipleH : frame.multipleV;
      const a = horizontal ? 1.0 : 0.0;
      const b = horizontal ? 0.0 : 1.0;
      if (n === 1) return polyArea(clipHalf(picture, a, b, level - 0.5)) * detInv;

      const lo = (horizontal ? pre.qxmin : pre.qymin) + 0.5;
      const hi = (horizontal ? pre.qxmax : pre.qymax) + 0.5;
      const kmin = Math.floor(lo * n) - 1;
      const kmax = Math.ceil(hi * n) + 1;
      const l = clamp(level, 0.0, 1.0);
      let sum = 0.0;
      for (let k = kmin; k <= kmax; k += 1) {
        const s0 = k / n - 0.5;
        const s1 = (k + l) / n - 0.5;
        let p = clipHalf(picture, -a, -b, -s0);
        p = clipHalf(p, a, b, s1);
        sum += polyArea(p);
      }
      return sum * detInv;
    }

    case PAT_BOX:
    case PAT_DIAMOND:
    case PAT_CIRCLE: {
      const size = frame.pattern === PAT_CIRCLE ? d.norm * Math.sqrt(Math.max(0.0, level)) : d.norm * level;
      if (frame.multipleH === 1 && frame.multipleV === 1) {
        return shapeArea(frame.pattern, picture, { x: 0.0, y: 0.0 }, size) * detInv;
      }
      let sum = 0.0;
      for (const cell of pre.cells) sum += shapeArea(frame.pattern, cell.region, cell.centre, size);
      return sum * detInv;
    }

    case PAT_CLOCK: {
      const n = frame.multipleH;
      const l = clamp(level, 0.0, 1.0);
      const w = TWO_PI * l / n;
      let sum = 0.0;
      for (let k = 0; k < n; k += 1) {
        const a0 = TWO_PI * k / n;
        if (w <= PI) sum += wedgeArea(picture, a0, a0 + w);
        else sum += polyArea(picture) - wedgeArea(picture, a0 + w, a0 + TWO_PI);
      }
      return sum * detInv;
    }

    case PAT_MATRIX: {
      const count = d.matrixCols * d.matrixRows;
      const below = Math.ceil(level * count - 0.5);
      return clamp(below, 0.0, count) / count;
    }

    default:
      return 0.0;
  }
}

function hardArea(frame, d, pre, level) {
  if (frame.reverse) return 1.0 - hardAreaRaw(frame, d, pre, d.wMin + d.wMax - level);
  return hardAreaRaw(frame, d, pre, level);
}

/** Adaptive Simpson, as Waveform.cpp's: the area has kinks, so bisect locally. */
function simpsonRec(f, a, b, fa, fm, fb, whole, eps, depth) {
  const m = 0.5 * (a + b);
  const lm = 0.5 * (a + m);
  const rm = 0.5 * (m + b);
  const flm = f(lm);
  const frm = f(rm);
  const left = (m - a) / 6.0 * (fa + 4.0 * flm + fm);
  const right = (b - m) / 6.0 * (fm + 4.0 * frm + fb);
  const delta = left + right - whole;
  if (depth <= 0 || Math.abs(delta) <= 15.0 * eps) return left + right + delta / 15.0;
  return simpsonRec(f, a, m, fa, flm, fm, left, 0.5 * eps, depth - 1)
    + simpsonRec(f, m, b, fm, frm, fb, right, 0.5 * eps, depth - 1);
}

function simpson(f, a, b, eps) {
  const fa = f(a);
  const fb = f(b);
  const fm = f(0.5 * (a + b));
  const whole = (b - a) / 6.0 * (fa + 4.0 * fm + fb);
  return simpsonRec(f, a, b, fa, fm, fb, whole, eps, 24);
}

function derive(frame) {
  const d = {
    centreX: 0.5, centreY: 0.5, scaleX: 1.0, cosR: 1.0, sinR: 0.0, norm: 1.0,
    wMin: 0.0, wMax: 1.0, unitsPerPixel: 1.0, matrixCols: MATRIX_BASE_COLS, matrixRows: MATRIX_BASE_ROWS,
  };
  const positioned = usesPositioner(frame.pattern);
  d.centreX = positioned ? clamp(frame.centreX, 0.0, 1.0) : 0.5;
  d.centreY = positioned ? clamp(frame.centreY, 0.0, 1.0) : 0.5;
  const pictureAspect = frame.outW / Math.max(1, frame.outH);
  d.scaleX = (frame.aspectComp ? pictureAspect : 1.0) * frame.aspect;
  d.cosR = Math.cos(frame.rotation);
  d.sinR = Math.sin(frame.rotation);
  d.matrixCols = MATRIX_BASE_COLS * frame.multipleH;
  d.matrixRows = MATRIX_BASE_ROWS * frame.multipleV;

  const picture = picturePoly(d);

  const gxx = d.cosR * d.scaleX / frame.outW;
  const gxy = -d.sinR / frame.outH;
  const gyx = d.sinR * d.scaleX / frame.outW;
  const gyy = d.cosR / frame.outH;
  const gradQx = Math.sqrt(gxx * gxx + gxy * gxy);
  const gradQy = Math.sqrt(gyx * gyx + gyy * gyy);

  switch (frame.pattern) {
    case PAT_HORIZONTAL:
    case PAT_VERTICAL: {
      const horizontal = frame.pattern === PAT_HORIZONTAL;
      const n = horizontal ? frame.multipleH : frame.multipleV;
      if (n === 1) {
        d.wMin = 1e300;
        d.wMax = -1e300;
        for (const v of picture) {
          const w = (horizontal ? v.x : v.y) + 0.5;
          d.wMin = Math.min(d.wMin, w);
          d.wMax = Math.max(d.wMax, w);
        }
      } else {
        d.wMin = 0.0;
        d.wMax = 1.0;
      }
      d.unitsPerPixel = (horizontal ? gradQx : gradQy) * n;
      d.norm = 1.0;
      break;
    }

    case PAT_BOX:
    case PAT_DIAMOND:
    case PAT_CIRCLE: {
      if (frame.multipleH === 1 && frame.multipleV === 1) {
        d.norm = 0.0;
        for (const v of picture) d.norm = Math.max(d.norm, normOf(frame.pattern, v.x, v.y));
      } else {
        d.norm = normOf(frame.pattern, 0.5 / frame.multipleH, 0.5 / frame.multipleV);
      }
      d.norm = Math.max(d.norm, 1e-9);
      d.wMin = 0.0;
      d.wMax = 1.0;
      if (frame.pattern === PAT_BOX) d.unitsPerPixel = gradQx / d.norm;
      else if (frame.pattern === PAT_DIAMOND) {
        d.unitsPerPixel = Math.sqrt((gxx + gyx) * (gxx + gyx) + (gxy + gyy) * (gxy + gyy)) / d.norm;
      } else d.unitsPerPixel = Math.sqrt(2.0) * gradQx / d.norm;
      break;
    }

    case PAT_CLOCK:
      d.norm = 1.0;
      d.wMin = 0.0;
      d.wMax = 1.0;
      d.unitsPerPixel = 2.0 * frame.multipleH * gradQx / PI;
      break;

    default:
      d.norm = 1.0;
      d.wMin = 0.0;
      d.wMax = 1.0;
      d.unitsPerPixel = 1.0 / frame.outW;
      break;
  }
  return d;
}

const edgeLevel = (d, position) => d.wMin + clamp(position, 0.0, 1.0) * (d.wMax - d.wMin);

function softArea(frame, d, pre, level, softW) {
  if (!(softW > 0.0)) return hardArea(frame, d, pre, level);

  if (frame.pattern === PAT_MATRIX) {
    const count = d.matrixCols * d.matrixRows;
    const lv = frame.reverse ? d.wMin + d.wMax - level : level;
    let sum = 0.0;
    for (let r = 0; r < count; r += 1) {
      const w = (r + 0.5) / count;
      sum += clamp(0.5 + (lv - w) / softW, 0.0, 1.0);
    }
    const area = sum / count;
    return frame.reverse ? 1.0 - area : area;
  }

  const f = (l) => hardArea(frame, d, pre, l);
  return simpson(f, level - 0.5 * softW, level + 0.5 * softW, 1e-10) / softW;
}

function areaLevel(frame, d, pre, position, softW) {
  const p = clamp(position, 0.0, 1.0);
  let lo = d.wMin - 0.5 * Math.max(softW, 0.0);
  let hi = d.wMax + 0.5 * Math.max(softW, 0.0);
  for (let i = 0; i < 60 && (hi - lo) > AREA_SOLVE_TOLERANCE; i += 1) {
    const mid = 0.5 * (lo + hi);
    if (softArea(frame, d, pre, mid, softW) < p) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

//===========================================================================
// Timing.cpp — the one phase Wipe takes from the clock.
//===========================================================================
function positiveMod(value, period) {
  if (!(period > 0.0)) return 0.0;
  const r = value % period; // fmod: keeps the sign of the numerator, as in C
  return r < 0.0 ? r + period : r;
}
const modPhase = (elapsedSeconds, cyclesPerSecond) => positiveMod(cyclesPerSecond * elapsedSeconds, 1.0);


export {
  f32, clamp01, clamp, PX_MAX, MULTIPLE_MAX,
  PAT_HORIZONTAL, PAT_VERTICAL, PAT_BOX, PAT_DIAMOND, PAT_CIRCLE, PAT_CLOCK, PAT_MATRIX, PAT_COUNT,
  LAW_AREA, LAW_COUNT,
  softnessPxFromParam, borderWidthPxFromParam, borderSoftnessPxFromParam, rotationRadiansFromParam,
  aspectFromParam, modAmountPxFromParam, modFrequencyFromParam, modSpeedHzFromParam, multipleFromParam,
  optionIndex, derive, prepare, edgeLevel, softArea, areaLevel, modPhase,
};
