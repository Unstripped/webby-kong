/* Golden banana drawn as flat, dithered noise pixels: only the PNG's silhouette is used. A light from the
   upper left gives the banana a bright rim along its top edge that falls off into dark grain, and white
   specks are rise off its upper edges like a flame, swaying and fading. The grain twinkles a little.
   Exposes window.Banana = { mount(el) -> { stop() } }. No dependencies. */
(function () {
  'use strict';

  var SCRIPT_SRC = (document.currentScript && document.currentScript.src) || '';
  var ASSET = SCRIPT_SRC.replace(/[^\/]*$/, '') + 'banana.png';
  var GRID_TARGET = 500;          /* cells across the shorter side of the box */
  var TILT = -30 * Math.PI / 180;    /* counter-clockwise tilt of the (mirrored) banana */
  var LIGHT_X = -0.45, LIGHT_Y = -0.9; /* direction towards the light (upper left) */
  var TICK_MS = 1000 / 14;        /* twinkle rate */
  var TWINKLE = 0.04;             /* fraction of cells re-rolled per tick */
  var MAX_PARTICLES = 16000;      /* flame specks in flight */
  var SWIRL_RADIUS = 0.08;        /* cursor reach, as a fraction of the box height */
  var SWIRL_DRAG = 2;             /* how hard specks are pulled along the cursor's path */
  var SWIRL_CURL = 3;             /* how hard they whirl around it */
  var SWIRL_DAMPING = 1.2;        /* per second; lower = stirred specks keep drifting longer */
  var STIR_GAIN = 3;              /* how hard the cursor stirs the banana's colours */
  var STIR_DECAY = 0.9;           /* per second; how fast the stirring motion dies down (lower = longer swirl) */
  var STIR_RELAX = 0.2;           /* per second; how fast stirred colours drift back (lower = marbling lingers) */
  var STIR_RETURN = 6;            /* px/s; the drift back never slows below this, so the last of it settles */
  var STIR_RADIUS = 0.14;         /* reach of the stirring inside the banana, as a fraction of the box height */
  var STIR_CELL = 2;              /* the liquid moves in blocks of this many pixels per side (cheaper, still crisp) */

  /* palette, brightest first: rim, lit, mid, shade, deep */
  var RIM = [255, 244, 184], HI = [255, 205, 58], MID = [201, 138, 18], LO = [107, 63, 6], DEEP = [36, 19, 2];
  var FLAME = [0.42, 0.66, 1];    /* flame speck tint, scaled by each speck's brightness (bright = sky blue) */

  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

  function mount(el) {
    if (!el) return { stop: function () {} };

    var canvas = document.createElement('canvas');
    canvas.setAttribute('aria-hidden', 'true');
    canvas.style.cssText = 'display:block;width:100%;height:100%;image-rendering:optimizeSpeed;' +
      'image-rendering:-moz-crisp-edges;image-rendering:pixelated;background:transparent;';
    el.appendChild(canvas);

    var ctx = canvas.getContext('2d');
    if (!ctx) { el.removeChild(canvas); return { stop: function () {} }; }

    var mq = window.matchMedia ? window.matchMedia('(prefers-reduced-motion: reduce)') : null;
    var reduced = !!(mq && mq.matches);

    var grid = document.createElement('canvas');
    var gctx = grid.getContext('2d');
    var img = null;
    var cols = 0, rows = 0;
    var out = null;
    var body = null;   /* { idx, bright, rim, noise } for silhouette cells */
    var rad = null;    /* flame particles and the edge cells that emit them */
    var raf = 0, last = 0, twinkleAt = 0, stopped = false, running = false;

    /* ---------------------------------------------------------------- layout */
    function buildLayout() {
      var dpr = clamp(window.devicePixelRatio || 1, 1, 2.5);
      var cw = el.clientWidth || el.offsetWidth || 320;
      var ch = el.clientHeight || el.offsetHeight || cw;
      var devW = Math.round(cw * dpr), devH = Math.round(ch * dpr);

      var cell = Math.max(2, Math.round(Math.min(devW, devH) / GRID_TARGET));
      cols = Math.max(8, Math.round(devW / cell));
      rows = Math.max(8, Math.round(devH / cell));
      canvas.width = cols * cell;
      canvas.height = rows * cell;
      grid.width = cols;
      grid.height = rows;

      /* fit the tilted banana in the box, leaving a margin so the flame has room around it */
      var asp = (img.naturalWidth || 1) / (img.naturalHeight || 1);
      var cs = Math.abs(Math.cos(TILT)), sn = Math.abs(Math.sin(TILT));
      var sizeY = Math.min(cols * 0.82 / (asp * cs + sn), rows * 0.82 / (asp * sn + cs));
      var mask = buildMask(sizeY * asp, sizeY);
      buildCells(mask, sizeY);
      out = gctx.createImageData(cols, rows);
    }

    /* Silhouette of the PNG, mirrored left-right and tilted TILT about the box centre. */
    function buildMask(w, h) {
      var c = document.createElement('canvas');
      c.width = cols; c.height = rows;
      var g = c.getContext('2d');
      g.translate(cols / 2, rows * 0.56);   /* a little below centre so the stem and its flame clear the top */
      g.rotate(TILT);
      g.scale(-1, 1);
      g.drawImage(img, -w / 2, -h / 2, w, h);
      var a = g.getImageData(0, 0, cols, rows).data;
      var mask = new Uint8Array(cols * rows);
      for (var i = 0; i < mask.length; i++) mask[i] = a[i * 4 + 3] > 120 ? 1 : 0;
      /* drop stray single cells so the edge reads clean */
      for (var y = 1; y < rows - 1; y++) {
        for (var x = 1; x < cols - 1; x++) {
          i = y * cols + x;
          if (mask[i] && !(mask[i - 1] + mask[i + 1] + mask[i - cols] + mask[i + cols])) mask[i] = 0;
        }
      }
      return mask;
    }

    /* Steps from (x, y) along (dx, dy) until the mask value differs from `inside`; capped at max. */
    function march(mask, x, y, dx, dy, inside, max) {
      var px = x + 0.5, py = y + 0.5;
      for (var s = 1; s <= max; s++) {
        var ix = Math.floor(px + dx * s), iy = Math.floor(py + dy * s);
        if (ix < 0 || iy < 0 || ix >= cols || iy >= rows) return inside ? s : max;
        if (mask[iy * cols + ix] !== inside) return s;
      }
      return max;
    }

    /* Distance (in cells) from every cell to the nearest banana cell: two-pass 3-4 chamfer, 0 inside. */
    function distanceOut(mask) {
      var dist = new Float32Array(cols * rows), BIG = 1e9, x, y, i;
      for (i = 0; i < dist.length; i++) dist[i] = mask[i] ? 0 : BIG;
      for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
          i = y * cols + x;
          if (!dist[i]) continue;
          if (x > 0) dist[i] = Math.min(dist[i], dist[i - 1] + 3);
          if (y > 0) {
            dist[i] = Math.min(dist[i], dist[i - cols] + 3);
            if (x > 0) dist[i] = Math.min(dist[i], dist[i - cols - 1] + 4);
            if (x < cols - 1) dist[i] = Math.min(dist[i], dist[i - cols + 1] + 4);
          }
        }
      }
      for (y = rows - 1; y >= 0; y--) {
        for (x = cols - 1; x >= 0; x--) {
          i = y * cols + x;
          if (!dist[i]) continue;
          if (x < cols - 1) dist[i] = Math.min(dist[i], dist[i + 1] + 3);
          if (y < rows - 1) {
            dist[i] = Math.min(dist[i], dist[i + cols] + 3);
            if (x < cols - 1) dist[i] = Math.min(dist[i], dist[i + cols + 1] + 4);
            if (x > 0) dist[i] = Math.min(dist[i], dist[i + cols - 1] + 4);
          }
        }
      }
      for (i = 0; i < dist.length; i++) dist[i] /= 3;
      return dist;
    }

    function buildCells(mask, sizeY) {
      var len = Math.hypot(LIGHT_X, LIGHT_Y), lx = LIGHT_X / len, ly = LIGHT_Y / len;
      var falloff = Math.max(4, sizeY * 0.3);
      var rimW = Math.max(1.5, sizeY * 0.035);
      var haloW = Math.max(6, sizeY * 0.22);   /* how high the flame reaches */
      var bIdx = [], bBright = [], bRim = [], eX = [], eY = [], eNx = [], eNy = [];
      var maxIn = Math.ceil(sizeY * 1.5);
      var away = distanceOut(mask);

      for (var y = 0; y < rows; y++) {
        for (var x = 0; x < cols; x++) {
          var i = y * cols + x;
          if (mask[i]) {
            /* depth below the lit edge, measured towards the light */
            var d = march(mask, x, y, lx, ly, 1, maxIn) - 0.5;
            bIdx.push(i);
            bBright.push(0.06 + 0.94 * Math.exp(-d / falloff));
            bRim.push(clamp(1 - d / rimW, 0, 1));
          } else {
            /* cells touching the banana's upward- or sideways-facing edge emit the flame; downward-facing ones
               are skipped, since specks rising from the underside would just pass behind the banana */
            if (away[i] <= 1.5 && x > 1 && y > 1 && x < cols - 2 && y < rows - 2) {
              var nx = away[i + 2] - away[i - 2], ny = away[i + 2 * cols] - away[i - 2 * cols];
              var nl = Math.hypot(nx, ny);
              if (nl > 0 && ny / nl < 0.3) eX.push(x + 0.5), eY.push(y + 0.5), eNx.push(nx / nl), eNy.push(ny / nl);
            }
          }
        }
      }
      body = { idx: Int32Array.from(bIdx), bright: Float32Array.from(bBright), rim: Float32Array.from(bRim),
        noise: new Float32Array(bIdx.length) };
      reroll(body.noise, 1);
      initSurface(mask);
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
    function initSurface(mask) {
      var s = body, F = STIR_CELL, cc = Math.ceil(cols / F), cr = Math.ceil(rows / F), n = cc * cr, b, c;
      s.mask = mask; s.cc = cc; s.cr = cr;
      s.tone0 = new Float32Array(cols * rows); s.rim0 = new Float32Array(cols * rows);
      for (b = 0; b < s.idx.length; b++) { s.tone0[s.idx[b]] = s.bright[b]; s.rim0[s.idx[b]] = s.rim[b]; }
      s.tone = Float32Array.from(s.tone0); s.rimF = Float32Array.from(s.rim0);
      s.cmask = new Uint8Array(n);                     /* block touches the banana */
      for (b = 0; b < s.idx.length; b++) {
        var i = s.idx[b];
        s.cmask[(((i / cols) | 0) / F | 0) * cc + ((i % cols) / F | 0)] = 1;
      }
      s.mapX = new Float32Array(n); s.mapY = new Float32Array(n);
      for (c = 0; c < n; c++) { s.mapX[c] = (c % cc) * F; s.mapY[c] = ((c / cc) | 0) * F; }
      s.map2X = Float32Array.from(s.mapX); s.map2Y = Float32Array.from(s.mapY);
      s.u = new Float32Array(n); s.v = new Float32Array(n); s.u2 = new Float32Array(n); s.v2 = new Float32Array(n);
      s.live = false; s.bx0 = s.by0 = s.bx1 = s.by1 = 0;
    }

    function stepSurface(dt) {
      var s = body, m = mouse, F = STIR_CELL, cc = s.cc, cr = s.cr, cm = s.cmask;
      var U = s.u, V = s.v, U2 = s.u2, V2 = s.v2, mX = s.mapX, mY = s.mapY, m2X = s.map2X, m2Y = s.map2Y;
      var speed = Math.hypot(m.vx, m.vy), x, y, c;
      if (m.active && speed > 1) {
        var R = Math.max(6, rows * STIR_RADIUS);
        var x0 = Math.max(0, Math.floor((m.x - R) / F)), x1 = Math.min(cc - 1, Math.ceil((m.x + R) / F));
        var y0 = Math.max(0, Math.floor((m.y - R) / F)), y1 = Math.min(cr - 1, Math.ceil((m.y + R) / F));
        var touched = false;
        for (y = y0; y <= y1; y++) {
          for (x = x0; x <= x1; x++) {
            c = y * cc + x;
            if (!cm[c]) continue;
            var dx = (x + 0.5) * F - m.x, dy = (y + 0.5) * F - m.y, dist = Math.sqrt(dx * dx + dy * dy);
            if (dist >= R || dist < 0.01) continue;
            /* drag along the cursor's path plus a curl on each side of it: the wake of a stirring stick */
            var w = (1 - dist / R) * (1 - dist / R) * dt * STIR_GAIN;
            var side = m.vx * dy - m.vy * dx > 0 ? 1 : -1;
            U[c] += w * (m.vx * SWIRL_DRAG - dy / dist * speed * SWIRL_CURL * side);
            V[c] += w * (m.vy * SWIRL_DRAG + dx / dist * speed * SWIRL_CURL * side);
            touched = true;
          }
        }
        if (touched) {
          /* pad the simulated box: the stirred motion travels on after the cursor has gone */
          var pad = Math.ceil(R / F);
          x0 = Math.max(0, x0 - pad); y0 = Math.max(0, y0 - pad);
          x1 = Math.min(cc - 1, x1 + pad); y1 = Math.min(cr - 1, y1 + pad);
          if (!s.live) { s.bx0 = x0; s.by0 = y0; s.bx1 = x1; s.by1 = y1; s.live = true; }
          else {
            s.bx0 = Math.min(s.bx0, x0); s.by0 = Math.min(s.by0, y0);
            s.bx1 = Math.max(s.bx1, x1); s.by1 = Math.max(s.by1, y1);
          }
        }
      }
      if (!s.live) return;

      var decay = Math.exp(-dt * STIR_DECAY), relax = 1 - Math.exp(-dt * STIR_RELAX), maxV = 0, maxD = 0;
      var k = dt / F;   /* pixels/s -> blocks per step */
      for (y = s.by0; y <= s.by1; y++) {
        for (x = s.bx0; x <= s.bx1; x++) {
          c = y * cc + x;
          if (!cm[c]) continue;
          var u = U[c], v = V[c];
          var sp = Math.abs(u) + Math.abs(v);
          if (sp > maxV) maxV = sp;
          /* the liquid now here was at (x - u dt, y - v dt): inherit that spot's source and motion, interpolated */
          var fx = clamp(x - u * k, 0, cc - 1.001), fy = clamp(y - v * k, 0, cr - 1.001);
          var ix = fx | 0, iy = fy | 0, ax = fx - ix, ay = fy - iy, bx = 1 - ax, by = 1 - ay;
          var c00 = iy * cc + ix, c10 = c00 + 1, c01 = c00 + cc, c11 = c01 + 1;
          m2X[c] = (mX[c00] * bx + mX[c10] * ax) * by + (mX[c01] * bx + mX[c11] * ax) * ay;
          m2Y[c] = (mY[c00] * bx + mY[c10] * ax) * by + (mY[c01] * bx + mY[c11] * ax) * ay;
          U2[c] = (U[c00] * bx + U[c10] * ax) * by + (U[c01] * bx + U[c11] * ax) * ay;
          V2[c] = (V[c00] * bx + V[c10] * ax) * by + (V[c01] * bx + V[c11] * ax) * ay;
        }
      }
      var mask = s.mask, tone = s.tone, rimF = s.rimF, tone0 = s.tone0, rim0 = s.rim0;
      for (y = s.by0; y <= s.by1; y++) {
        for (x = s.bx0; x <= s.bx1; x++) {
          c = y * cc + x;
          if (!cm[c]) continue;
          U[c] = U2[c] * decay; V[c] = V2[c] * decay;
          var hx = x * F, hy = y * F;                  /* this block's home, in pixels */
          /* drift back home: a fraction of the distance, but never slower than STIR_RETURN, so it finishes */
          var ofx = m2X[c] - hx, ofy = m2Y[c] - hy, off = Math.sqrt(ofx * ofx + ofy * ofy);
          var back = Math.max(off * relax, STIR_RETURN * dt), keep = off > back ? 1 - back / off : 0;
          var sx = hx + ofx * keep, sy = hy + ofy * keep;
          mX[c] = sx; mY[c] = sy;
          var dev = off * keep;
          if (dev > maxD) maxD = dev;
          /* each pixel of the block shows the original shade at its source; a source outside the banana keeps
             the pixel's own shade */
          for (var oy = 0; oy < F; oy++) {
            var py = hy + oy;
            if (py >= rows) break;
            var ry = Math.round(sy + oy);
            for (var ox = 0; ox < F; ox++) {
              var px = hx + ox;
              if (px >= cols) break;
              var p = py * cols + px;
              if (!mask[p]) continue;
              var rx = Math.round(sx + ox), src = p;
              if (rx >= 0 && ry >= 0 && rx < cols && ry < rows && mask[ry * cols + rx]) src = ry * cols + rx;
              tone[p] = tone0[src]; rimF[p] = rim0[src];
            }
          }
        }
      }
      if (maxV < 0.05 && maxD < 0.3) {
        for (y = s.by0; y <= s.by1; y++) {
          for (x = s.bx0; x <= s.bx1; x++) {
            c = y * cc + x;
            mX[c] = m2X[c] = x * F; mY[c] = m2Y[c] = y * F; U[c] = V[c] = 0;
          }
        }
        var px0 = s.bx0 * F, px1 = Math.min(cols, (s.bx1 + 1) * F), py0 = s.by0 * F, py1 = Math.min(rows, (s.by1 + 1) * F);
        for (y = py0; y < py1; y++) for (x = px0; x < px1; x++) { c = y * cols + x; tone[c] = tone0[c]; rimF[c] = rim0[c]; }
        s.live = false;
      }
    }

    /* ---------------------------------------------------------------- flame */
    function buildEmitter(mask, eX, eY, eNx, eNy, haloW) {
      var n = clamp(eX.length * 3, 600, MAX_PARTICLES);
      rad = {
        mask: mask, haloW: haloW, n: n,
        ex: Float32Array.from(eX), ey: Float32Array.from(eY), enx: Float32Array.from(eNx), eny: Float32Array.from(eNy),
        x: new Float32Array(n), y: new Float32Array(n),             /* current position, derived from age */
        ox: new Float32Array(n), oy: new Float32Array(n),           /* launch point */
        vx: new Float32Array(n), v0: new Float32Array(n), acc: new Float32Array(n),
        amp: new Float32Array(n), freq: new Float32Array(n), phase: new Float32Array(n),
        age: new Float32Array(n), life: new Float32Array(n), bright: new Float32Array(n),
        offX: new Float32Array(n), offY: new Float32Array(n),       /* displacement stirred in by the mouse */
        sx: new Float32Array(n), sy: new Float32Array(n)            /* ...and its velocity */
      };
      /* start mid-flight so the page opens on a full flame rather than an empty sky */
      for (var k = 0; k < n; k++) emit(k, Math.random());
    }

    /* (Re)launch particle k from a random edge cell, `progress` (0..1) of the way through its life. */
    function emit(k, progress) {
      var r = rad, m = r.ex.length;
      if (!m) { r.life[k] = 1; r.age[k] = 1; return; }
      var e = (Math.random() * m) | 0;
      var life = 5 + Math.random() * 6;
      var rise = r.haloW * (1.2 + Math.random() * 2.8);   /* how high this one climbs before it fades out */
      r.ox[k] = r.ex[e]; r.oy[k] = r.ey[e];
      r.v0[k] = 0.6 * rise / life;                         /* starts slow and speeds up as it rises, like heat */
      r.acc[k] = 0.8 * rise / (life * life);
      r.vx[k] = r.enx[e] * 0.15 * rise / life;             /* a slight push off the edge it left from */
      r.amp[k] = r.haloW * (0.05 + Math.random() * 0.15);  /* side-to-side sway */
      r.freq[k] = 0.6 + Math.random();
      r.phase[k] = Math.random() * Math.PI * 2;
      r.life[k] = life;
      r.age[k] = life * progress;
      r.bright[k] = 170 + Math.random() * 85;
      r.offX[k] = r.offY[k] = r.sx[k] = r.sy[k] = 0;
      place(k);
    }

    function place(k) {
      var r = rad, t = r.age[k];
      r.x[k] = r.ox[k] + r.vx[k] * t + r.amp[k] * (Math.sin(r.freq[k] * t + r.phase[k]) - Math.sin(r.phase[k])) + r.offX[k];
      r.y[k] = r.oy[k] - (r.v0[k] * t + 0.5 * r.acc[k] * t * t) + r.offY[k];
    }

    function stepParticles(dt) {
      var r = rad, m = mouse;
      var speed = Math.hypot(m.vx, m.vy);
      var stir = m.active && speed > 1;
      var R = Math.max(6, rows * SWIRL_RADIUS), damp = Math.exp(-dt * SWIRL_DAMPING);
      for (var k = 0; k < r.n; k++) {
        r.age[k] += dt;
        if (stir) {
          /* specks near the cursor are dragged along its path and curled into a whirl on each side of it */
          var dx = r.x[k] - m.x, dy = r.y[k] - m.y, dist = Math.sqrt(dx * dx + dy * dy);
          if (dist < R && dist > 0.01) {
            var w = (1 - dist / R) * (1 - dist / R) * dt;
            var side = m.vx * dy - m.vy * dx > 0 ? 1 : -1;
            r.sx[k] += w * (m.vx * SWIRL_DRAG - dy / dist * speed * SWIRL_CURL * side);
            r.sy[k] += w * (m.vy * SWIRL_DRAG + dx / dist * speed * SWIRL_CURL * side);
          }
        }
        if (r.sx[k] || r.sy[k]) {
          r.offX[k] += r.sx[k] * dt;
          r.offY[k] += r.sy[k] * dt;
          r.sx[k] *= damp; r.sy[k] *= damp;
          if (Math.abs(r.sx[k]) + Math.abs(r.sy[k]) < 0.01) r.sx[k] = r.sy[k] = 0;
        }
        place(k);
        if (r.age[k] >= r.life[k] || r.x[k] < 0 || r.y[k] < 0 || r.x[k] >= cols || r.y[k] >= rows) emit(k, 0);
      }
    }

    /* ---------------------------------------------------------------- mouse */
    var mouse = { x: 0, y: 0, vx: 0, vy: 0, t: 0, active: false };

    function onPointerMove(e) {
      var b = canvas.getBoundingClientRect();
      if (!b.width || !b.height) return;
      var gx = (e.clientX - b.left) / b.width * cols, gy = (e.clientY - b.top) / b.height * rows;
      var dtm = (e.timeStamp - mouse.t) / 1000;
      if (mouse.active && dtm > 0 && dtm < 0.2) {
        mouse.vx = mouse.vx * 0.5 + (gx - mouse.x) / dtm * 0.5;
        mouse.vy = mouse.vy * 0.5 + (gy - mouse.y) / dtm * 0.5;
      }
      mouse.x = gx; mouse.y = gy; mouse.t = e.timeStamp; mouse.active = true;
    }

    function onPointerLeave() { mouse.active = false; mouse.vx = mouse.vy = 0; }

    function reroll(noise, fraction) {
      if (fraction >= 1) { for (var i = 0; i < noise.length; i++) noise[i] = Math.random(); return; }
      var n = Math.round(noise.length * fraction);
      for (var k = 0; k < n; k++) noise[(Math.random() * noise.length) | 0] = Math.random();
    }

    /* ---------------------------------------------------------------- frame */
    function put(d, j, c, a) { d[j] = c[0]; d[j + 1] = c[1]; d[j + 2] = c[2]; d[j + 3] = a; }

    /* Palette colour for banana pixel i from its (possibly stirred) tone and rim, dithered by its noise roll. */
    function surfaceColour(i) {
      var c = body.idx[i], r = body.noise[i];
      if (r < body.rimF[c] * 0.85) return RIM;
      var v = body.tone[c] + (r - 0.5) * 0.6;
      return v > 0.78 ? HI : v > 0.5 ? MID : v > 0.24 ? LO : DEEP;
    }

    function draw() {
      var d = out.data;
      d.fill(0);
      var i, j, r, v;

      for (i = 0; i < body.idx.length; i++) put(d, body.idx[i] * 4, surfaceColour(i), 255);

      /* flame: specks fade as they rise */
      var p = rad;
      for (i = 0; i < p.n; i++) {
        var c = (p.y[i] | 0) * cols + (p.x[i] | 0);
        if (p.mask[c]) continue;
        v = (p.bright[i] * Math.pow(1 - p.age[i] / p.life[i], 0.8)) | 0;
        if (v < 40) continue;
        j = c * 4;
        if (d[j + 3] && d[j + 2] >= v) continue;
        d[j] = (v * FLAME[0]) | 0; d[j + 1] = (v * FLAME[1]) | 0; d[j + 2] = (v * FLAME[2]) | 0; d[j + 3] = 255;
      }

      gctx.putImageData(out, 0, 0);
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(grid, 0, 0, canvas.width, canvas.height);
    }

    function frame(now) {
      if (stopped) return;
      var dt = last ? Math.min(0.05, (now - last) / 1000) : 0;
      last = now;
      twinkleAt += dt * 1000;
      if (twinkleAt >= TICK_MS) {
        twinkleAt = 0;
        reroll(body.noise, TWINKLE);
      }
      /* the cursor's speed fades out once it stops moving, so a resting mouse stirs nothing */
      var fade = Math.exp(-dt * 8);
      mouse.vx *= fade; mouse.vy *= fade;
      stepParticles(dt);
      stepSurface(dt);
      draw();
      raf = requestAnimationFrame(frame);
    }

    /* ---------------------------------------------------------------- lifecycle */
    function start() {
      if (running || stopped || reduced) return;
      running = true;
      last = 0;
      raf = requestAnimationFrame(frame);
    }

    function halt() {
      running = false;
      if (raf) cancelAnimationFrame(raf);
      raf = 0;
    }

    function onVisibility() {
      if (document.hidden) halt();
      else start();
    }

    var resizeTimer = 0;
    function onResize() {
      clearTimeout(resizeTimer);
      resizeTimer = setTimeout(function () {
        if (stopped || !img) return;
        buildLayout();
        draw();
      }, 120);
    }

    var ro = null;

    function onMotionPref() {
      reduced = !!(mq && mq.matches);
      if (reduced) halt();
      else start();
    }

    function stop() {
      stopped = true;
      halt();
      clearTimeout(resizeTimer);
      document.removeEventListener('visibilitychange', onVisibility);
      window.removeEventListener('resize', onResize);
      window.removeEventListener('pointermove', onPointerMove);
      document.documentElement.removeEventListener('pointerleave', onPointerLeave);
      if (ro) ro.disconnect();
      if (mq && mq.removeEventListener) mq.removeEventListener('change', onMotionPref);
      else if (mq && mq.removeListener) mq.removeListener(onMotionPref);
      if (canvas.parentNode) canvas.parentNode.removeChild(canvas);
      img = null; body = null; rad = null; out = null;
    }

    function boot() {
      if (stopped) return;
      buildLayout();
      draw();
      document.addEventListener('visibilitychange', onVisibility);
      window.addEventListener('resize', onResize);
      /* the canvas ignores the pointer (the form sits on top), so watch the whole window instead */
      window.addEventListener('pointermove', onPointerMove, { passive: true });
      document.documentElement.addEventListener('pointerleave', onPointerLeave);
      if (window.ResizeObserver && el) {
        ro = new ResizeObserver(onResize);
        ro.observe(el);
      }
      if (mq) {
        if (mq.addEventListener) mq.addEventListener('change', onMotionPref);
        else if (mq.addListener) mq.addListener(onMotionPref);
      }
      start();
    }

    img = new Image();
    img.onload = boot;
    img.onerror = function () { /* leave the canvas empty rather than break layout */ };
    img.src = ASSET;

    return { stop: stop };
  }

  window.Banana = { mount: mount };
})();
