/* Runs the real browser decoder module against real firmware encoder output, then checks
 * that malformed variants of it are rejected rather than thrown on. */

const fs = require('fs');
const path = require('path');

const MirrorDecode = require(path.join(__dirname, '..', '..', 'relay', 'decode.js'));

const W = 240, H = 135;
const [, , framesPath, expectPath] = process.argv;

const canvas = new Uint16Array(W * H);

// The same painting rule the canvas sink implements, into a plain buffer so the result
// can be compared pixel for pixel.
const sink = {
  putTile(px, tw, th, col, row, half) {
    const step = half ? 2 : 1;
    const ox = col * MirrorDecode.TILE_W, oy = row * MirrorDecode.TILE_H;

    for (let y = 0; y < th; y++) {
      for (let x = 0; x < tw; x++) {
        const c = px[y * tw + x];
        for (let dy = 0; dy < step; dy++) {
          for (let dx = 0; dx < step; dx++) {
            const gx = ox + x * step + dx, gy = oy + y * step + dy;
            if (gx >= ox + MirrorDecode.TILE_W || gy >= oy + MirrorDecode.TILE_H) continue;
            if (gx >= W || gy >= H) continue;
            canvas[gy * W + gx] = c;
          }
        }
      }
    }
  },
};

const buf = fs.readFileSync(framesPath);
const all = [];
let off = 0, msgs = 0, frames = 0, tiles = 0, bad = 0;

while (off + 4 <= buf.length) {
  const len = buf.readUInt32LE(off);
  off += 4;
  if (off + len > buf.length) { console.log('truncated stream'); bad++; break; }

  const slice = buf.buffer.slice(buf.byteOffset + off, buf.byteOffset + off + len);
  all.push(slice);
  const r = MirrorDecode.decodeFrame(slice, sink);

  if (!r) { console.log(`  !! message ${msgs} rejected by decoder (${len} B)`); bad++; }
  else { tiles += r.tiles; if (r.last) frames++; }

  msgs++;
  off += len;
}

console.log(`decoded ${msgs} messages, ${frames} complete frames, ${tiles} tiles`);

const expect = fs.readFileSync(expectPath);
let diff = 0, first = -1;

for (let i = 0; i < W * H; i++) {
  const want = expect.readUInt16LE(i * 2);
  if (canvas[i] !== want) { if (first < 0) first = i; diff++; }
}

if (diff) {
  console.log(`  !! ${diff}/${W * H} px differ; first at (${first % W},${(first / W) | 0}) ` +
    `got ${canvas[first].toString(16)} want ${expect.readUInt16LE(first * 2).toString(16)}`);
  bad++;
} else {
  console.log('  ok  final canvas is pixel-exact against the encoder input');
}

// The viewers request a keyframe when a FRAME is rejected. A throw skips that request, so
// every malformed input must come back as null or a result
const nullSink = { putTile() {} };
let fuzzed = 0, threw = 0, seed = 1;
const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed % n; };

function decodeSafe(dv) {
  fuzzed++;
  try {
    return MirrorDecode.decodeFrame(dv, nullSink);
  } catch (e) {
    if (!threw++) console.log(`  !! decoder threw instead of rejecting: ${e.message}`);
    return null;
  }
}

for (const ab of all.slice(0, 12)) {
  for (let n = 0; n < ab.byteLength; n++) decodeSafe(new DataView(ab, 0, n));

  for (let k = 0; k < 256; k++) {
    const u8 = new Uint8Array(ab.slice(0));
    u8[rnd(u8.length)] = rnd(256);
    decodeSafe(new DataView(u8.buffer));
  }
}

// PAL4 record with an empty payload as the last bytes of the message
const pal4Empty = new Uint8Array([0x01, 0x04, 0, 0, 16, 15, 15, 9, 1, 0, 0, 0, 3, 0, 0]);
if (decodeSafe(new DataView(pal4Empty.buffer)) !== null) {
  console.log('  !! empty PAL4 payload was accepted');
  bad++;
}

if (threw) {
  console.log(`  !! ${threw}/${fuzzed} malformed messages threw`);
  bad++;
} else {
  console.log(`  ok  ${fuzzed} truncated or corrupted messages, none threw`);
}

console.log(bad ? `FAILED (${bad})` : 'ALL PASS');
process.exit(bad ? 1 : 0);
