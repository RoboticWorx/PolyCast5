/* End-to-end: dev relay + a fake device replaying real encoder frames + a viewer that
 * decodes them with the browser module and sends input back. */

const fs = require('fs');
const { spawn } = require('child_process');
const WebSocket = require('ws');

const path = require('path');
const MirrorDecode = require(path.join(__dirname, '..', '..', 'relay', 'decode.js'));

const SP = process.env.MIRROR_TEST_OUT || __dirname;
const RELAY = path.join(__dirname, '..', '..', 'relay', 'dev-relay.js');
const PORT = 8137;
const LIMIT_PORT = 8138; // A second relay allowing 2 attempts a minute, for the rate limit

const W = 240, H = 135;
let failures = 0;
const log = (s) => console.log(s);
const fail = (s) => { console.log('  !! ' + s); failures++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const CHECK_RE = /^[0-9A-HJKMNP-TV-Z]{3}$/;

// Split the dump back into individual messages, as the device would send them
function loadMessages(p) {
  const buf = fs.readFileSync(p);
  const out = [];
  let off = 0;
  while (off + 4 <= buf.length) {
    const len = buf.readUInt32LE(off);
    off += 4;
    out.push(Buffer.from(buf.subarray(off, off + len)));
    off += len;
  }
  return out;
}

const messages = loadMessages(`${SP}/frames.bin`);
const expect = fs.readFileSync(`${SP}/expect.raw`);

const canvas = new Uint16Array(W * H);
const sink = {
  putTile(px, tw, th, col, row, half) {
    const step = half ? 2 : 1;
    const ox = col * 16, oy = row * 15;
    for (let y = 0; y < th; y++) for (let x = 0; x < tw; x++) {
      const c = px[y * tw + x];
      for (let dy = 0; dy < step; dy++) for (let dx = 0; dx < step; dx++) {
        const gx = ox + x * step + dx, gy = oy + y * step + dy;
        if (gx >= ox + 16 || gy >= oy + 15 || gx >= W || gy >= H) continue;
        canvas[gy * W + gx] = c;
      }
    }
  },
};

let relayOut = '';

function startRelay(port, viewLimit) {
  const p = spawn(process.execPath, [RELAY], {
    env: { ...process.env, PORT: String(port), VIEW_LIMIT: String(viewLimit) },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  p.stdout.on('data', (d) => { relayOut += d.toString(); });
  p.stderr.on('data', (d) => { relayOut += d.toString(); });
  return p;
}

const relays = [startRelay(PORT, 1000), startRelay(LIMIT_PORT, 2)];

const done = () => {
  log(failures ? `\nFAILED (${failures})` : '\nALL PASS');
  if (failures) log('\n--- relay output ---\n' + relayOut);
  for (const r of relays) { try { r.kill(); } catch (_) {} }
  process.exit(failures ? 1 : 0);
};

// Resolves with the open socket (its relay control messages collected in .ctl), or the
// HTTP status the relay refused it with
function attach(query, port = PORT) {
  return new Promise((resolve) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/v?${query}`);
    ws.binaryType = 'arraybuffer';
    ws.ctl = [];
    ws.on('message', (data, isBinary) => { if (!isBinary) ws.ctl.push(JSON.parse(data.toString())); });
    ws.on('open', () => resolve({ ws, status: 101 }));
    ws.on('error', (e) => {
      const m = /server response: (\d+)/.exec(e.message);
      resolve({ ws: null, status: m ? Number(m[1]) : 0 });
    });
  });
}

function expectStatus(r, want, what) {
  if (r.ws) { fail(`${what} was accepted`); r.ws.close(); }
  else if (r.status !== want) fail(`${what} got ${r.status}, want ${want}`);
  else log(`  ok  ${what} refused with ${want}`);
}

setTimeout(async () => {
  log('=== relay end-to-end ===');

  // --- device side ---
  const dev = new WebSocket(`ws://127.0.0.1:${PORT}/d`, ['pc5.mirror.v1']);
  dev.binaryType = 'arraybuffer';

  let code = null;
  const devCtl = [];
  const inputSeen = [];
  const lastViewerMsg = () => devCtl.filter((m) => m.t === 'viewer').pop() || {};
  const lockMsgs = () => devCtl.filter((m) => m.t === 'locked');

  dev.on('message', (data, isBinary) => {
    if (!isBinary) {
      const m = JSON.parse(data.toString());
      if (m.t === 'ready') { code = m.code; log(`  device got code ${code}`); }
      devCtl.push(m);
      return;
    }
    inputSeen.push(Buffer.from(data));
  });

  await new Promise((r) => dev.on('open', r));
  await sleep(150);

  if (!code) { fail('device never received a pairing code'); return done(); }
  if (!/^[0-9A-HJKMNP-TV-Z]{6}$/.test(code)) fail(`code "${code}" is not 6 unambiguous chars`);
  else log('  ok  code is 6 chars from the unambiguous alphabet');

  if (dev.protocol !== 'pc5.mirror.v1') fail(`subprotocol not echoed (got "${dev.protocol}")`);
  else log('  ok  relay echoed the subprotocol the firmware offers');

  // --- refusals ---
  expectStatus(await attach('code=ZZZZZZ'), 404, 'a code no device holds');
  expectStatus(await attach('code=ABC'), 400, 'a malformed code');

  // --- first attach, using the lowercase/dashed form a human would type ---
  const typed = code.toLowerCase().slice(0, 3) + '-' + code.toLowerCase().slice(3);
  const first = await attach(`code=${encodeURIComponent(typed)}`);

  if (!first.ws) { fail(`first viewer refused with ${first.status}`); return done(); }
  log('  ok  viewer paired using a lowercase, dashed code');

  const view = first.ws;
  await sleep(150);

  const paired = view.ctl.find((m) => m.t === 'paired') || {};
  const { resume, check } = paired;

  if (paired.w !== W || paired.h !== H) fail(`"paired" carried ${paired.w}x${paired.h}`);
  if (!UUID_RE.test(resume || '')) fail(`resume token "${resume}" is not a v4 UUID`);
  else log('  ok  first attach was issued a resume token');
  if (!CHECK_RE.test(check || '')) fail(`check code "${check}" is not 3 unambiguous chars`);
  else log('  ok  first attach was issued a 3-char check code');

  if (lastViewerMsg().state !== 'attached') fail('device was not told a viewer attached');
  else if (lastViewerMsg().check !== check) fail(`device saw check "${lastViewerMsg().check}", viewer "${check}"`);
  else log('  ok  device notified of the attach, with the same check code');

  // --- stream every message ---
  let framesComplete = 0;
  const acks = [];

  view.on('message', (data, isBinary) => {
    if (!isBinary) return;
    const ab = data instanceof ArrayBuffer
      ? data
      : data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
    const r = MirrorDecode.decodeFrame(ab, sink);
    if (!r) { fail('viewer could not decode a relayed message'); return; }
    if (r.last) {
      framesComplete++;
      const a = Buffer.from([0x11, r.seq & 0xFF, r.seq >> 8]);
      acks.push(r.seq);
      view.send(a);
    }
  });

  for (const m of messages) {
    dev.send(m, { binary: true });
    await new Promise((r) => setImmediate(r));
  }
  await sleep(400);

  log(`  streamed ${messages.length} messages, viewer completed ${framesComplete} frames`);
  if (framesComplete !== 9) fail(`expected 9 complete frames, got ${framesComplete}`);
  else log('  ok  all frames arrived intact through the relay');

  let diff = 0;
  for (let i = 0; i < W * H; i++) if (canvas[i] !== expect.readUInt16LE(i * 2)) diff++;
  if (diff) fail(`${diff} px differ after relaying`);
  else log('  ok  relayed canvas is pixel-exact');

  // --- input travels the other way ---
  view.send(Buffer.from([0x10, 0, 2])); // SELECT tap
  view.send(Buffer.from([0x10, 2, 1])); // UP down
  view.send(Buffer.from([0x10, 2, 0])); // UP up
  view.send(Buffer.from([0x13]));       // keyframe request
  await sleep(250);

  // Acks are interleaved with the button traffic, so look for the messages by type
  const btnTap = inputSeen.find((m) => m[0] === 0x10 && m[1] === 0 && m[2] === 2);
  const btnDown = inputSeen.find((m) => m[0] === 0x10 && m[1] === 2 && m[2] === 1);
  const btnUp = inputSeen.find((m) => m[0] === 0x10 && m[1] === 2 && m[2] === 0);
  const keyReq = inputSeen.find((m) => m[0] === 0x13 && m.length === 1);

  if (!btnTap || btnTap.length !== 3) fail('SELECT-tap never reached the device intact');
  else if (!btnDown || btnDown.length !== 3) fail('UP-down never reached the device intact');
  else if (!btnUp) fail('UP-up never reached the device');
  else if (!keyReq) fail('keyframe request never reached the device');
  else log('  ok  button and keyframe messages reached the device byte-for-byte');

  if (acks.length !== framesComplete) fail('ack count does not match completed frames');
  else log(`  ok  viewer acked every completed frame (${acks.length})`);

  // --- the slot is taken, even for the token holder ---
  expectStatus(await attach(`code=${code}&resume=${resume}`), 404, 'a second concurrent viewer');

  // --- the viewer's tab closes, never approved ---
  view.close();
  await sleep(250);

  if (lastViewerMsg().state !== 'detached') fail('device was not told the viewer left');
  else log('  ok  device notified of viewer detach');

  const leave = async (ws) => { ws.close(); await sleep(250); };
  const approve = async (from, chk) => { from.send(JSON.stringify({ t: 'approved', check: chk })); await sleep(150); };
  const otherCheck = (c) => (c === 'AAA' ? 'BBB' : 'AAA');

  // Unlocked, so the attach must get in with a pair unlike the last one, and the device
  // must see the new check
  async function freshAttach(query, prev, what) {
    const r = await attach(query);
    if (!r.ws) { fail(`${what}: refused with ${r.status}`); return null; }
    await sleep(150);

    const p = r.ws.ctl.find((m) => m.t === 'paired') || {};
    r.ws.pair = p;

    if (!UUID_RE.test(p.resume || '') || !CHECK_RE.test(p.check || '')) fail(`${what}: malformed pair`);
    else if (p.resume === prev.resume || p.check === prev.check) fail(`${what}: pair not fresh`);
    else if (lastViewerMsg().state !== 'attached' || lastViewerMsg().check !== p.check) {
      fail(`${what}: device not told the new check`);
    } else {
      log(`  ok  ${what}`);
    }
    return r.ws;
  }

  // --- unlocked: a viewer that left unapproved holds nothing ---
  let cur = await freshAttach(`code=${code}`, paired,
    'another browser attaches after an unapproved viewer left, with a fresh token and check');
  if (!cur) return done();
  await leave(cur);

  // A tab refreshed before approval offers the token it was just given
  cur = await freshAttach(`code=${code}&resume=${encodeURIComponent(cur.pair.resume)}`, cur.pair,
    'a stale resume token while unlocked is ignored and a fresh pair issued');
  if (!cur) return done();

  // --- "approved" that must not lock ---
  await approve(cur, cur.pair.check);
  await leave(cur);
  cur = await freshAttach(`code=${code}`, cur.pair, 'an "approved" sent by the viewer locks nothing');
  if (!cur) return done();

  await approve(dev, otherCheck(cur.pair.check));
  await leave(cur);
  cur = await freshAttach(`code=${code}`, cur.pair, 'an "approved" with a mismatched check locks nothing');
  if (!cur) return done();

  // The relay still holds this check once the viewer is gone
  await leave(cur);
  await approve(dev, cur.pair.check);
  cur = await freshAttach(`code=${code}`, cur.pair, 'an "approved" with no viewer attached locks nothing');
  if (!cur) return done();

  if (lockMsgs().length) fail('device was sent "locked" for an approval that locked nothing');
  else log('  ok  no "locked" confirmation for any approval that locked nothing');

  // --- "approved" with the attached viewer's check locks ---
  const locked = cur.pair;
  await approve(dev, locked.check);

  if (lockMsgs().length !== 1 || lockMsgs()[0].check !== locked.check) {
    fail(`device got ${JSON.stringify(lockMsgs())} for the lock, want one "locked" with ${locked.check}`);
  } else {
    log('  ok  the lock is confirmed to the device with "locked" and the approved check');
  }
  expectStatus(await attach(`code=${code}&resume=${encodeURIComponent(locked.resume)}`), 404,
    'a second viewer while the approved one is attached');
  await leave(cur);

  expectStatus(await attach(`code=${code}`), 404, 'a rejoin without the resume token, once locked');
  expectStatus(await attach(`code=${code}&resume=00000000-0000-4000-8000-000000000000`), 404,
    'a rejoin with the wrong resume token, once locked');
  expectStatus(await attach(`code=${code}&resume=${encodeURIComponent(resume)}`), 404,
    'a rejoin with a token issued before the lock');

  const again = await attach(`code=${code}&resume=${encodeURIComponent(locked.resume)}`);

  if (!again.ws) {
    fail(`the approved viewer could not rejoin (${again.status})`);
  } else {
    await sleep(150);
    const p2 = again.ws.ctl.find((m) => m.t === 'paired') || {};

    if (p2.resume !== locked.resume || p2.check !== locked.check) {
      fail('a rejoin was issued a different token or check code');
    } else if (lastViewerMsg().state !== 'attached' || lastViewerMsg().check !== locked.check) {
      fail('device was not told of the rejoin with the same check code');
    } else {
      log('  ok  the approved viewer rejoined with the same token and check code');
    }

    // A later "approved", for another check or the same one, cannot move or repeat the lock
    await approve(dev, otherCheck(locked.check));
    await approve(dev, locked.check);
    if (lockMsgs().length !== 1) fail(`a later "approved" sent "locked" again (${lockMsgs().length})`);
    else log('  ok  a later "approved" leaves the lock alone and confirms nothing');
    await leave(again.ws);
  }

  // --- two tabs racing for the slot: exactly one wins ---
  const race = await Promise.all([
    attach(`code=${code}&resume=${locked.resume}`),
    attach(`code=${code}&resume=${locked.resume}`),
  ]);
  const winners = race.filter((r) => r.ws);
  const loser = race.find((r) => !r.ws);

  if (winners.length !== 1 || !loser || loser.status !== 404) {
    fail(`racing rejoins: ${winners.length} won, loser status ${loser && loser.status}`);
    for (const r of winners) r.ws.close();
    return done();
  }
  log('  ok  of two racing rejoins exactly one took the slot');

  // --- device going away closes the viewer ---
  const survivor = winners[0].ws;
  const viewerClosed = new Promise((r) => survivor.on('close', r));
  dev.close();
  await Promise.race([viewerClosed, sleep(1000)]);

  if (survivor.readyState !== WebSocket.CLOSED) fail('viewer survived the device disconnecting');
  else log('  ok  viewer dropped when the device went away');

  // --- rate limit, on the relay allowing 2 attempts a minute ---
  const statuses = [];
  for (let i = 0; i < 3; i++) statuses.push((await attach('code=ZZZZZZ', LIMIT_PORT)).status);

  if (statuses.join() !== '404,404,429') fail(`rate limit: got ${statuses.join()}, want 404,404,429`);
  else log('  ok  the attempt past the limit was refused with 429');

  done();
}, 600);
