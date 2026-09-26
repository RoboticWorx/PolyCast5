# Screen Mirror host tests

Runs the real Screen Mirror encoder, the real browser decoder and the real relay on a PC.
No hardware, no flashing, no cloud. `components/mirror/src/mirror_capture.c`,
`mirror_encode.c`, `mirror_thermal.c`, the quality controller `mirror_quality.c`, the
thermal page's `lcd_ir_exp_render.c` and the button scheduler `gpio_remote.c` are compiled
against the project's real headers plus stand-ins for the ESP-IDF ones in `stub/`, which is
the same trick `ai_freq.c` uses.

```bash
./run.sh
```

Needs `gcc` (MSYS2 UCRT64 is fine) and `node`. The relay suite additionally needs the
`ws` package and is skipped automatically without it:

```bash
npm install ws
export NODE_PATH=$PWD/node_modules
```

## What each suite covers

| Suite | Covers |
|---|---|
| `test_encode.c` | Round-trip pixel-exactness at every quality level, all five tile encodings, half scale, capture-time redaction (blanks on entry, blanks a capture made while redacted, returns real pixels on lift, and leaves no secret in the shadow), and the compression ratio on menu / thermal / DOOM / noise content |
| `test_regress.c` | The reference-buffer rules: a keyframe must resend a screen that has not changed (or a newly attached viewer sees a blank canvas), a quality change must resend, and tiles from a failed send must come back |
| `test_decode.js` | The **actual** `scripts/relay/decode.js` the storefront ships, fed real encoder output, compared pixel for pixel |
| `test_thermal.c` + `test_thermal.js` | The thermal channel. The page's integer tap builder is bit-identical to the float one it replaced; the browser's port of the render math matches `lcd_ir_exp_render.c` pixel for pixel on the page's own configuration and on random sizes, flips, spans and palettes; then a scripted session (label up and down, latest-wins, panel edits in the straddling tile column, palette change, freeze, tab reload, every quality level, detail view, redaction, leaving the page with no flush) is replayed through the real `canvasSink`, and the viewer must show exactly the panel's pixels at every checkpoint. Also measures the tile path against the channel on a noisy canvas |
| `test_remote_buttons.c` | The web-button click scheduler in `gpio_remote.c`: bunched clicks never merge, bunched clicks on different pins keep their order, taps queue behind a hold while an open hold never delays another pin, gaps between presses, the queue cap, `clear()`, late polls, the real 10 ms tick clock with events landing just after a poll, and clock wrap |
| `test_quality.c` | The quality controller in `mirror_quality.c` and its ack round-trip ring. The ring: pairing a stamp with its ack, the 2^32 us wrap, acks with no stamp, older than the ring, handled before their stamp, repeated, or passing stamps whose own ack never comes, and the pending age. The controller, window by window: the per-window statistics (lower median once the two slowest acks are dropped, since one stall delays only the frames in flight; the second fastest ack for the baseline, since preemption only shortens samples), the keyframe window skipped and the hold after every change, idle windows climbing without arming a probe and load snapping back to the level it held in one change, a window with an ack still owed counting for nothing, neutral windows resetting the clear streak, a blocked send, an ack timeout and overdue acks as congestion, a run of silent windows as one strike, the byte cap, the viewer's pin, failed probes doubling the wait to 60 windows, a probe that outlives 15 windows letting the next level climb at once and a held one resetting the wait, a resume keeping the baseline and backoff and a new viewer dropping both, the baseline standing through a still minute and a standing queue and ignoring a 0 ms ack whether among nine or alone, the baseline cap, a route change, and single spikes. Then the real controller on a simulated link in 1 ms steps (TCP send buffer that blocks when full, relay queue, viewer acks, pacing gate, ack timeout, periodic keyframe): a still screen climbs to level 0 and stays; busy content on a fast link with 30 ms CPU-bound sends stays at level 0 where the old send-time controller drops to 3; uplink- and viewer-limited links step down and settle, probes backing off; a level whose own frames keep a queue standing never climbs into more; a 40 s still screen before busy content still settles at level 3; 10 KB/s ends at level 3; 20 s busy / 20 s still content climbs back to level 0 on every still screen and returns to level 3 in one change; sparse presses on a 250 ms path climb after one backoff wait; stamps made 200 ms late by preemption, Wi-Fi stalls up to 1.5 s and a 400 ms path change nothing; a 700 ms path started at level 2 climbs to level 0; a link that clears after five limited minutes is back at level 0 within one backoff wait |
| `syntax_check.py` | Compiles every changed firmware source with the **real ESP-IDF cross-compiler** using its own command line out of `build/compile_commands.json`, `-fsyntax-only` so nothing is written. A source newer than the configure borrows a neighbour's command line from the same component, and is listed as such. This is the check that caught a missing `esp_attr.h` include which the host stubs had masked |
| `test_relay.js` | The **actual** `scripts/relay/dev-relay.js`, with a simulated device replaying real frames: pairing, code normalisation, subprotocol echo, 400/404/429 refusals, one-viewer-at-a-time, the resume token and check code, a fresh pair per attach until approval (an unapproved viewer that leaves holds nothing, a stale token is ignored), the lock on a device `approved` (only with a matching check and an attached viewer, never from the viewer) and its `locked` confirmation to the device, token-only rejoin once locked, racing rejoins, the rate limit, the input return path, ack pacing, and viewer drop on device loss |

## A note on the stubs

`stub/` stands in for **ESP-IDF** headers only (`esp_attr.h`, `esp_err.h`, `freertos/`),
because the real ones need the target toolchain. Project headers such as
`polycast5_macros.h` are used straight from `components/`, never copied. An earlier copy
defined `POLYCAST5_USE_PSRAM_BSS` as empty, which let a source compile on the host while
failing on target because it never included `esp_attr.h`. The stubs that remain follow the
real include chain, and `syntax_check.py` is the backstop for anything they get wrong.

The feature ships switched off in `polycast5_macros.h`, so `run.sh` passes
`-DPOLYCAST5_EN_SCREEN_MIRROR=1` to compile the code under test.

## What they do NOT cover

Anything that needs the chip: the cost the capture hook adds to `lcd_task`, PSRAM
bandwidth contention, TLS memory headroom, Wi-Fi/BLE coexistence, battery drain, and the
real I2C and 20 ms poll timing under the button scheduler. Validate those by flashing,
with `POLYCAST5_DEBUG_GPIO` on to trace injected presses through the real button state
machine.

For the thermal channel: the session test composes the screen itself, so it trusts that
LVGL blits the canvas unchanged and that `lcd_ir_exp.c` finds every object drawn over it.
It also cannot tear the seqlock, which needs `lcd_task` preempting `mirror_task` mid-copy.

For the quality controller: the link, the content sizes and the preemption are a model. Real
thresholds want a bench log: `Quality down/up` lines carry the round trip, baseline, queue,
throughput and send time behind every change, and `MIRROR` at DEBUG logs every window.
