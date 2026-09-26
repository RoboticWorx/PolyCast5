/**
 * Local Screen Mirror relay, for bench testing without Cloudflare.
 *
 * Speaks the same protocol as the Worker in src/index.js and also serves the test viewer
 * page, so a device on your LAN can be mirrored with nothing deployed anywhere.
 *
 *   npm install ws
 *   node dev-relay.js
 *
 * Then point the device at this machine. From an ESP-IDF monitor console, or by building
 * with the NVS override, set the relay URI to:
 *
 *   ws://<this-pc-ip>:8080/d
 *
 * and open http://<this-pc-ip>:8080/ in a browser.
 *
 * Plain ws:// on purpose: TLS is the last thing to add, not the first. The production
 * relay is wss:// and the Shopify page requires it (the storefront is HTTPS, so a
 * browser would block mixed content).
 *
 * VIEW_LIMIT sets the /v attempts allowed per client IP per minute (default 10, as the
 * Worker; 0 turns it off).
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');

const PORT = process.env.PORT ? Number(process.env.PORT) : 8080;
const VIEW_LIMIT = process.env.VIEW_LIMIT !== undefined ? Number(process.env.VIEW_LIMIT) : 10;
const VIEW_WINDOW_MS = 60 * 1000;

const ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
const CODE_LEN = 6;
const CHECK_LEN = 3;

const UNCLAIMED_TTL_MS = 10 * 60 * 1000;
const SESSION_MAX_MS = 60 * 60 * 1000;

/**
 * @type {Map<string, {device: import('ws').WebSocket, viewer: import('ws').WebSocket|null,
 *   at: number, resume: string|null, check: string|null, locked: boolean,
 *   timer: NodeJS.Timeout|null}>}
 */
const sessions = new Map();

/** @type {Map<string, {start: number, n: number}>} */
const attempts = new Map();

function randomChars(len) {
  let out = '';
  for (let i = 0; i < len; i++) out += ALPHABET[crypto.randomInt(ALPHABET.length)];
  return out;
}

// Never the check just replaced, as on the Worker
function freshCheck(prev) {
  let check = randomChars(CHECK_LEN);
  while (check === prev) check = randomChars(CHECK_LEN);
  return check;
}

function normalizeCode(raw) {
  if (!raw) return null;
  const s = String(raw)
    .toUpperCase()
    .replace(/[^0-9A-Z]/g, '')
    .replace(/O/g, '0')
    .replace(/[IL]/g, '1')
    .replace(/U/g, 'V');
  return s.length === CODE_LEN ? s : null;
}

function isOpen(ws) {
  return !!ws && ws.readyState === ws.OPEN;
}

function send(ws, obj) {
  if (isOpen(ws)) ws.send(JSON.stringify(obj));
}

function refuse(socket, status, text, body) {
  socket.write(`HTTP/1.1 ${status} ${text}\r\nContent-Type: text/plain\r\n` +
    `Content-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\n\r\n${body}`);
  socket.destroy();
}

// Fixed window per client IP, standing in for the Worker's VIEW_LIMITER binding
function limited(ip) {
  if (!VIEW_LIMIT) return false;

  const now = Date.now();
  let a = attempts.get(ip);

  if (!a || now - a.start >= VIEW_WINDOW_MS) {
    a = { start: now, n: 0 };
    attempts.set(ip, a);
  }
  return ++a.n > VIEW_LIMIT;
}

// One timer per session, standing in for the Durable Object's single alarm
function armTimer(s, at) {
  clearTimeout(s.timer);
  s.timer = setTimeout(() => {
    for (const ws of [s.device, s.viewer]) {
      send(ws, { t: 'bye', reason: 'timeout' });
      if (ws) ws.close(1001, 'timeout');
    }
  }, Math.max(0, at - Date.now()));
}

const server = http.createServer((req, res) => {
  if (req.url === '/health') {
    res.writeHead(200).end('ok');
    return;
  }

  // Serve the test viewer and the shared decoder sitting next to this file
  const name = req.url === '/' || !req.url ? 'viewer.html' : path.basename(req.url.split('?')[0]);
  const allowed = { 'viewer.html': 'text/html; charset=utf-8', 'decode.js': 'text/javascript' };

  if (!allowed[name]) {
    res.writeHead(404).end('not found');
    return;
  }

  const file = path.join(__dirname, name);

  if (fs.existsSync(file)) {
    res.writeHead(200, { 'Content-Type': allowed[name] });
    res.end(fs.readFileSync(file));
  } else {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end(name + ' not found next to dev-relay.js');
  }
});

const SUBPROTOCOL = 'pc5.mirror.v1';

// Name the subprotocol back when the firmware offers it. The browser offers none, and
// then ws never calls this
const wss = new WebSocketServer({
  noServer: true,
  handleProtocols: (protocols) => (protocols.has(SUBPROTOCOL) ? SUBPROTOCOL : false),
});

server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url, 'http://localhost');

  if (url.pathname === '/d') {
    // Refuse a collision rather than displacing the device already on that code, which
    // is what the Worker does. Astronomically unlikely, but silently hijacking a live
    // session is the wrong failure
    let code = randomChars(CODE_LEN);
    for (let tries = 0; sessions.has(code) && tries < 8; tries++) code = randomChars(CODE_LEN);

    if (sessions.has(code)) {
      refuse(socket, 409, 'Conflict', 'code in use');
      return;
    }

    wss.handleUpgrade(req, socket, head, (ws) => {
      const s = {
        device: ws, viewer: null, at: Date.now(),
        resume: null, check: null, locked: false, timer: null,
      };

      sessions.set(code, s);
      ws._code = code;
      ws._role = 'device';

      console.log(`[relay] device connected, code ${code}`);
      armTimer(s, s.at + UNCLAIMED_TTL_MS);
      send(ws, { t: 'ready', code, ttl: UNCLAIMED_TTL_MS / 1000 });

      wireUp(ws);
    });
    return;
  }

  if (url.pathname === '/v') {
    const code = normalizeCode(url.searchParams.get('code'));

    if (!code) {
      refuse(socket, 400, 'Bad Request', 'bad code');
      return;
    }
    if (limited(req.socket.remoteAddress || 'unknown')) {
      refuse(socket, 429, 'Too Many Requests', 'too many attempts');
      return;
    }

    // No device, a slot already taken and a wrong resume token all look the same, as on
    // the Worker. The token only counts once the device has approved a viewer
    const s = sessions.get(code);

    if (!s || !isOpen(s.device) || isOpen(s.viewer) ||
        (s.locked && url.searchParams.get('resume') !== s.resume)) {
      refuse(socket, 404, 'Not Found', 'no such code');
      return;
    }

    // Without verifyClient, ws runs this callback synchronously, so two racing attaches
    // cannot both pass the check above
    wss.handleUpgrade(req, socket, head, (ws) => {
      // Until the lock every attach is a new viewer, so one that left unapproved holds nothing
      if (!s.locked) {
        s.resume = crypto.randomUUID();
        s.check = freshCheck(s.check);
      }

      s.viewer = ws;
      ws._code = code;
      ws._role = 'viewer';

      console.log(`[relay] viewer attached to ${code}, check ${s.check}`);
      send(ws, { t: 'paired', w: 240, h: 135, resume: s.resume, check: s.check });
      send(s.device, { t: 'viewer', state: 'attached', check: s.check });
      armTimer(s, s.at + SESSION_MAX_MS);

      wireUp(ws);
    });
    return;
  }

  refuse(socket, 404, 'Not Found', 'not found');
});

// {"t":"approved","check":...} from the device: lock the session to the viewer showing that
// check. One no longer attached (it left, or another took its place) locks nothing. The
// device streams only after {"t":"locked"}, as on the Worker
function deviceControl(s, code, data) {
  let m;
  try {
    m = JSON.parse(data.toString());
  } catch (e) {
    return;
  }

  if (!m || m.t !== 'approved' || s.locked || !s.check || m.check !== s.check ||
      !isOpen(s.viewer)) {
    return;
  }

  s.locked = true;
  console.log(`[relay] ${code} locked to the viewer with check ${s.check}`);
  send(s.device, { t: 'locked', check: s.check });
}

function wireUp(ws) {
  let bytes = 0;
  let lastReport = Date.now();

  ws.on('message', (data, isBinary) => {
    const s = sessions.get(ws._code);
    if (!s) return;

    // Text is relay control, and only the device has any; binary is passed straight through
    if (!isBinary) {
      if (ws._role === 'device') deviceControl(s, ws._code, data);
      return;
    }

    const peer = ws._role === 'device' ? s.viewer : s.device;
    if (isOpen(peer)) peer.send(data, { binary: true });

    if (ws._role === 'device') {
      bytes += data.length;
      const now = Date.now();

      if (now - lastReport >= 2000) {
        const kbs = bytes / 1024 / ((now - lastReport) / 1000);
        console.log(`[relay] ${ws._code}: ${kbs.toFixed(1)} KB/s from device`);
        bytes = 0;
        lastReport = now;
      }
    }
  });

  // A protocol violation emits 'error' and then closes; unhandled, it kills the relay
  ws.on('error', (e) => console.log(`[relay] ${ws._code} ${ws._role}: ${e.message}`));

  ws.on('close', () => {
    const s = sessions.get(ws._code);
    if (!s) return;

    if (ws._role === 'device') {
      console.log(`[relay] device gone, dropping ${ws._code}`);
      clearTimeout(s.timer);
      if (isOpen(s.viewer)) s.viewer.close(1001, 'device_gone');
      s.viewer = null;
      sessions.delete(ws._code);
    } else if (s.viewer === ws) {
      // A refreshed tab can take the slot while this socket is still closing; only the
      // current viewer's close means the device lost it
      console.log(`[relay] viewer detached from ${ws._code}`);
      s.viewer = null;
      send(s.device, { t: 'viewer', state: 'detached' });
    }
  });
}

server.listen(PORT, () => {
  console.log(`PolyCast5 Screen Mirror dev relay on :${PORT}`);
  console.log(`  device -> ws://<this-pc-ip>:${PORT}/d`);
  console.log(`  viewer -> http://<this-pc-ip>:${PORT}/`);
});
