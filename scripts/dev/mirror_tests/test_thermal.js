/* Holds the browser's thermal port to the device's render math, pixel for pixel, then
 * replays a scripted session through the real canvas sink and checks what the viewer
 * shows at every checkpoint. Inputs come from test_thermal.c.
 *
 *   node test_thermal.js <render.bin> <stream.bin> */

const fs = require('fs');
const path = require('path');

const MirrorDecode = require(path.join(__dirname, '..', '..', 'relay', 'decode.js'));

const W = 240, H = 135;
const [, , renderPath, streamPath] = process.argv;

let bad = 0;
const fail = (msg) => { console.log(`  !! ${msg}`); bad++; };
const pass = (msg) => console.log(`  ok  ${msg}`);

function reader(buf) {
  let off = 0;
  return {
    more: () => off < buf.length,
    u8: () => buf.readUInt8(off++),
    u16: () => { const v = buf.readUInt16LE(off); off += 2; return v; },
    u32: () => { const v = buf.readUInt32LE(off); off += 4; return v; },
    i16s: (n) => { const a = new Int16Array(n); for (let i = 0; i < n; i++) { a[i] = buf.readInt16LE(off); off += 2; } return a; },
    u16s: (n) => { const a = new Uint16Array(n); for (let i = 0; i < n; i++) { a[i] = buf.readUInt16LE(off); off += 2; } return a; },
    bytes: (n) => { const ab = buf.buffer.slice(buf.byteOffset + off, buf.byteOffset + off + n); off += n; return ab; },
    str: (n) => { const s = buf.toString('latin1', off, off + n); off += n; return s; },
  };
}

/* ---- 1. taps and render against the C ---- */

console.log('=== taps and render vs lcd_ir_exp_render.c ===');

const rv = reader(fs.readFileSync(renderPath));
let tapTables = 0, tapBad = 0, cases = 0, caseBad = 0;
const thermalMsgs = [];

while (rv.more()) {
  const tag = String.fromCharCode(rv.u8());

  if (tag === 'T') {
    const dst = rv.u16(), src = rv.u16(), flip = rv.u8() === 1;
    const idx = rv.i16s(dst * 4), w = rv.i16s(dst * 4);
    const t = MirrorDecode.thermalTaps(dst, src, flip);
    tapTables++;

    for (let i = 0; i < dst * 4; i++) {
      if (t.idx[i] !== idx[i] || t.w[i] !== w[i]) {
        if (!tapBad++) console.log(`  !! taps ${dst} from ${src}${flip ? ' flipped' : ''} differ at ${i >> 2}.${i & 3}: ` +
          `got ${t.idx[i]}/${t.w[i]} want ${idx[i]}/${w[i]}`);
        break;
      }
    }
  } else if (tag === 'V') {
    const len = rv.u32();
    const msg = rv.bytes(len);
    const w = rv.u16(), h = rv.u16();
    const want = rv.u16s(w * h);
    thermalMsgs.push(msg);
    cases++;

    const t = MirrorDecode.decodeThermal(msg);
    if (!t || !t.on || !t.palette || t.w !== w || t.h !== h) {
      if (!caseBad++) console.log(`  !! case ${cases - 1}: message did not decode as a full ON frame`);
      continue;
    }

    const got = MirrorDecode.thermalRender(t, t.palette,
      MirrorDecode.thermalTaps(t.w, t.cols, t.flipH), MirrorDecode.thermalTaps(t.h, t.rows, t.flipV));

    let diff = 0, first = -1;
    for (let i = 0; i < w * h; i++) if (got[i] !== want[i]) { if (first < 0) first = i; diff++; }
    if (diff) {
      if (!caseBad++) console.log(`  !! case ${cases - 1} (${t.cols}x${t.rows} -> ${w}x${h}, lo ${t.lo} hi ${t.hi}): ` +
        `${diff} px differ, first at (${first % w},${(first / w) | 0}) got ${got[first].toString(16)} want ${want[first].toString(16)}`);
    }
  } else {
    fail(`unknown record '${tag}' in ${renderPath}`);
    break;
  }
}

if (tapBad) fail(`${tapBad}/${tapTables} tap tables differ from the C`);
else pass(`${tapTables} tap tables identical to the C`);
if (caseBad) fail(`${caseBad}/${cases} renders differ from the C`);
else pass(`${cases} renders pixel-identical to the C, the page's own configuration included`);

/* ---- 2. a session through the real canvas sink ---- */

console.log('\n=== scripted session through canvasSink ===');

// Just enough of CanvasRenderingContext2D, with putImageData's dirty-rectangle rules as
// the HTML spec gives them: replace, never blend, clipped to the image and the canvas
function fakeCtx() {
  const fb = new Uint8ClampedArray(W * H * 4);
  return {
    fb,
    createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
    putImageData(img, dx, dy, sx, sy, sw, sh) {
      if (sx === undefined) { sx = 0; sy = 0; sw = img.width; sh = img.height; }
      if (sw < 0) { sx += sw; sw = -sw; }
      if (sh < 0) { sy += sh; sh = -sh; }
      if (sx < 0) { sw += sx; sx = 0; }
      if (sy < 0) { sh += sy; sy = 0; }
      if (sx + sw > img.width) sw = img.width - sx;
      if (sy + sh > img.height) sh = img.height - sy;
      if (sw <= 0 || sh <= 0) return;

      for (let y = sy; y < sy + sh; y++) {
        for (let x = sx; x < sx + sw; x++) {
          const X = dx + x, Y = dy + y;
          if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
          const s = (y * img.width + x) * 4, d = (Y * W + X) * 4;
          fb[d] = img.data[s]; fb[d + 1] = img.data[s + 1]; fb[d + 2] = img.data[s + 2]; fb[d + 3] = img.data[s + 3];
        }
      }
    },
  };
}

let ctx = fakeCtx(), sink = MirrorDecode.canvasSink(ctx);
let msgs = 0, thermals = 0, checkpoints = 0, cpBad = 0;
const L5 = MirrorDecode.LUT5, L6 = MirrorDecode.LUT6;
const sv = reader(fs.readFileSync(streamPath));

while (sv.more()) {
  const tag = String.fromCharCode(sv.u8());

  if (tag === 'M') {
    const msg = sv.bytes(sv.u32());
    const type = new Uint8Array(msg)[0];
    msgs++;

    if (type === MirrorDecode.MSG_THERMAL) {
      thermals++;
      const t = MirrorDecode.decodeThermal(msg);
      if (!t) { fail(`THERMAL message ${msgs - 1} rejected`); continue; }
      if (!sink.thermal(t)) fail(`THERMAL message ${msgs - 1} arrived with no palette to draw it`);
      thermalMsgs.push(msg);
    } else if (!MirrorDecode.decodeFrame(msg, sink)) {
      fail(`FRAME message ${msgs - 1} rejected`);
    }
  } else if (tag === 'R') {
    ctx = fakeCtx();
    sink = MirrorDecode.canvasSink(ctx);
  } else if (tag === 'E') {
    const name = sv.str(sv.u8());
    const want = sv.u16s(W * H);
    checkpoints++;

    let diff = 0, first = -1;
    for (let i = 0; i < W * H; i++) {
      const c = want[i], o = i * 4;
      if (ctx.fb[o] !== L5[(c >> 11) & 0x1F] || ctx.fb[o + 1] !== L6[(c >> 5) & 0x3F] ||
          ctx.fb[o + 2] !== L5[c & 0x1F] || ctx.fb[o + 3] !== 255) {
        if (first < 0) first = i;
        diff++;
      }
    }

    if (diff) {
      const o = first * 4, c = want[first];
      fail(`"${name}": ${diff} px differ, first at (${first % W},${(first / W) | 0}) got ` +
        `rgb(${ctx.fb[o]},${ctx.fb[o + 1]},${ctx.fb[o + 2]}) want rgb(${L5[(c >> 11) & 0x1F]},${L6[(c >> 5) & 0x3F]},${L5[c & 0x1F]})`);
      cpBad++;
    }
  } else {
    fail(`unknown record '${tag}' in ${streamPath}`);
    break;
  }
}

console.log(`      ${msgs} messages (${thermals} THERMAL), ${checkpoints} checkpoints`);
if (!cpBad) pass(`every checkpoint pixel-exact: the viewer shows exactly what the panel shows`);

/* ---- 3. malformed THERMAL messages ---- */

console.log('\n=== malformed THERMAL messages ===');

let fuzzed = 0, threw = 0, seed = 7;
const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed % n; };
const fz = MirrorDecode.canvasSink(fakeCtx());

function tryOne(dv) {
  fuzzed++;
  try {
    const t = MirrorDecode.decodeThermal(dv);
    if (t) fz.thermal(t);
  } catch (e) {
    if (!threw++) console.log(`  !! threw instead of rejecting: ${e.message}`);
  }
}

for (const ab of thermalMsgs.slice(0, 16)) {
  for (let n = 0; n < ab.byteLength; n += 7) tryOne(new DataView(ab, 0, n));
  for (let k = 0; k < 128; k++) {
    const u8 = new Uint8Array(ab.slice(0));
    u8[rnd(Math.min(u8.length, 64))] = rnd(256); // The header is where the damage lands
    tryOne(new DataView(u8.buffer));
  }
}

if (threw) fail(`${threw}/${fuzzed} malformed messages threw`);
else pass(`${fuzzed} truncated or corrupted messages, none threw`);

console.log(bad ? `FAILED (${bad})` : 'ALL PASS');
process.exit(bad ? 1 : 0);
