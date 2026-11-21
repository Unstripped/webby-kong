/* Shining golden banana rendered as chunky pixels with a flowing gold texture
   and edge-emitted sparks. Exposes window.Banana = { mount(el) -> { stop() } }.
   No dependencies. */
(function () {
  'use strict';

  var TAU = Math.PI * 2;
  var SCRIPT_SRC = (document.currentScript && document.currentScript.src) || '';
  var ASSET = SCRIPT_SRC.replace(/[^\/]*$/, '') + 'banana.png';
  var GRID_TARGET = 118; /* banana cells across, at most */
  var MAX_SPARKS = 150;

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
    var tex = null;         /* {w,h,data} texture at grid resolution */
    var img = null;
    var cols = 0, rows = 0, cell = 3;
    var out = null;         /* ImageData for the low-res frame */
    var px = null;          /* masked pixel table */
    var meanWl = 200;       /* average luminance of the masked body */
    var edgeCells = null;   /* candidate spark origins */
    var sparks = [];
    var raf = 0, t0 = 0, stopped = false;
    var running = false;

    /* ---------------------------------------------------------------- layout */
    function buildLayout() {
      var dpr = clamp(window.devicePixelRatio || 1, 1, 2.5);
      var cw = el.clientWidth || el.offsetWidth || 320;
      var ch = el.clientHeight || el.offsetHeight || cw;
      var devW = Math.round(cw * dpr), devH = Math.round(ch * dpr);

      cell = Math.max(2, Math.round(Math.min(devW, devH) / GRID_TARGET));
      cols = Math.max(8, Math.round(devW / cell));
      rows = Math.max(8, Math.round(devH / cell));

      canvas.width = cols * cell;
      canvas.height = rows * cell;
      grid.width = cols;
      grid.height = rows;

      var asp = (img.naturalWidth || 1) / (img.naturalHeight || 1);
      var sizeX = Math.min(cols, rows * asp);
      var sizeY = sizeX / asp;
      var tw = Math.max(16, Math.min(Math.round(sizeX), 200));
      var th = Math.max(16, Math.round(tw / asp));

      tex = makeTexture(tw, th);
      buildPixelTable(sizeX, sizeY, (cols - sizeX) * 0.5, (rows - sizeY) * 0.5);
      out = gctx.createImageData(cols, rows);
      sparks.length = 0;
      t0 = 0;
    }

    /* Downsample the banana to a small texture, keep colour + alpha. */
    function makeTexture(tw, th) {
      var c = document.createElement('canvas');
      c.width = tw; c.height = th;
      var g = c.getContext('2d');
      g.imageSmoothingEnabled = true;
      g.clearRect(0, 0, tw, th);
      g.drawImage(img, 0, 0, tw, th);
      var d = g.getImageData(0, 0, tw, th);
      return { w: tw, h: th, data: d.data };
    }

    /* Collect masked pixels, rim strength and spark origins. */
    function buildPixelTable(sizeX, sizeY, ox, oy) {
      var tw = tex.w, th = tex.h;
      var mask = new Uint8Array(cols * rows);
      var uArr = [], vArr = [], gxArr = [], gyArr = [];
      var i, gx, gy, u, v, tx, ty, a;

      for (gy = 0; gy < rows; gy++) {
        for (gx = 0; gx < cols; gx++) {
          u = (gx + 0.5 - ox) / sizeX;
          v = (gy + 0.5 - oy) / sizeY;
          if (u < 0 || u > 1 || v < 0 || v > 1) continue;
          tx = clamp(Math.floor(u * tw), 0, tw - 1);
          ty = clamp(Math.floor(v * th), 0, th - 1);
          a = tex.data[(ty * tw + tx) * 4 + 3];
          if (a > 120) mask[gy * cols + gx] = 1;
        }
      }

      /* drop stray single cells so the silhouette edge reads clean */
      for (gy = 1; gy < rows - 1; gy++) {
        for (gx = 1; gx < cols - 1; gx++) {
          i = gy * cols + gx;
          if (mask[i] && !(mask[i - 1] + mask[i + 1] + mask[i - cols] + mask[i + cols])) mask[i] = 0;
        }
      }
      for (gy = 0; gy < rows; gy++) {
        for (gx = 0; gx < cols; gx++) {
          if (!mask[gy * cols + gx]) continue;
          uArr.push((gx + 0.5 - ox) / sizeX);
          vArr.push((gy + 0.5 - oy) / sizeY);
          gxArr.push(gx); gyArr.push(gy);
        }
      }

      /* cheap 2-pass chamfer distance so rim pixels can shimmer */
      var dist = new Float32Array(cols * rows);
      var BIG = 1e6;
      for (i = 0; i < dist.length; i++) dist[i] = mask[i] ? BIG : 0;
      for (gy = 0; gy < rows; gy++) {
        for (gx = 0; gx < cols; gx++) {
          i = gy * cols + gx;
          if (dist[i] === 0) continue;
          if (gx > 0) dist[i] = Math.min(dist[i], dist[i - 1] + 1);
          if (gy > 0) dist[i] = Math.min(dist[i], dist[i - cols] + 1);
        }
      }
      for (gy = rows - 1; gy >= 0; gy--) {
        for (gx = cols - 1; gx >= 0; gx--) {
          i = gy * cols + gx;
          if (dist[i] === 0) continue;
          if (gx < cols - 1) dist[i] = Math.min(dist[i], dist[i + 1] + 1);
          if (gy < rows - 1) dist[i] = Math.min(dist[i], dist[i + cols] + 1);
        }
      }
      var rim = new Float32Array(uArr.length);
      for (i = 0; i < rim.length; i++) {
        rim[i] = clamp(1 - dist[gyArr[i] * cols + gxArr[i]] / 3.2, 0, 1);
      }

      px = {
        mask: mask,
        n: uArr.length,
        u: Float32Array.from(uArr),
        v: Float32Array.from(vArr),
        gx: Int32Array.from(gxArr),
        gy: Int32Array.from(gyArr),
        rim: rim
      };

      var acc = 0, ref = [0, 0, 0, 0];
      for (i = 0; i < px.n; i++) {
        sampleRGB(px.u[i], px.v[i], ref);
        acc += ref[0] + ref[1] + ref[2];
      }
      meanWl = px.n ? Math.max(40, acc / px.n) : 200;

      /* spark origins: masked pixels touching the silhouette edge, with normals */
      var list = [];
      for (i = 0; i < px.n; i++) {
        gx = px.gx[i]; gy = px.gy[i];
        var nx = 0, ny = 0;
        if (!mask[gy * cols + gx - 1] || !mask[gy * cols + gx + 1] ||
            !mask[(gy - 1) * cols + gx] || !mask[(gy + 1) * cols + gx]) {
          for (var dy = -1; dy <= 1; dy++) {
            for (var dx = -1; dx <= 1; dx++) {
              var xx = gx + dx, yy = gy + dy;
              if (xx < 0 || yy < 0 || xx >= cols || yy >= rows) { nx -= dx * 0.5; ny -= dy * 0.5; continue; }
              if (!mask[yy * cols + xx]) { nx += dx; ny += dy; }
            }
          }
          var len = Math.hypot(nx, ny);
          if (len < 0.001) { nx = 0; ny = -1; len = 1; }
          list.push(gx, gy, nx / len, ny / len);
        }
      }
      edgeCells = Int32Array.from(list);
    }

    /* ------------------------------------------------------------ sampling */
    function sampleRGB(u, v, res) {
      var tw = tex.w, th = tex.h, d = tex.data;
      var x = clamp(u * tw - 0.5, 0, tw - 1.001), y = clamp(v * th - 0.5, 0, th - 1.001);
      var x0 = x | 0, y0 = y | 0, fx = x - x0, fy = y - y0;
      var x1 = x0 + 1, y1 = y0 + 1;
      if (x1 > tw - 1) x1 = tw - 1;
      if (y1 > th - 1) y1 = th - 1;
      var i00 = (y0 * tw + x0) * 4, i10 = (y0 * tw + x1) * 4;
      var i01 = (y1 * tw + x0) * 4, i11 = (y1 * tw + x1) * 4;
      var w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
      res[0] = d[i00] * w00 + d[i10] * w10 + d[i01] * w01 + d[i11] * w11;
      res[1] = d[i00 + 1] * w00 + d[i10 + 1] * w10 + d[i01 + 1] * w01 + d[i11 + 1] * w11;
      res[2] = d[i00 + 2] * w00 + d[i10 + 2] * w10 + d[i01 + 2] * w01 + d[i11 + 2] * w11;
      res[3] = d[i00 + 3] * w00 + d[i10 + 3] * w10 + d[i01 + 3] * w01 + d[i11 + 3] * w11;
    }

    /* ------------------------------------------------------------- sprites */
    function spawn(now) {
      if (!edgeCells || !edgeCells.length) return;
      var k = (Math.random() * (edgeCells.length / 4)) | 0;
      k *= 4;
      var gx = edgeCells[k], gy = edgeCells[k + 1];
      var nx = edgeCells[k + 2], ny = edgeCells[k + 3];
      var jitter = 0.55;
      var speed = 7 + Math.random() * 13;
      var pale = Math.random() < 0.38;
      sparks.push({
        x: gx + 0.5 + nx * 0.6, y: gy + 0.5 + ny * 0.6,
        vx: (nx + (Math.random() - 0.5) * jitter) * speed,
        vy: (ny + (Math.random() - 0.5) * jitter) * speed - 2,
        life: 0, max: 0.7 + Math.random() * 1.2,
        big: Math.random() < 0.32,
        c: pale ? [255, 232, 170] : [255, 252, 236]
      });
    }


    /* --------------------------------------------------------------- frame */
    var tmp = [0, 0, 0, 0], tmp2 = [0, 0, 0, 0];

    function frame(now) {
      if (stopped) return;
      if (!t0) t0 = now;
      var t = (now - t0) / 1000;

      var d = out.data;
      d.fill(0);

      /* rotating swirl about the banana centre */
      var ang = t * 0.42;
      var ca = Math.cos(ang), sa = Math.sin(ang);
      var pulse = 1 + 0.11 * Math.sin(t * 1.35);
      var wob = 1 + 0.09 * Math.sin(t * 0.62);

      var n = px.n, data = d;
      for (var i = 0; i < n; i++) {
        var u = px.u[i], v = px.v[i];
        /* domain warp: sine flow + slow swirl, no silhouette change */
        var rx = u - 0.5, ry = v - 0.5;
        var rr = rx * rx + ry * ry;
        var k2 = 0.85 * (1 - Math.min(1, rr * 3.1));
        var wx = rx * ca - ry * sa;
        var wy = rx * sa + ry * ca;
        var su = 0.5 + wx * wob + 0.085 * k2 * Math.sin(TAU * (v * 1.9 + t * 0.21));
        var sv = 0.5 + wy * wob + 0.075 * k2 * Math.sin(TAU * (u * 1.7 - t * 0.17));
        su += 0.034 * Math.sin(TAU * (v * 2.7 - t * 0.31) + 1.3);
        sv += 0.034 * Math.sin(TAU * (u * 3.1 + t * 0.26) + 2.7);

        sampleRGB(u, v, tmp);        /* base form, keeps the silhouette shading */
        sampleRGB(su, sv, tmp2);     /* flowing gold texture */
        /* drive brightness from how bright the flowing sample is, so the form
           never breaks into holes when the warp lands on the shaded belly */
        var wl = tmp2[0] + tmp2[1] + tmp2[2];
        var m = clamp(0.86 + 0.42 * (wl / meanWl - 1), 0.76, 1.42);
        var r = tmp[0] * m, g = tmp[1] * m, b = tmp[2] * m;

        /* travelling specular band + sparkle where highlights pass */
        var band = Math.sin(TAU * (su * 1.35 + sv * 2.2 - t * 0.34));
        var boost = 1;
        if (band > 0.15) {
          var s2 = (band - 0.15) * 1.2;
          boost += s2 * s2 * 0.85;
        }
        var tw1 = Math.sin(u * 71.3 + t * 3.1) * Math.sin(v * 58.7 - t * 2.4);
        if (tw1 > 0.80) boost += (tw1 - 0.80) * 2.0;

        /* rim shimmer */
        var rm = px.rim[i];
        if (rm > 0) boost += rm * rm * (0.28 + 0.30 * Math.sin(t * 1.7 + u * 9.0));

        var mul = pulse * boost;
        r = r * mul; g = g * mul; b = b * mul;
        /* keep every hot pixel inside the gold hue cone, with headroom for bloom */
        if (r > 238) r = 238;
        if (g > r * 0.90) g = r * 0.90;
        if (b > r * 0.70) b = r * 0.70;
        var j = (px.gy[i] * cols + px.gx[i]) * 4;
        data[j] = r;
        data[j + 1] = g;
        data[j + 2] = b;
        data[j + 3] = 255;
      }

      /* sparks */
      for (var s = 0; s < 2; s++) if (sparks.length < MAX_SPARKS) spawn(now);
      var dt = Math.min(0.05, last ? (now - last) / 1000 : 0.016);
      last = now;
      for (var p = sparks.length - 1; p >= 0; p--) {
        var sp = sparks[p];
        sp.life += dt;
        if (sp.life >= sp.max) { sparks.splice(p, 1); continue; }
        var pxv = sp.x, pyv = sp.y;
        sp.x += sp.vx * dt; sp.y += sp.vy * dt;
        sp.vy += 5 * dt; sp.vx *= 0.986; sp.vy *= 0.986;
        var f = 1 - sp.life / sp.max;
        var a = f * f * f;
        if (a < 0.05) continue;
        var rr2 = sp.c[0] * a, gg = sp.c[1] * a, bb = sp.c[2] * a;
        var ix = sp.x | 0, iy = sp.y | 0;
        var idx = -1;
        if (ix >= 0 && iy >= 0 && ix < cols && iy < rows && !px.mask[iy * cols + ix]) {
          idx = (iy * cols + ix) * 4;
          var h0 = d[idx] + rr2, h1 = d[idx + 1] + gg, h2 = d[idx + 2] + bb;
          d[idx] = h0 > 255 ? 255 : h0;
          d[idx + 1] = h1 > 255 ? 255 : h1;
          d[idx + 2] = h2 > 255 ? 255 : h2;
          d[idx + 3] = 255;
        }
        if (sp.big && idx >= 0) {
          var tx2 = (ix + 1 < cols) ? idx + 4 : -1;
          if (tx2 >= 0) {
            var k0 = d[tx2] + rr2 * 0.5, k1 = d[tx2 + 1] + gg * 0.5, k2b = d[tx2 + 2] + bb * 0.5;
            d[tx2] = k0 > 255 ? 255 : k0;
            d[tx2 + 1] = k1 > 255 ? 255 : k1;
            d[tx2 + 2] = k2b > 255 ? 255 : k2b;
            d[tx2 + 3] = 255;
          }
        }
        /* short trail one cell back along the flight path */
        var qx = (pxv - sp.vx * 0.035) | 0, qy = (pyv - sp.vy * 0.035) | 0;
        if (qx >= 0 && qy >= 0 && qx < cols && qy < rows && !px.mask[qy * cols + qx]) {
          var qi = (qy * cols + qx) * 4;
          var m0 = d[qi] + rr2 * 0.42, m1 = d[qi + 1] + gg * 0.42, m2 = d[qi + 2] + bb * 0.42;
          d[qi] = m0 > 255 ? 255 : m0;
          d[qi + 1] = m1 > 255 ? 255 : m1;
          d[qi + 2] = m2 > 255 ? 255 : m2;
          d[qi + 3] = 255;
        }
      }

      /* blit: crisp pixels, then a soft additive bloom that breathes */
      gctx.putImageData(out, 0, 0);
      var w2 = canvas.width, h2 = canvas.height;
      ctx.clearRect(0, 0, w2, h2);
      ctx.imageSmoothingEnabled = false;
      ctx.globalCompositeOperation = 'source-over';
      ctx.globalAlpha = 1;
      ctx.drawImage(grid, 0, 0, w2, h2);
      var glowA = 0.30 + 0.14 * Math.sin(t * 1.35);
      if (glowA > 0) {
        ctx.imageSmoothingEnabled = true;
        ctx.globalCompositeOperation = 'screen';
        ctx.globalAlpha = glowA;
        var gs = 1.05;
        ctx.drawImage(grid, (w2 - w2 * gs) / 2, (h2 - h2 * gs) / 2, w2 * gs, h2 * gs);
        ctx.globalAlpha = 1;
        ctx.globalCompositeOperation = 'source-over';
      }

      raf = requestAnimationFrame(frame);
    }

    var last = 0;

    /* ------------------------------------------------------------ lifecycle */
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

    function stop() {
      stopped = true;
      halt();
      document.removeEventListener('visibilitychange', onVisibility);
      window.removeEventListener('resize', onResize);
      if (ro) ro.disconnect();
      if (mq && mq.removeEventListener) mq.removeEventListener('change', onMotionPref);
      if (canvas.parentNode) canvas.parentNode.removeChild(canvas);
      img = null; tex = null; px = null; out = null; sparks.length = 0;
    }

    var resizeTimer = 0;
    function onResize() {
      clearTimeout(resizeTimer);
      resizeTimer = setTimeout(function () {
        resizeTimer = 0;
        if (stopped || !img) return;
        buildLayout();
        if (reduced) drawStill();
      }, 120);
    }

    var ro = null;

    function onMotionPref() {
      reduced = !!(mq && mq.matches);
      if (reduced) { halt(); sparks.length = 0; drawStill(); }
      else start();
    }

    /* static, still-pixelated frame for reduced motion */
    function drawStill() {
      var d = out.data;
      d.fill(0);
      for (var i = 0; i < px.n; i++) {
        sampleRGB(px.u[i], px.v[i], tmp);
        var j = (px.gy[i] * cols + px.gx[i]) * 4;
        d[j] = tmp[0]; d[j + 1] = tmp[1]; d[j + 2] = tmp[2]; d[j + 3] = 255;
      }
      gctx.putImageData(out, 0, 0);
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(grid, 0, 0, canvas.width, canvas.height);
    }

    function boot() {
      if (stopped) return;
      buildLayout();
      document.addEventListener('visibilitychange', onVisibility);
      window.addEventListener('resize', onResize);
      if (window.ResizeObserver && el) {
        ro = new ResizeObserver(onResize);
        ro.observe(el);
      }
      if (mq) {
        if (mq.addEventListener) mq.addEventListener('change', onMotionPref);
        else if (mq.addListener) mq.addListener(onMotionPref);
      }
      if (reduced) drawStill();
      else start();
    }

    img = new Image();
    img.onload = boot;
    img.onerror = function () { /* leave the canvas empty rather than break layout */ };
    img.src = ASSET;

    return { stop: stop };
  }

  window.Banana = { mount: mount };
})();
