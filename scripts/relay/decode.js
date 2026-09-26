/**
 * PolyCast5 Screen Mirror frame decoder.
 *
 * Canonical implementation, shared by the dev viewer and the Shopify storefront page and
 * covered by test_decode.js against output from the real firmware encoder. Pure parsing:
 * it hands finished tiles to a sink, so it can be tested without a canvas.
 *
 * Wire format lives in components/mirror/include/mirror_proto.h. Everything is
 * little-endian, including the RGB565 pixels: the device never byte-swaps, because the
 * big-endian swap the panel needs happens further down in its SPI driver.
 *
 * The thermal render below is a port of components/lcd/src/lcd_ir_exp_render.c and must
 * stay pixel-identical to it: test_thermal.js holds the two to that.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.MirrorDecode = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var MSG_FRAME = 0x01;
  var ENC_RAW = 0, ENC_SOLID = 1, ENC_RLE_H = 2, ENC_PAL4 = 3, ENC_RLE_V = 4;
  var FLAG_KEYFRAME = 0x01, FLAG_HALF = 0x02, FLAG_LAST = 0x04, FLAG_REDACTED = 0x08;

  var TILE_W = 16, TILE_H = 15;
  var SCR_W = 240, SCR_H = 135;
  var GRID_COLS = SCR_W / TILE_W, GRID_ROWS = SCR_H / TILE_H;

  var MSG_THERMAL = 0x06;
  var TH_ON = 0x01, TH_PALETTE = 0x02, TH_FLIP_H = 0x04, TH_FLIP_V = 0x08, TH_CROSSHAIR = 0x10;
  var TH_TILE_BYTES = (GRID_COLS * GRID_ROWS + 7) >> 3;
  var TH_HDR = 22 + TH_TILE_BYTES;

  /**
   * Decode one FRAME message.
   *
   * @param {ArrayBuffer|DataView} buf
   * @param {{putTile: function(Uint16Array, number, number, number, number, boolean)}} sink
   * @returns {null|{seq:number,last:boolean,keyframe:boolean,redacted:boolean,level:number,tiles:number}}
   *          null if the message is not a well-formed frame
   */
  function decodeFrame(buf, sink) {
    var dv = buf instanceof DataView ? buf : new DataView(buf);

    if (dv.byteLength < 10 || dv.getUint8(0) !== MSG_FRAME) return null;

    var flags = dv.getUint8(1);
    var seq = dv.getUint16(2, true);
    var tw = dv.getUint8(4), th = dv.getUint8(5);
    var cols = dv.getUint8(6), rows = dv.getUint8(7);
    var count = dv.getUint16(8, true);
    var half = (flags & FLAG_HALF) !== 0;

    if (tw < 1 || th < 1 || tw > TILE_W || th > TILE_H || cols < 1 || rows < 1) return null;

    var total = tw * th;
    var px = new Uint16Array(total);
    var off = 10;

    for (var k = 0; k < count; k++) {
      if (off + 5 > dv.byteLength) return null;

      var tile = dv.getUint16(off, true);
      var enc = dv.getUint8(off + 2);
      var plen = dv.getUint16(off + 3, true);
      var p = off + 5;

      if (p + plen > dv.byteLength) return null;
      if (tile >= cols * rows) return null;

      if (enc === ENC_SOLID) {
        if (plen !== 2) return null;
        px.fill(dv.getUint16(p, true));
      } else if (enc === ENC_RAW) {
        if (plen !== total * 2) return null;
        for (var i = 0; i < total; i++) px[i] = dv.getUint16(p + i * 2, true);
      } else if (enc === ENC_RLE_H || enc === ENC_RLE_V) {
        var vert = enc === ENC_RLE_V;
        var n = 0, q = p;

        while (q + 3 <= p + plen) {
          var run = dv.getUint8(q);
          var c = dv.getUint16(q + 1, true);
          q += 3;

          while (run-- > 0) {
            if (n >= total) return null;
            px[vert ? (n % th) * tw + ((n / th) | 0) : n] = c;
            n++;
          }
        }
        if (n !== total) return null;
      } else if (enc === ENC_PAL4) {
        if (plen < 1) return null;
        var pn = dv.getUint8(p);
        if (pn < 1 || pn > 16) return null;
        if (plen !== 1 + pn * 2 + ((total + 1) >> 1)) return null;

        var base = p + 1 + pn * 2;
        for (var j = 0; j < total; j++) {
          var byte = dv.getUint8(base + (j >> 1));
          var idx = (j & 1) ? (byte & 0x0F) : (byte >> 4);
          if (idx >= pn) return null;
          px[j] = dv.getUint16(p + 1 + idx * 2, true);
        }
      } else {
        return null;
      }

      sink.putTile(px, tw, th, tile % cols, (tile / cols) | 0, half);
      off = p + plen;
    }

    return {
      seq: seq,
      last: (flags & FLAG_LAST) !== 0,
      keyframe: (flags & FLAG_KEYFRAME) !== 0,
      redacted: (flags & FLAG_REDACTED) !== 0,
      level: flags >> 4,
      tiles: count,
    };
  }

  /**
   * Decode one THERMAL message.
   *
   * @param {ArrayBuffer|DataView} buf
   * @returns {null|{seq:number,on:boolean}} null if malformed. With on, also the canvas
   *          rect (x, y, w, h), the source frame (cols, rows, px), lo, hi, the crosshair
   *          (crosshair, fg, bg), flipH, flipV, tiles (bitset) and palette (null when the
   *          last one still applies)
   */
  function decodeThermal(buf) {
    var dv = buf instanceof DataView ? buf : new DataView(buf);

    if (dv.byteLength < 4 || dv.getUint8(0) !== MSG_THERMAL) return null;

    var flags = dv.getUint8(1);
    var r = { seq: dv.getUint16(2, true), on: (flags & TH_ON) !== 0 };

    if (!r.on) return r;
    if (dv.byteLength < TH_HDR) return null;

    r.x = dv.getUint8(4); r.y = dv.getUint8(5); r.w = dv.getUint8(6); r.h = dv.getUint8(7);
    r.cols = dv.getUint8(8); r.rows = dv.getUint8(9);
    r.lo = dv.getInt32(10, true); r.hi = dv.getInt32(14, true);
    r.fg = dv.getUint16(18, true); r.bg = dv.getUint16(20, true);
    r.flipH = (flags & TH_FLIP_H) !== 0;
    r.flipV = (flags & TH_FLIP_V) !== 0;
    r.crosshair = (flags & TH_CROSSHAIR) !== 0;

    if (r.w < 1 || r.h < 1 || r.x + r.w > SCR_W || r.y + r.h > SCR_H) return null;
    if (r.cols < 1 || r.rows < 1) return null;

    r.tiles = new Uint8Array(TH_TILE_BYTES);
    for (var i = 0; i < TH_TILE_BYTES; i++) r.tiles[i] = dv.getUint8(22 + i);

    var off = TH_HDR;
    r.palette = null;

    if (flags & TH_PALETTE) {
      if (off + 512 > dv.byteLength) return null;
      r.palette = new Uint16Array(256);
      for (var p = 0; p < 256; p++) r.palette[p] = dv.getUint16(off + p * 2, true);
      off += 512;
    }

    var n = r.cols * r.rows;
    if (off + n * 2 !== dv.byteLength) return null;

    r.px = new Int16Array(n);
    for (var k = 0; k < n; k++) r.px[k] = dv.getInt16(off + k * 2, true);

    return r;
  }

  // a / b rounded to nearest, ties to even, for b > 0. Exact while |a| < 2^53: the
  // quotient is small, so its fraction is either 0 or far above the division's rounding
  function divRound(a, b) {
    var q = Math.floor(a / b), r = a - q * b;
    if (r < 0) { r += b; q -= 1; } else if (r >= b) { r -= b; q += 1; }
    if (2 * r > b || (2 * r === b && (q & 1))) q += 1;
    return q;
  }

  /**
   * One axis of Catmull-Rom taps, as irx_render_taps(): four source indices and four Q8
   * weights per output pixel, flattened.
   */
  function thermalTaps(dstN, srcN, flip) {
    var den = 2 * dstN, den3 = den * den * den;
    var idx = new Int16Array(dstN * 4), w = new Int16Array(dstN * 4);

    for (var d = 0; d < dstN; d++) {
      var num = (2 * d + 1) * srcN - dstN;
      var i0 = Math.floor(num / den), r = num - i0 * den;
      var r2 = r * r, r3 = r2 * r;
      var n = [
        -r3 + 2 * r2 * den - r * den * den,
        3 * r3 - 5 * r2 * den + 2 * den3,
        -3 * r3 + 4 * r2 * den + r * den * den,
        r3 - r2 * den,
      ];
      var sum = 0;

      for (var k = 0; k < 4; k++) {
        var si = i0 - 1 + k;
        if (si < 0) si = 0;
        if (si > srcN - 1) si = srcN - 1;
        if (flip) si = srcN - 1 - si;
        idx[d * 4 + k] = si;
        w[d * 4 + k] = divRound(128 * n[k], den3);
        sum += w[d * 4 + k];
      }

      w[d * 4 + 1] += 256 - sum;
    }

    return { idx: idx, w: w };
  }

  /**
   * Upscale a decoded THERMAL frame to w x h RGB565, as irx_render_frame() and
   * irx_render_crosshair() draw the device canvas.
   *
   * @param t       decodeThermal() result with on set
   * @param palette 256 RGB565 entries
   * @param tx      thermalTaps(t.w, t.cols, t.flipH)
   * @param ty      thermalTaps(t.h, t.rows, t.flipV)
   * @returns {Uint16Array}
   */
  function thermalRender(t, palette, tx, ty) {
    var sw = t.cols, sh = t.rows, dw = t.w, dh = t.h, src = t.px;
    var mid = new Int16Array(sh * dw);
    var x, k, s1, s2, v, lo, hi;

    // Pass 1: horizontal
    for (var r = 0; r < sh; r++) {
      var so = r * sw, mo = r * dw;

      for (x = 0; x < dw; x++) {
        k = x * 4;
        s1 = src[so + tx.idx[k + 1]];
        s2 = src[so + tx.idx[k + 2]];
        v = (src[so + tx.idx[k]] * tx.w[k] + s1 * tx.w[k + 1] +
             s2 * tx.w[k + 2] + src[so + tx.idx[k + 3]] * tx.w[k + 3]) >> 8;
        lo = s1 < s2 ? s1 : s2;
        hi = s1 < s2 ? s2 : s1;
        if (v < lo) v = lo; else if (v > hi) v = hi;
        mid[mo + x] = v;
      }
    }

    // Pass 2: vertical, then normalise and map through the palette. int32 like the C
    var span = (t.hi - t.lo) | 0;
    var recip = span > 0 ? ((255 << 16) / span) | 0 : 0;
    var out = new Uint16Array(dw * dh);

    for (var y = 0; y < dh; y++) {
      k = y * 4;
      var r0 = ty.idx[k] * dw, r1 = ty.idx[k + 1] * dw, r2 = ty.idx[k + 2] * dw, r3 = ty.idx[k + 3] * dw;
      var w0 = ty.w[k], w1 = ty.w[k + 1], w2 = ty.w[k + 2], w3 = ty.w[k + 3];
      var oo = y * dw;

      for (x = 0; x < dw; x++) {
        s1 = mid[r1 + x];
        s2 = mid[r2 + x];
        v = (mid[r0 + x] * w0 + s1 * w1 + s2 * w2 + mid[r3 + x] * w3) >> 8;
        lo = s1 < s2 ? s1 : s2;
        hi = s1 < s2 ? s2 : s1;
        if (v < lo) v = lo; else if (v > hi) v = hi;

        var d = (v - t.lo) | 0;
        if (d < 0) d = 0; else if (d > span) d = span;

        var n = Math.imul(d, recip) >> 16;
        if (n > 255) n = 255;

        out[oo + x] = palette[n];
      }
    }

    if (t.crosshair) {
      var cx = dw >> 1, cy = dh >> 1;

      for (var a = 2; a <= 7; a++) {
        for (var s = -1; s <= 1; s++) {
          var col = s === 0 ? t.fg : t.bg;
          var yy = cy + s, xx = cx + s;

          if (yy >= 0 && yy < dh) {
            if (cx - a >= 0) out[yy * dw + cx - a] = col;
            if (cx + a < dw) out[yy * dw + cx + a] = col;
          }
          if (xx >= 0 && xx < dw) {
            if (cy - a >= 0) out[(cy - a) * dw + xx] = col;
            if (cy + a < dh) out[(cy + a) * dw + xx] = col;
          }
        }
      }
    }

    return out;
  }

  // 5->8 and 6->8 bit expansion by replication. A plain left shift is wrong: (31 << 3) is
  // 248, so white would come out grey and every bright colour slightly dim.
  var LUT5 = new Uint8Array(32), LUT6 = new Uint8Array(64);
  for (var a = 0; a < 32; a++) LUT5[a] = (a * 527 + 23) >> 6;
  for (var b = 0; b < 64; b++) LUT6[b] = (b * 259 + 33) >> 6;

  /**
   * A sink that paints onto a 240x135 canvas context. FRAME tiles go through putTile and
   * THERMAL messages through thermal(); the thermal frame is painted back over the canvas
   * part of every tile it owns whenever either arrives.
   */
  function canvasSink(ctx) {
    var therm = null; // { x, y, w, h, tiles, img } while the channel is on
    var pal = null; // The last palette sent; later messages may omit it
    var taps = { key: '', x: null, y: null };

    function owns(tile) {
      return therm !== null && (therm.tiles[tile >> 3] & (1 << (tile & 7))) !== 0;
    }

    function paintOver(col, row) {
      var x0 = Math.max(col * TILE_W, therm.x), x1 = Math.min((col + 1) * TILE_W, therm.x + therm.w);
      var y0 = Math.max(row * TILE_H, therm.y), y1 = Math.min((row + 1) * TILE_H, therm.y + therm.h);
      if (x1 > x0 && y1 > y0) {
        ctx.putImageData(therm.img, therm.x, therm.y, x0 - therm.x, y0 - therm.y, x1 - x0, y1 - y0);
      }
    }

    return {
      /**
       * Apply one decodeThermal() result.
       * @returns {boolean} false if it could not be drawn for want of a palette, which a
       *          keyframe request fixes
       */
      thermal: function (t) {
        if (!t.on) { therm = null; return true; }
        if (t.palette) pal = t.palette;
        if (!pal) { therm = null; return false; }

        var key = t.w + ',' + t.h + ',' + t.cols + ',' + t.rows + ',' + t.flipH + ',' + t.flipV;
        if (key !== taps.key) {
          taps = { key: key, x: thermalTaps(t.w, t.cols, t.flipH), y: thermalTaps(t.h, t.rows, t.flipV) };
        }

        var px = thermalRender(t, pal, taps.x, taps.y);
        var img = ctx.createImageData(t.w, t.h);
        var d = img.data;

        for (var i = 0; i < px.length; i++) {
          var c = px[i], o = i * 4;
          d[o] = LUT5[(c >> 11) & 0x1F]; d[o + 1] = LUT6[(c >> 5) & 0x3F]; d[o + 2] = LUT5[c & 0x1F]; d[o + 3] = 255;
        }

        therm = { x: t.x, y: t.y, w: t.w, h: t.h, tiles: t.tiles, img: img };

        for (var tile = 0; tile < GRID_COLS * GRID_ROWS; tile++) {
          if (owns(tile)) paintOver(tile % GRID_COLS, (tile / GRID_COLS) | 0);
        }
        return true;
      },

      putTile: function (px, tw, th, col, row, half) {
        var step = half ? 2 : 1;
        var img = ctx.createImageData(tw * step, th * step);
        var d = img.data;

        for (var y = 0; y < th; y++) {
          for (var x = 0; x < tw; x++) {
            var c = px[y * tw + x];
            var r = LUT5[(c >> 11) & 0x1F], g = LUT6[(c >> 5) & 0x3F], bl = LUT5[c & 0x1F];

            // At half scale one sample stands for a 2x2 block of the real screen
            for (var dy = 0; dy < step; dy++) {
              for (var dx = 0; dx < step; dx++) {
                var o = (((y * step + dy) * tw * step) + (x * step + dx)) * 4;
                d[o] = r; d[o + 1] = g; d[o + 2] = bl; d[o + 3] = 255;
              }
            }
          }
        }

        // A half-scale tile spans 16x16 but a tile is only 15 tall, so clip the last row
        ctx.putImageData(img, col * TILE_W, row * TILE_H, 0, 0, TILE_W, TILE_H);

        // Under the thermal canvas the stream carries a constant placeholder
        if (owns(row * GRID_COLS + col)) paintOver(col, row);
      },
    };
  }

  return {
    decodeFrame: decodeFrame,
    decodeThermal: decodeThermal,
    thermalTaps: thermalTaps,
    thermalRender: thermalRender,
    canvasSink: canvasSink,
    MSG_THERMAL: MSG_THERMAL,
    LUT5: LUT5,
    LUT6: LUT6,
    TILE_W: TILE_W,
    TILE_H: TILE_H,
    FLAG_KEYFRAME: FLAG_KEYFRAME,
    FLAG_HALF: FLAG_HALF,
    FLAG_LAST: FLAG_LAST,
    FLAG_REDACTED: FLAG_REDACTED,
  };
});
