/**
 * PolyCast5 Screen Mirror relay.
 *
 * One Durable Object per pairing code, holding at most one device socket and one viewer
 * socket and forwarding binary frames between them. The code IS the routing key, so there
 * is no session table and no KV. Session state (connect time, resume token, check code,
 * lock) rides on the device socket's hibernation attachment and dies with it; the only
 * storage the object touches is its alarm, which is deleted when the device leaves.
 *
 * Text frames are relay control (JSON). Binary frames are application payload and are
 * passed through byte for byte, so the firmware's wire format can change without the
 * relay knowing or caring.
 */

// Crockford base32 without I, L, O and U, so a code read off a 240x135 screen and typed
// on a phone cannot be ambiguous
const ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
const CODE_LEN = 6;
const CHECK_LEN = 3;

const SUBPROTOCOL = 'pc5.mirror.v1';

const UNCLAIMED_TTL_MS = 10 * 60 * 1000; // Code dies if no viewer joins
const SESSION_MAX_MS = 60 * 60 * 1000; // Hard cap from device connect

// A guess for a code nobody holds, a slot already taken and a wrong resume token all look
// the same, so a probe learns nothing about which codes are live
function noSuchCode() {
  return new Response('no such code', { status: 404 });
}

// 32 divides 256, so the modulo is unbiased
function randomChars(len) {
  const bytes = new Uint8Array(len);
  crypto.getRandomValues(bytes);

  let out = '';
  for (let i = 0; i < len; i++) out += ALPHABET[bytes[i] % ALPHABET.length];
  return out;
}

// Never the check just replaced: the device voids its approval on any other check, so a
// repeat is the one draw that could ride an approval the relay never locked
function freshCheck(prev) {
  let check = randomChars(CHECK_LEN);
  while (check === prev) check = randomChars(CHECK_LEN);
  return check;
}

function normalizeCode(raw) {
  if (!raw) return null;

  // Accept lowercase, spaces and dashes, and fold the letters people habitually
  // substitute for digits. Crockford's own rules.
  const s = raw
    .toUpperCase()
    .replace(/[^0-9A-Z]/g, '')
    .replace(/O/g, '0')
    .replace(/[IL]/g, '1')
    .replace(/U/g, 'V');

  if (s.length !== CODE_LEN) return null;
  for (const ch of s) if (!ALPHABET.includes(ch)) return null;
  return s;
}

// One IPv6 client owns a whole /64, so keying on the full address would let it rotate
// past the limit
function limitKey(ip) {
  if (!ip.includes(':') || ip.includes('.')) return ip;

  const [head, tail = ''] = ip.split('::');
  const h = head ? head.split(':') : [];
  const t = tail ? tail.split(':') : [];
  return [...h, ...Array(Math.max(0, 8 - h.length - t.length)).fill('0'), ...t]
    .slice(0, 4)
    .join(':');
}

export class MirrorSession {
  constructor(state, env) {
    this.state = state;
    this.env = env;
  }

  // Only OPEN sockets count: getWebSockets() still returns one the client has closed
  // until the close handshake finishes, and that must not hold a slot
  sockets(tag) {
    return this.state
      .getWebSockets()
      .filter((ws) => ws.readyState === WebSocket.OPEN && this.state.getTags(ws).includes(tag));
  }

  peerOf(ws) {
    const want = this.state.getTags(ws).includes('device') ? 'viewer' : 'device';
    return this.sockets(want)[0] || null;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const role = url.searchParams.get('role');
    const code = url.searchParams.get('code');

    if (request.headers.get('Upgrade') !== 'websocket') {
      return new Response('expected websocket', { status: 426 });
    }

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);

    // RFC 6455: if the client offered a subprotocol we must name the one we selected.
    // The firmware offers pc5.mirror.v1; the browser offers none
    const offered = (request.headers.get('Sec-WebSocket-Protocol') || '')
      .split(',')
      .map((p) => p.trim());
    const accepted = offered.includes(SUBPROTOCOL) ? SUBPROTOCOL : null;

    if (role === 'device') {
      if (this.sockets('device').length > 0) {
        return new Response('code in use', { status: 409 });
      }

      const at = Date.now();

      this.state.acceptWebSocket(server, ['device']);
      server.serializeAttachment({ at, resume: null, check: null, locked: false });
      await this.state.storage.setAlarm(at + UNCLAIMED_TTL_MS);
      server.send(JSON.stringify({ t: 'ready', code, ttl: UNCLAIMED_TTL_MS / 1000 }));
    } else {
      // Check the slot and take it with no await in between, or two attaches racing on
      // the same code could both pass
      const device = this.sockets('device')[0];
      if (!device || this.sockets('viewer').length > 0) {
        return noSuchCode();
      }

      // Once the device has approved a viewer only its token gets back in. Until then every
      // attach is a new viewer with a fresh pair, so one that left unapproved holds nothing
      const session = device.deserializeAttachment();
      if (session.locked) {
        if (url.searchParams.get('resume') !== session.resume) {
          return noSuchCode();
        }
      } else {
        session.resume = crypto.randomUUID();
        session.check = freshCheck(session.check);
        device.serializeAttachment(session);
      }

      this.state.acceptWebSocket(server, ['viewer']);

      server.send(JSON.stringify({
        t: 'paired', w: 240, h: 135, resume: session.resume, check: session.check,
      }));
      device.send(JSON.stringify({ t: 'viewer', state: 'attached', check: session.check }));

      // A Durable Object has ONE alarm, so this replaces the unclaimed-code TTL. The target
      // is absolute, so a re-attach rewrites the same instant instead of sliding the cap
      await this.state.storage.setAlarm(session.at + SESSION_MAX_MS);
    }

    return new Response(null, {
      status: 101,
      webSocket: client,
      headers: accepted ? { 'Sec-WebSocket-Protocol': accepted } : undefined,
    });
  }

  // Binary is the application's business, not ours. Text is relay control, and only the
  // device has any: the viewer's is ignored
  webSocketMessage(ws, msg) {
    if (typeof msg !== 'string') {
      this.peerOf(ws)?.send(msg);
    } else if (this.state.getTags(ws).includes('device')) {
      this.deviceControl(ws, msg);
    }
  }

  // {"t":"approved","check":...}: the user approved the viewer showing that check, so only
  // its token may re-attach from now on. A check that is no longer the attached viewer's
  // (it left, or another took its place) locks nothing. {"t":"locked"} confirms a lock:
  // the device streams nothing before it, since until then the slot can change hands
  deviceControl(device, text) {
    let m;
    try {
      m = JSON.parse(text);
    } catch (e) {
      return;
    }

    const session = device.deserializeAttachment();
    if (!m || m.t !== 'approved' || session.locked || !session.check ||
        m.check !== session.check || this.sockets('viewer').length === 0) {
      return;
    }

    session.locked = true;
    device.serializeAttachment(session);

    // A device whose close is already in flight gets nothing; its session ends with it
    if (device.readyState === WebSocket.OPEN) {
      device.send(JSON.stringify({ t: 'locked', check: session.check }));
    }
  }

  async webSocketClose(ws) {
    const isDevice = this.state.getTags(ws).includes('device');

    // Before compatibility date 2026-04-07 the runtime leaves a client-closed socket in
    // CLOSING until we reply
    try {
      ws.close(1000, 'closed');
    } catch (e) {
      // Already closed
    }

    const peer = this.peerOf(ws);

    if (isDevice) {
      peer?.close(1001, 'device_gone');
      await this.state.storage.deleteAlarm();
    } else if (peer && this.sockets('viewer').length === 0) {
      // A refreshed tab can attach while its old socket is still closing; that late
      // close must not report the new viewer gone
      peer.send(JSON.stringify({ t: 'viewer', state: 'detached' }));
    }
  }

  webSocketError(ws) {
    return this.webSocketClose(ws);
  }

  async alarm() {
    for (const ws of this.state.getWebSockets()) {
      try {
        ws.send(JSON.stringify({ t: 'bye', reason: 'timeout' }));
        ws.close(1001, 'timeout');
      } catch (e) {
        // Already gone
      }
    }
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === '/health') {
      return new Response('ok');
    }

    if (request.headers.get('Upgrade') !== 'websocket') {
      return new Response('PolyCast5 Screen Mirror relay', { status: 200 });
    }

    // Device: allocate a fresh code and route to its own object
    if (url.pathname === '/d') {
      const code = randomChars(CODE_LEN);
      const id = env.SESSION.idFromName(code);
      const target = new URL(request.url);

      target.searchParams.set('role', 'device');
      target.searchParams.set('code', code);

      return env.SESSION.get(id).fetch(new Request(target, request));
    }

    // Viewer: the code they typed selects the same object
    if (url.pathname === '/v') {
      const code = normalizeCode(url.searchParams.get('code'));

      if (!code) {
        return new Response('bad code', { status: 400 });
      }

      // Before routing, so a refused guess never wakes a Durable Object. Skipped when the
      // [[ratelimits]] binding is absent
      if (env.VIEW_LIMITER) {
        const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
        const { success } = await env.VIEW_LIMITER.limit({ key: limitKey(ip) });

        if (!success) {
          return new Response('too many attempts', { status: 429 });
        }
      }

      const id = env.SESSION.idFromName(code);
      const target = new URL(request.url);

      target.searchParams.set('role', 'viewer');
      target.searchParams.set('code', code);

      return env.SESSION.get(id).fetch(new Request(target, request));
    }

    return new Response('not found', { status: 404 });
  },
};
