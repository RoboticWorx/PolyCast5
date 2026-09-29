# PolyCast5 Screen Mirror relay

Pairs a PolyCast5 with a browser so the device's screen can be watched and its buttons
pressed from anywhere. The device is behind home NAT and the firmware has no AP+STA mode,
so it can never be a server: it dials **out** to this relay, the browser dials out to the
same relay, and the two are matched by the 6-character code shown on the device.

The relay is a dumb byte pump. Text frames are relay control (JSON); binary frames are
application payload and are forwarded untouched, so the firmware's wire format can change
without the relay knowing.

```
PolyCast5 ──wss──►┐
                  ├─► mirror.polycast5.com ◄── browser (wss)
(behind NAT) ─────┘        pairs by code
```

## Files

| File | Purpose |
|---|---|
| `src/index.js` | The Cloudflare Worker: one Durable Object per pairing code |
| `wrangler.toml` | Worker config and the Durable Object binding |
| `dev-relay.js` | A local Node relay that speaks the same protocol, for bench testing |
| `viewer.html` | A plain test viewer, served by `dev-relay.js` |
| `decode.js` | The frame decoder, shared by `viewer.html` and the Shopify storefront page |

`decode.js` is also copied into the storefront theme as
`assets/pc5-mirror-decode.js`. **Keep the two byte-identical** — the storefront copy is
the one customers run, and this one is the one that gets tested.

## Bench testing, with nothing deployed

This is the fastest way to see the feature work, and it needs no Cloudflare account, no
DNS and no TLS.

```bash
npm install ws
node dev-relay.js
```

Then point the device at this machine. The relay URI is compiled in as
`wss://polycast5-mirror.polycast5.workers.dev/d` but is overridden by the NVS string `mirror/uri`, so set
that to:

```
ws://<your-pc-ip>:8080/d
```

Open `http://<your-pc-ip>:8080/` in a browser, put the device into
**Wi-Fi ▸ Screen Mirror**, and type the code it shows.

The dev relay prints the device's outbound throughput every two seconds, which is the
quickest way to see the quality ladder reacting to a busy screen. It also applies the
Worker's pairing rate limit; `VIEW_LIMIT=0 node dev-relay.js` turns that off.

Plain `ws://` is deliberate here: TLS is the last thing to add, not the first. Production
must be `wss://`, because the Shopify page is served over HTTPS and a browser blocks a
`ws://` connection from it as mixed content.

## Deploying the Worker

```bash
npm install
npx wrangler deploy
```

Then add `mirror.polycast5.com` as a custom domain for the Worker and uncomment the
`routes` line in `wrangler.toml`. The DNS record has to live in the Cloudflare zone for
`polycast5.com`; the Shopify storefront cannot host this, since it serves pages only.

Requirements:

- **Wrangler 4.36 or later**, for the `[[ratelimits]]` binding. `package.json` pins it.
- **SQLite-backed Durable Objects** (`new_sqlite_classes`), the only backend the Workers
  Free plan offers. A class's backend cannot be changed after its first deploy.
- The rate limiter's `namespace_id` must not be used by another limiter in the account.
- **Plan.** Free (100,000 requests and 13,000 GB-s of Durable Object duration a day)
  covers bench use. A streaming session keeps its object awake, so it is billed for
  duration the whole time, and every WebSocket message it receives counts as 1/20 of a
  request. A public deployment wants Workers Paid.

### Why Cloudflare Workers

- A Durable Object **is** a session — one object per code holding two sockets. No session
  table, no KV, nothing to expire by hand.
- Anycast edge. A device in Berlin and a viewer in Sydney each terminate nearby instead of
  sharing one round trip to a single VPS, and round-trip time is what caps the frame rate.
- No egress billing, which matters for something shaped like video.
- Nothing to patch or monitor.

`fly.io` running `dev-relay.js` behind TLS is a reasonable alternative if plain Node is
preferred; it loses the geography and you own the process lifecycle.

## Protocol

Device connects to `/d`; the relay allocates a code and replies:

```json
{"t":"ready","code":"K7M2Q9","ttl":600}
{"t":"viewer","state":"attached","check":"AB7"}
{"t":"viewer","state":"detached"}
{"t":"locked","check":"AB7"}
{"t":"bye","reason":"timeout"}
```

Viewer connects to `/v?code=K7M2Q9` and gets:

```json
{"t":"paired","w":240,"h":135,"resume":"<uuid>","check":"AB7"}
```

When the user approves the viewer on the device, the device tells the relay which one:

```json
{"t":"approved","check":"AB7"}
```

Until then every attach is a new viewer with a fresh resume token and 3-character check
code (never the check just replaced), and any `resume` offered is ignored, so a viewer
that leaves unapproved holds nothing. `approved` locks the session if a viewer is attached
and its check matches, and the relay confirms with `locked`; otherwise it is ignored. Text
from the viewer is always ignored. Once locked, only the approved viewer's token attaches
(`/v?code=K7M2Q9&resume=<uuid>`), getting the same token and check back, until the
device's connection ends.

The viewer keeps the latest token in `localStorage` under `pc5mirror.resume.<CODE>`
(overwritten on every `paired`) and the tab's last code in `sessionStorage` under
`pc5mirror.lastcode`. On load it fills the code back in and, holding a token for it,
attaches once, so a refresh returns to the session; a new tab or a later visit needs the
code typed. A refused auto-attach forgets the code and is not retried. The storefront's
Disconnect ends the session on the device and forgets the token.

| Refusal | Status |
|---|---|
| Malformed code | 400 |
| No device on that code, viewer slot taken, or (once locked) missing / wrong resume token | 404 `no such code`, identical for all three |
| Too many attempts from this IP | 429 |

A browser does not expose the status of a failed WebSocket handshake, so the page only
sees the connection fail.

Everything else is binary and passes through verbatim. The frame and input formats are
defined in `components/mirror/include/mirror_proto.h`, including the STATUS message the
device sends after every attach: pending approval, or live.

### Approval on the device

An attached viewer gets nothing until the owner approves it. The device shows the check
code and waits for **SELECT**; the viewer page shows the same code, so the owner can see it
is their own browser before pressing. **RIGHT** declines it instead, which ends the session
and tells the viewer why. SELECT approves only the check on screen; the device sends
`approved` and streams nothing, not even STATUS LIVE, until `locked` comes back. Approval
lasts until the device's relay connection drops, which also retires the code, so a
refreshed tab carrying the token goes straight back to live. A tab refreshed before
approval is a new viewer and needs approving again. Any other check voids the approval on
the device, since the relay cannot have locked to it. While a locked session has no
viewer, the device shows that only the approved browser can rejoin; stopping and starting
again gets a fresh code.

Codes use Crockford base32 minus the ambiguous letters, and the viewer endpoint folds
lowercase, dashes, `O`→`0` and `I`/`L`→`1`, so a code read off a 135-pixel-tall screen and
typed on a phone still lands.

## Limits

| Limit | Value | Why |
|---|---|---|
| Viewers per session | 1 | Any browser may take a free slot until the device approves one; then only it can come back |
| Device sockets per code | 1 | A duplicate device gets 409 and retries with a fresh code |
| Unclaimed code TTL | 10 min | A code left on screen dies on its own |
| Session cap | 60 min from device connect | Bounds an abandoned session; re-attaching does not extend it |
| Pairing attempts | 10 / min per IP | The `VIEW_LIMITER` binding, checked before any Durable Object wakes. IPv6 counts per /64 |

32⁶ is about 1.07 × 10⁹, so a blind guess against ~50 live codes lands roughly once in 21
million attempts, and the rate limit slows guessing further. It is counted per Cloudflare
location and eventually consistent, so treat it as a brake, not a wall. The real control
is approval: a guessed code that beats the owner's own browser to the relay still sees
nothing until someone at the device presses SELECT, and the owner's browser is refused
while it holds the slot, which is hard to miss. The check code tells the owner which
browser they are approving; after approval the resume token keeps others out.

The lock's trip to the relay is covered by `locked`. If the approved viewer leaves before
`approved` lands and another browser takes the slot, the relay refuses the lock and the
device, never hearing `locked`, sends that browser nothing but STATUS PENDING.

## What the relay operator can see

Everything: the screen, and every character typed into a text box. The transport is TLS so
nobody else can, but this is not end to end. The firmware blanks nothing: PIN, password,
key and saved-login screens are streamed and remotely typeable like every other screen, so
the summary is "don't mirror something you would not show the person running the relay."

If end-to-end is wanted later, the right shape is a key in the URL **fragment** (`#k=...`)
carried by the QR code on the device, since a fragment is never sent to the server. Do not
derive a key from the pairing code: 30 bits is not a key.
