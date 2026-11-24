/* Golden banana drawn as flat, dithered noise pixels: only the PNG's silhouette is used. A light from the
   upper left gives the banana a bright rim along its top edge that falls off into dark grain, and white
   specks are rise off its upper edges like a flame, swaying and fading. The grain twinkles a little.
   mountBanana(el, { src }) -> { stop() }; `src` is the silhouette PNG's URL. No dependencies. */

const GRID_TARGET = 500; /* cells across the shorter side of the box */
const TILT = (-30 * Math.PI) / 180; /* counter-clockwise tilt of the (mirrored) banana */
const LIGHT_X = -0.45, LIGHT_Y = -0.9; /* direction towards the light (upper left) */
const TICK_MS = 1000 / 14; /* twinkle rate */
const TWINKLE = 0.04; /* fraction of cells re-rolled per tick */
const MAX_PARTICLES = 16000; /* flame specks in flight */
const SWIRL_RADIUS = 0.08; /* cursor reach, as a fraction of the box height */
const SWIRL_DRAG = 2; /* how hard specks are pulled along the cursor's path */
const SWIRL_CURL = 3; /* how hard they whirl around it */
const SWIRL_DAMPING = 1.2; /* per second; lower = stirred specks keep drifting longer */
const STIR_GAIN = 3; /* how hard the cursor stirs the banana's colours */
const STIR_DECAY = 0.9; /* per second; how fast the stirring motion dies down (lower = longer swirl) */
const STIR_RELAX = 0.2; /* per second; how fast stirred colours drift back (lower = marbling lingers) */
const STIR_RETURN = 6; /* px/s; the drift back never slows below this, so the last of it settles */
const STIR_RADIUS = 0.14; /* reach of the stirring inside the banana, as a fraction of the box height */
const STIR_CELL = 2; /* the liquid moves in blocks of this many pixels per side (cheaper, still crisp) */

type RGB = readonly [number, number, number];

/* palette, brightest first: rim, lit, mid, shade, deep */
const RIM: RGB = [255, 244, 184], HI: RGB = [255, 205, 58], MID: RGB = [201, 138, 18], LO: RGB = [107, 63, 6],
  DEEP: RGB = [36, 19, 2];
const FLAME: RGB = [0.42, 0.66, 1]; /* flame speck tint, scaled by each speck's brightness (bright = sky blue) */

function clamp(v: number, a: number, b: number): number {
  return v < a ? a : v > b ? b : v;
}

/* silhouette cells: index into the grid, light falloff, rim strength, dither roll */
interface Cells {
  idx: Int32Array;
  bright: Float32Array;
  rim: Float32Array;
  noise: Float32Array;
}

/* the stirrable colour surface (see initSurface) */
interface Surface {
  mask: Uint8Array;
  cc: number;
  cr: number;
  tone0: Float32Array;
  rim0: Float32Array;
  tone: Float32Array;
  rimF: Float32Array;
  cmask: Uint8Array;
  mapX: Float32Array;
  mapY: Float32Array;
  map2X: Float32Array;
  map2Y: Float32Array;
  u: Float32Array;
  v: Float32Array;
  u2: Float32Array;
  v2: Float32Array;
  live: boolean;
  bx0: number;
  by0: number;
  bx1: number;
  by1: number;
}

/* flame particles and the edge cells that emit them */
interface Flame {
  mask: Uint8Array;
  haloW: number;
  n: number;
  ex: Float32Array;
  ey: Float32Array;
  enx: Float32Array;
  eny: Float32Array;
  x: Float32Array; /* current position, derived from age */
  y: Float32Array;
  ox: Float32Array; /* launch point */
  oy: Float32Array;
  vx: Float32Array;
  v0: Float32Array;
  acc: Float32Array;
  amp: Float32Array;
  freq: Float32Array;
  phase: Float32Array;
  age: Float32Array;
  life: Float32Array;
  bright: Float32Array;
  offX: Float32Array; /* displacement stirred in by the mouse */
  offY: Float32Array;
  sx: Float32Array; /* ...and its velocity */
  sy: Float32Array;
}

export interface BananaHandle {
  stop(): void;
}

export function mountBanana(el: HTMLElement, opts: { src: string }): BananaHandle {
  const canvas = document.createElement('canvas');
  canvas.setAttribute('aria-hidden', 'true');
  canvas.style.cssText =
    'display:block;width:100%;height:100%;image-rendering:optimizeSpeed;' +
    'image-rendering:-moz-crisp-edges;image-rendering:pixelated;background:transparent;';
  el.appendChild(canvas);

  const ctx0 = canvas.getContext('2d');
  if (!ctx0) {
    el.removeChild(canvas);
    return { stop() {} };
  }
  const ctx: CanvasRenderingContext2D = ctx0;

  const mq = typeof window.matchMedia === 'function' ? window.matchMedia('(prefers-reduced-motion: reduce)') : null;
  let reduced = !!(mq && mq.matches);

  const grid = document.createElement('canvas');
  const gctx = grid.getContext('2d')!;
  let img: HTMLImageElement | null = null;
  let cols = 0, rows = 0;
  let out: ImageData | null = null;
  let body: (Cells & Surface) | null = null;
  let rad: Flame | null = null;
  let raf = 0, last = 0, twinkleAt = 0, stopped = false, running = false;

  /* ---------------------------------------------------------------- layout */
  function buildLayout(): void {
    const dpr = clamp(window.devicePixelRatio || 1, 1, 2.5);
    const cw = el.clientWidth || el.offsetWidth || 320;
    const ch = el.clientHeight || el.offsetHeight || cw;
    const devW = Math.round(cw * dpr), devH = Math.round(ch * dpr);

    const cell = Math.max(2, Math.round(Math.min(devW, devH) / GRID_TARGET));
    cols = Math.max(8, Math.round(devW / cell));
    rows = Math.max(8, Math.round(devH / cell));
    canvas.width = cols * cell;
    canvas.height = rows * cell;
    grid.width = cols;
    grid.height = rows;

    /* fit the tilted banana in the box, leaving a margin so the flame has room around it */
    const im = img!;
    const asp = (im.naturalWidth || 1) / (im.naturalHeight || 1);
    const cs = Math.abs(Math.cos(TILT)), sn = Math.abs(Math.sin(TILT));
    const sizeY = Math.min((cols * 0.82) / (asp * cs + sn), (rows * 0.82) / (asp * sn + cs));
    const mask = buildMask(im, sizeY * asp, sizeY);
    buildCells(mask, sizeY);
    out = gctx.createImageData(cols, rows);
  }

  /* Silhouette of the PNG, mirrored left-right and tilted TILT about the box centre. */
  function buildMask(im: HTMLImageElement, w: number, h: number): Uint8Array {
    const c = document.createElement('canvas');
    c.width = cols;
    c.height = rows;
    const g = c.getContext('2d')!;
    g.translate(cols / 2, rows * 0.56); /* a little below centre so the stem and its flame clear the top */
    g.rotate(TILT);
    g.scale(-1, 1);
    g.drawImage(im, -w / 2, -h / 2, w, h);
    const a = g.getImageData(0, 0, cols, rows).data;
    const mask = new Uint8Array(cols * rows);
    for (let i = 0; i < mask.length; i++) mask[i] = a[i * 4 + 3]! > 120 ? 1 : 0;
    /* drop stray single cells so the edge reads clean */
    for (let y = 1; y < rows - 1; y++) {
      for (let x = 1; x < cols - 1; x++) {
        const i = y * cols + x;
        if (mask[i] && !(mask[i - 1]! + mask[i + 1]! + mask[i - cols]! + mask[i + cols]!)) mask[i] = 0;
      }
    }
    return mask;
  }

  /* Steps from (x, y) along (dx, dy) until the mask value differs from `inside`; capped at max. */
  function march(mask: Uint8Array, x: number, y: number, dx: number, dy: number, inside: number, max: number): number {
    const px = x + 0.5, py = y + 0.5;
    for (let s = 1; s <= max; s++) {
      const ix = Math.floor(px + dx * s), iy = Math.floor(py + dy * s);
      if (ix < 0 || iy < 0 || ix >= cols || iy >= rows) return inside ? s : max;
      if (mask[iy * cols + ix] !== inside) return s;
    }
    return max;
  }

  /* Distance (in cells) from every cell to the nearest banana cell: two-pass 3-4 chamfer, 0 inside. */
  function distanceOut(mask: Uint8Array): Float32Array {
    const dist = new Float32Array(cols * rows), BIG = 1e9;
    for (let i = 0; i < dist.length; i++) dist[i] = mask[i] ? 0 : BIG;
    for (let y = 0; y < rows; y++) {
      for (let x = 0; x < cols; x++) {
        const i = y * cols + x;
        if (!dist[i]) continue;
        if (x > 0) dist[i] = Math.min(dist[i]!, dist[i - 1]! + 3);
        if (y > 0) {
          dist[i] = Math.min(dist[i]!, dist[i - cols]! + 3);
          if (x > 0) dist[i] = Math.min(dist[i]!, dist[i - cols - 1]! + 4);
          if (x < cols - 1) dist[i] = Math.min(dist[i]!, dist[i - cols + 1]! + 4);
        }
      }
    }
    for (let y = rows - 1; y >= 0; y--) {
      for (let x = cols - 1; x >= 0; x--) {
        const i = y * cols + x;
        if (!dist[i]) continue;
        if (x < cols - 1) dist[i] = Math.min(dist[i]!, dist[i + 1]! + 3);
        if (y < rows - 1) {
          dist[i] = Math.min(dist[i]!, dist[i + cols]! + 3);
          if (x < cols - 1) dist[i] = Math.min(dist[i]!, dist[i + cols + 1]! + 4);
          if (x > 0) dist[i] = Math.min(dist[i]!, dist[i + cols - 1]! + 4);
        }
      }
    }
    for (let i = 0; i < dist.length; i++) dist[i] = dist[i]! / 3;
    return dist;
  }

  function buildCells(mask: Uint8Array, sizeY: number): void {
    const len = Math.hypot(LIGHT_X, LIGHT_Y), lx = LIGHT_X / len, ly = LIGHT_Y / len;
    const falloff = Math.max(4, sizeY * 0.3);
    const rimW = Math.max(1.5, sizeY * 0.035);
    const haloW = Math.max(6, sizeY * 0.22); /* how high the flame reaches */
    const bIdx: number[] = [], bBright: number[] = [], bRim: number[] = [];
    const eX: number[] = [], eY: number[] = [], eNx: number[] = [], eNy: number[] = [];
    const maxIn = Math.ceil(sizeY * 1.5);
    const away = distanceOut(mask);

    for (let y = 0; y < rows; y++) {
      for (let x = 0; x < cols; x++) {
        const i = y * cols + x;
        if (mask[i]) {
          /* depth below the lit edge, measured towards the light */
          const d = march(mask, x, y, lx, ly, 1, maxIn) - 0.5;
          bIdx.push(i);
          bBright.push(0.06 + 0.94 * Math.exp(-d / falloff));
          bRim.push(clamp(1 - d / rimW, 0, 1));
        } else {
          /* cells touching the banana's upward- or sideways-facing edge emit the flame; downward-facing ones
             are skipped, since specks rising from the underside would just pass behind the banana */
          if (away[i]! <= 1.5 && x > 1 && y > 1 && x < cols - 2 && y < rows - 2) {
            const nx = away[i + 2]! - away[i - 2]!, ny = away[i + 2 * cols]! - away[i - 2 * cols]!;
            const nl = Math.hypot(nx, ny);
            if (nl > 0 && ny / nl < 0.3) {
              eX.push(x + 0.5);
              eY.push(y + 0.5);
              eNx.push(nx / nl);
              eNy.push(ny / nl);
            }
          }
        }
      }
    }
    const cells: Cells = {
      idx: Int32Array.from(bIdx),
      bright: Float32Array.from(bBright),
      rim: Float32Array.from(bRim),
      noise: new Float32Array(bIdx.length),
    };
    reroll(cells.noise, 1);
    body = { ...cells, ...initSurface(mask, cells) };
    buildEmitter(mask, eX, eY, eNx, eNy, haloW);
  }

  /* ---------------------------------------------------------------- colour stirring */
  /* The cursor stirs a velocity field inside the silhouette. Every banana block remembers where its colour came
     from (a source map, starting as the identity); each frame that map, and the motion itself, are carried along
     the flow, and every pixel shows the original shade found at its source. Dark shade from the underside
     therefore marbles into the gold like two liquids being mixed, in crisp streaks: shades are only ever looked
     up, never blended or re-rounded frame after frame. The map then relaxes back to the identity, so the banana
     slowly settles. The liquid runs on a coarser grid (STIR_CELL pixels per block side) to keep a full-banana
     stir cheap; only a box around the stirred area is simulated, and it switches off once everything settles.
     Velocities and map coordinates are in pixel units throughout. */
  function initSurface(mask: Uint8Array, cells: Cells): Surface {
    const F = STIR_CELL, cc = Math.ceil(cols / F), cr = Math.ceil(rows / F), n = cc * cr;
    const tone0 = new Float32Array(cols * rows), rim0 = new Float32Array(cols * rows);
    for (let b = 0; b < cells.idx.length; b++) {
      tone0[cells.idx[b]!] = cells.bright[b]!;
      rim0[cells.idx[b]!] = cells.rim[b]!;
    }
    const cmask = new Uint8Array(n); /* block touches the banana */
    for (let b = 0; b < cells.idx.length; b++) {
      const i = cells.idx[b]!;
      cmask[((((i / cols) | 0) / F) | 0) * cc + (((i % cols) / F) | 0)] = 1;
    }
    const mapX = new Float32Array(n), mapY = new Float32Array(n);
    for (let c = 0; c < n; c++) {
      mapX[c] = (c % cc) * F;
      mapY[c] = ((c / cc) | 0) * F;
    }
    return {
      mask, cc, cr, tone0, rim0,
      tone: Float32Array.from(tone0), rimF: Float32Array.from(rim0),
      cmask, mapX, mapY,
      map2X: Float32Array.from(mapX), map2Y: Float32Array.from(mapY),
      u: new Float32Array(n), v: new Float32Array(n), u2: new Float32Array(n), v2: new Float32Array(n),
      live: false, bx0: 0, by0: 0, bx1: 0, by1: 0,
    };
  }

  function stepSurface(dt: number): void {
    const s = body!, m = mouse, F = STIR_CELL, cc = s.cc, cr = s.cr, cm = s.cmask;
    const U = s.u, V = s.v, U2 = s.u2, V2 = s.v2, mX = s.mapX, mY = s.mapY, m2X = s.map2X, m2Y = s.map2Y;
    const speed = Math.hypot(m.vx, m.vy);
    if (m.active && speed > 1) {
      const R = Math.max(6, rows * STIR_RADIUS);
      let x0 = Math.max(0, Math.floor((m.x - R) / F)), x1 = Math.min(cc - 1, Math.ceil((m.x + R) / F));
      let y0 = Math.max(0, Math.floor((m.y - R) / F)), y1 = Math.min(cr - 1, Math.ceil((m.y + R) / F));
      let touched = false;
      for (let y = y0; y <= y1; y++) {
        for (let x = x0; x <= x1; x++) {
          const c = y * cc + x;
          if (!cm[c]) continue;
          const dx = (x + 0.5) * F - m.x, dy = (y + 0.5) * F - m.y, dist = Math.sqrt(dx * dx + dy * dy);
          if (dist >= R || dist < 0.01) continue;
          /* drag along the cursor's path plus a curl on each side of it: the wake of a stirring stick */
          const w = (1 - dist / R) * (1 - dist / R) * dt * STIR_GAIN;
          const side = m.vx * dy - m.vy * dx > 0 ? 1 : -1;
          U[c] = U[c]! + w * (m.vx * SWIRL_DRAG - (dy / dist) * speed * SWIRL_CURL * side);
          V[c] = V[c]! + w * (m.vy * SWIRL_DRAG + (dx / dist) * speed * SWIRL_CURL * side);
          touched = true;
        }
      }
      if (touched) {
        /* pad the simulated box: the stirred motion travels on after the cursor has gone */
        const pad = Math.ceil(R / F);
        x0 = Math.max(0, x0 - pad);
        y0 = Math.max(0, y0 - pad);
        x1 = Math.min(cc - 1, x1 + pad);
        y1 = Math.min(cr - 1, y1 + pad);
        if (!s.live) {
          s.bx0 = x0; s.by0 = y0; s.bx1 = x1; s.by1 = y1; s.live = true;
        } else {
          s.bx0 = Math.min(s.bx0, x0); s.by0 = Math.min(s.by0, y0);
          s.bx1 = Math.max(s.bx1, x1); s.by1 = Math.max(s.by1, y1);
        }
      }
    }
    if (!s.live) return;

    const decay = Math.exp(-dt * STIR_DECAY), relax = 1 - Math.exp(-dt * STIR_RELAX);
    let maxV = 0, maxD = 0;
    const k = dt / F; /* pixels/s -> blocks per step */
    for (let y = s.by0; y <= s.by1; y++) {
      for (let x = s.bx0; x <= s.bx1; x++) {
        const c = y * cc + x;
        if (!cm[c]) continue;
        const u = U[c]!, v = V[c]!;
        const sp = Math.abs(u) + Math.abs(v);
        if (sp > maxV) maxV = sp;
        /* the liquid now here was at (x - u dt, y - v dt): inherit that spot's source and motion, interpolated */
        const fx = clamp(x - u * k, 0, cc - 1.001), fy = clamp(y - v * k, 0, cr - 1.001);
        const ix = fx | 0, iy = fy | 0, ax = fx - ix, ay = fy - iy, bx = 1 - ax, by = 1 - ay;
        const c00 = iy * cc + ix, c10 = c00 + 1, c01 = c00 + cc, c11 = c01 + 1;
        m2X[c] = (mX[c00]! * bx + mX[c10]! * ax) * by + (mX[c01]! * bx + mX[c11]! * ax) * ay;
        m2Y[c] = (mY[c00]! * bx + mY[c10]! * ax) * by + (mY[c01]! * bx + mY[c11]! * ax) * ay;
        U2[c] = (U[c00]! * bx + U[c10]! * ax) * by + (U[c01]! * bx + U[c11]! * ax) * ay;
        V2[c] = (V[c00]! * bx + V[c10]! * ax) * by + (V[c01]! * bx + V[c11]! * ax) * ay;
      }
    }
    const mask = s.mask, tone = s.tone, rimF = s.rimF, tone0 = s.tone0, rim0 = s.rim0;
    for (let y = s.by0; y <= s.by1; y++) {
      for (let x = s.bx0; x <= s.bx1; x++) {
        const c = y * cc + x;
        if (!cm[c]) continue;
        U[c] = U2[c]! * decay;
        V[c] = V2[c]! * decay;
        const hx = x * F, hy = y * F; /* this block's home, in pixels */
        /* drift back home: a fraction of the distance, but never slower than STIR_RETURN, so it finishes */
        const ofx = m2X[c]! - hx, ofy = m2Y[c]! - hy, off = Math.sqrt(ofx * ofx + ofy * ofy);
        const back = Math.max(off * relax, STIR_RETURN * dt), keep = off > back ? 1 - back / off : 0;
        const sx = hx + ofx * keep, sy = hy + ofy * keep;
        mX[c] = sx;
        mY[c] = sy;
        const dev = off * keep;
        if (dev > maxD) maxD = dev;
        /* each pixel of the block shows the original shade at its source; a source outside the banana keeps
           the pixel's own shade */
        for (let oy = 0; oy < F; oy++) {
          const py = hy + oy;
          if (py >= rows) break;
          const ry = Math.round(sy + oy);
          for (let ox = 0; ox < F; ox++) {
            const px = hx + ox;
            if (px >= cols) break;
            const p = py * cols + px;
            if (!mask[p]) continue;
            const rx = Math.round(sx + ox);
            let src = p;
            if (rx >= 0 && ry >= 0 && rx < cols && ry < rows && mask[ry * cols + rx]) src = ry * cols + rx;
            tone[p] = tone0[src]!;
            rimF[p] = rim0[src]!;
          }
        }
      }
    }
    if (maxV < 0.05 && maxD < 0.3) {
      for (let y = s.by0; y <= s.by1; y++) {
        for (let x = s.bx0; x <= s.bx1; x++) {
          const c = y * cc + x;
          mX[c] = m2X[c] = x * F;
          mY[c] = m2Y[c] = y * F;
          U[c] = V[c] = 0;
        }
      }
      const px0 = s.bx0 * F, px1 = Math.min(cols, (s.bx1 + 1) * F), py0 = s.by0 * F, py1 = Math.min(rows, (s.by1 + 1) * F);
      for (let y = py0; y < py1; y++) {
        for (let x = px0; x < px1; x++) {
          const c = y * cols + x;
          tone[c] = tone0[c]!;
          rimF[c] = rim0[c]!;
        }
      }
      s.live = false;
    }
  }

  /* ---------------------------------------------------------------- flame */
  function buildEmitter(mask: Uint8Array, eX: number[], eY: number[], eNx: number[], eNy: number[], haloW: number): void {
    const n = clamp(eX.length * 3, 600, MAX_PARTICLES);
    rad = {
      mask, haloW, n,
      ex: Float32Array.from(eX), ey: Float32Array.from(eY), enx: Float32Array.from(eNx), eny: Float32Array.from(eNy),
      x: new Float32Array(n), y: new Float32Array(n),
      ox: new Float32Array(n), oy: new Float32Array(n),
      vx: new Float32Array(n), v0: new Float32Array(n), acc: new Float32Array(n),
      amp: new Float32Array(n), freq: new Float32Array(n), phase: new Float32Array(n),
      age: new Float32Array(n), life: new Float32Array(n), bright: new Float32Array(n),
      offX: new Float32Array(n), offY: new Float32Array(n),
      sx: new Float32Array(n), sy: new Float32Array(n),
    };
    /* start mid-flight so the page opens on a full flame rather than an empty sky */
    for (let k = 0; k < n; k++) emit(k, Math.random());
  }

  /* (Re)launch particle k from a random edge cell, `progress` (0..1) of the way through its life. */
  function emit(k: number, progress: number): void {
    const r = rad!, m = r.ex.length;
    if (!m) {
      r.life[k] = 1;
      r.age[k] = 1;
      return;
    }
    const e = (Math.random() * m) | 0;
    const life = 5 + Math.random() * 6;
    const rise = r.haloW * (1.2 + Math.random() * 2.8); /* how high this one climbs before it fades out */
    r.ox[k] = r.ex[e]!;
    r.oy[k] = r.ey[e]!;
    r.v0[k] = (0.6 * rise) / life; /* starts slow and speeds up as it rises, like heat */
    r.acc[k] = (0.8 * rise) / (life * life);
    r.vx[k] = (r.enx[e]! * 0.15 * rise) / life; /* a slight push off the edge it left from */
    r.amp[k] = r.haloW * (0.05 + Math.random() * 0.15); /* side-to-side sway */
    r.freq[k] = 0.6 + Math.random();
    r.phase[k] = Math.random() * Math.PI * 2;
    r.life[k] = life;
    r.age[k] = life * progress;
    r.bright[k] = 170 + Math.random() * 85;
    r.offX[k] = r.offY[k] = r.sx[k] = r.sy[k] = 0;
    place(k);
  }

  function place(k: number): void {
    const r = rad!, t = r.age[k]!;
    r.x[k] =
      r.ox[k]! + r.vx[k]! * t + r.amp[k]! * (Math.sin(r.freq[k]! * t + r.phase[k]!) - Math.sin(r.phase[k]!)) + r.offX[k]!;
    r.y[k] = r.oy[k]! - (r.v0[k]! * t + 0.5 * r.acc[k]! * t * t) + r.offY[k]!;
  }

  function stepParticles(dt: number): void {
    const r = rad!, m = mouse;
    const speed = Math.hypot(m.vx, m.vy);
    const stir = m.active && speed > 1;
    const R = Math.max(6, rows * SWIRL_RADIUS), damp = Math.exp(-dt * SWIRL_DAMPING);
    for (let k = 0; k < r.n; k++) {
      r.age[k] = r.age[k]! + dt;
      if (stir) {
        /* specks near the cursor are dragged along its path and curled into a whirl on each side of it */
        const dx = r.x[k]! - m.x, dy = r.y[k]! - m.y, dist = Math.sqrt(dx * dx + dy * dy);
        if (dist < R && dist > 0.01) {
          const w = (1 - dist / R) * (1 - dist / R) * dt;
          const side = m.vx * dy - m.vy * dx > 0 ? 1 : -1;
          r.sx[k] = r.sx[k]! + w * (m.vx * SWIRL_DRAG - (dy / dist) * speed * SWIRL_CURL * side);
          r.sy[k] = r.sy[k]! + w * (m.vy * SWIRL_DRAG + (dx / dist) * speed * SWIRL_CURL * side);
        }
      }
      if (r.sx[k] || r.sy[k]) {
        r.offX[k] = r.offX[k]! + r.sx[k]! * dt;
        r.offY[k] = r.offY[k]! + r.sy[k]! * dt;
        r.sx[k] = r.sx[k]! * damp;
        r.sy[k] = r.sy[k]! * damp;
        if (Math.abs(r.sx[k]!) + Math.abs(r.sy[k]!) < 0.01) r.sx[k] = r.sy[k] = 0;
      }
      place(k);
      if (r.age[k]! >= r.life[k]! || r.x[k]! < 0 || r.y[k]! < 0 || r.x[k]! >= cols || r.y[k]! >= rows) emit(k, 0);
    }
  }

  /* ---------------------------------------------------------------- mouse */
  const mouse = { x: 0, y: 0, vx: 0, vy: 0, t: 0, active: false };

  function onPointerMove(e: PointerEvent): void {
    const b = canvas.getBoundingClientRect();
    if (!b.width || !b.height) return;
    const gx = ((e.clientX - b.left) / b.width) * cols, gy = ((e.clientY - b.top) / b.height) * rows;
    const dtm = (e.timeStamp - mouse.t) / 1000;
    if (mouse.active && dtm > 0 && dtm < 0.2) {
      mouse.vx = mouse.vx * 0.5 + ((gx - mouse.x) / dtm) * 0.5;
      mouse.vy = mouse.vy * 0.5 + ((gy - mouse.y) / dtm) * 0.5;
    }
    mouse.x = gx;
    mouse.y = gy;
    mouse.t = e.timeStamp;
    mouse.active = true;
  }

  function onPointerLeave(): void {
    mouse.active = false;
    mouse.vx = mouse.vy = 0;
  }

  function reroll(noise: Float32Array, fraction: number): void {
    if (fraction >= 1) {
      for (let i = 0; i < noise.length; i++) noise[i] = Math.random();
      return;
    }
    const n = Math.round(noise.length * fraction);
    for (let k = 0; k < n; k++) noise[(Math.random() * noise.length) | 0] = Math.random();
  }

  /* ---------------------------------------------------------------- frame */
  function put(d: Uint8ClampedArray, j: number, c: RGB, a: number): void {
    d[j] = c[0];
    d[j + 1] = c[1];
    d[j + 2] = c[2];
    d[j + 3] = a;
  }

  /* Palette colour for banana pixel i from its (possibly stirred) tone and rim, dithered by its noise roll. */
  function surfaceColour(s: Cells & Surface, i: number): RGB {
    const c = s.idx[i]!, r = s.noise[i]!;
    if (r < s.rimF[c]! * 0.85) return RIM;
    const v = s.tone[c]! + (r - 0.5) * 0.6;
    return v > 0.78 ? HI : v > 0.5 ? MID : v > 0.24 ? LO : DEEP;
  }

  function draw(): void {
    const s = body!, o = out!;
    const d = o.data;
    d.fill(0);

    for (let i = 0; i < s.idx.length; i++) put(d, s.idx[i]! * 4, surfaceColour(s, i), 255);

    /* flame: specks fade as they rise */
    const p = rad!;
    for (let i = 0; i < p.n; i++) {
      const c = (p.y[i]! | 0) * cols + (p.x[i]! | 0);
      if (p.mask[c]) continue;
      const v = (p.bright[i]! * Math.pow(1 - p.age[i]! / p.life[i]!, 0.8)) | 0;
      if (v < 40) continue;
      const j = c * 4;
      if (d[j + 3] && d[j + 2]! >= v) continue;
      d[j] = (v * FLAME[0]) | 0;
      d[j + 1] = (v * FLAME[1]) | 0;
      d[j + 2] = (v * FLAME[2]) | 0;
      d[j + 3] = 255;
    }

    gctx.putImageData(o, 0, 0);
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(grid, 0, 0, canvas.width, canvas.height);
  }

  function frame(now: number): void {
    if (stopped) return;
    const dt = last ? Math.min(0.05, (now - last) / 1000) : 0;
    last = now;
    twinkleAt += dt * 1000;
    if (twinkleAt >= TICK_MS) {
      twinkleAt = 0;
      reroll(body!.noise, TWINKLE);
    }
    /* the cursor's speed fades out once it stops moving, so a resting mouse stirs nothing */
    const fade = Math.exp(-dt * 8);
    mouse.vx *= fade;
    mouse.vy *= fade;
    stepParticles(dt);
    stepSurface(dt);
    draw();
    raf = requestAnimationFrame(frame);
  }

  /* ---------------------------------------------------------------- lifecycle */
  function start(): void {
    if (running || stopped || reduced) return;
    running = true;
    last = 0;
    raf = requestAnimationFrame(frame);
  }

  function halt(): void {
    running = false;
    if (raf) cancelAnimationFrame(raf);
    raf = 0;
  }

  function onVisibility(): void {
    if (document.hidden) halt();
    else start();
  }

  let resizeTimer = 0;
  function onResize(): void {
    window.clearTimeout(resizeTimer);
    resizeTimer = window.setTimeout(() => {
      if (stopped || !img) return;
      buildLayout();
      draw();
    }, 120);
  }

  let ro: ResizeObserver | null = null;

  function onMotionPref(): void {
    reduced = !!(mq && mq.matches);
    if (reduced) halt();
    else start();
  }

  function stop(): void {
    stopped = true;
    halt();
    window.clearTimeout(resizeTimer);
    document.removeEventListener('visibilitychange', onVisibility);
    window.removeEventListener('resize', onResize);
    window.removeEventListener('pointermove', onPointerMove);
    document.documentElement.removeEventListener('pointerleave', onPointerLeave);
    if (ro) ro.disconnect();
    if (mq && typeof mq.removeEventListener === 'function') mq.removeEventListener('change', onMotionPref);
    else if (mq && typeof mq.removeListener === 'function') mq.removeListener(onMotionPref);
    if (canvas.parentNode) canvas.parentNode.removeChild(canvas);
    img = null;
    body = null;
    rad = null;
    out = null;
  }

  function boot(): void {
    if (stopped) return;
    buildLayout();
    draw();
    document.addEventListener('visibilitychange', onVisibility);
    window.addEventListener('resize', onResize);
    /* the canvas ignores the pointer (the form sits on top), so watch the whole window instead */
    window.addEventListener('pointermove', onPointerMove, { passive: true });
    document.documentElement.addEventListener('pointerleave', onPointerLeave);
    if (typeof ResizeObserver === 'function') {
      ro = new ResizeObserver(onResize);
      ro.observe(el);
    }
    if (mq) {
      if (typeof mq.addEventListener === 'function') mq.addEventListener('change', onMotionPref);
      else if (typeof mq.addListener === 'function') mq.addListener(onMotionPref);
    }
    start();
  }

  img = new Image();
  img.onload = boot;
  img.onerror = () => {
    /* leave the canvas empty rather than break layout */
  };
  img.src = opts.src;

  return { stop };
}
