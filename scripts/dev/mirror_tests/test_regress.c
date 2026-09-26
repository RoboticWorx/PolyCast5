/* Regression tests for the Screen Mirror encoder's reference-buffer handling.
 *
 * These cover the cases where the encoder must resend a tile it has already "sent":
 * a new viewer needing a full repaint, and a send that failed on the wire. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mirror.h"
#include "mirror_priv.h"
#include "mirror_proto.h"

static uint16_t truth[MIRROR_SCR_W * MIRROR_SCR_H];
static uint8_t msgbuf[MIRROR_MSG_MAX_BYTES];
static int failures = 0;

static void ok(const char *what) { printf("  ok  %s\n", what); }
static void bad(const char *what) { printf("  !! %s\n", what); failures++; }

static void push_screen(const uint16_t *s)
{
    for (int y = 0; y < MIRROR_SCR_H; y += 20) {
        const int y2 = (y + 19 < MIRROR_SCR_H) ? y + 19 : MIRROR_SCR_H - 1;
        mirror_capture(0, (int16_t)y, MIRROR_SCR_W - 1, (int16_t)y2, &s[(size_t)y * MIRROR_SCR_W]);
    }
}

static void fill(uint16_t *s, int phase)
{
    for (int y = 0; y < MIRROR_SCR_H; y++)
        for (int x = 0; x < MIRROR_SCR_W; x++)
            s[y * MIRROR_SCR_W + x] = (uint16_t)(((x / 8 + y / 8 + phase) & 1) ? 0xFFFF : 0x18E3);
}

/* Encode a frame and return how many tiles and bytes it produced. */
static uint16_t run_frame(mirror_quality_t q, size_t *out_bytes)
{
    const uint16_t tiles = mirror_encode_frame(q);
    size_t bytes = 0;

    for (uint16_t i = 0; i < mirror_encode_msg_count(); i++) {
        bytes += mirror_encode_build_msg(i, 1, false, q, msgbuf, sizeof(msgbuf));
    }

    if (out_bytes) *out_bytes = bytes;
    return tiles;
}

int main(void)
{
    mirror_state = MIRROR_LIVE;

    /* ---- A new viewer must get a full repaint ---- */
    printf("=== keyframe after a steady screen ===\n");

    fill(truth, 0);
    push_screen(truth);
    mirror_force_keyframe();

    uint16_t t0 = run_frame(MIRROR_Q_EXACT, NULL);
    printf("      first frame: %u tiles\n", t0);
    if (t0 != MIRROR_TILE_CNT) bad("first frame was not the whole screen");
    else ok("first frame covers every tile");

    /* Screen unchanged; a repaint alone must cost nothing */
    push_screen(truth);
    if (run_frame(MIRROR_Q_EXACT, NULL) != 0) bad("an identical repaint still emitted tiles");
    else ok("identical repaint emits nothing");

    /* Now a viewer attaches. The device asks for a keyframe: it MUST resend everything,
       because the new browser's canvas is blank. */
    mirror_force_keyframe();
    uint16_t t1 = run_frame(MIRROR_Q_EXACT, NULL);
    printf("      keyframe after steady state: %u tiles\n", t1);
    if (t1 != MIRROR_TILE_CNT) bad("keyframe did NOT resend the screen - a new viewer would see blank");
    else ok("keyframe resends every tile");

    /* ---- A quality change must resend, since the reference holds the old quantization ---- */
    printf("\n=== quality change ===\n");
    mirror_force_keyframe();
    uint16_t t2 = run_frame(MIRROR_Q_444, NULL);
    if (t2 != MIRROR_TILE_CNT) bad("quality change did not resend every tile");
    else ok("quality change resends every tile");

    /* ---- A failed send must not lose tiles ---- */
    printf("\n=== send failure recovery ===\n");

    fill(truth, 1);
    push_screen(truth);

    uint16_t t3 = mirror_encode_frame(MIRROR_Q_EXACT);
    uint16_t msgs = mirror_encode_msg_count();
    printf("      frame to send: %u tiles in %u messages\n", t3, msgs);
    if (msgs < 2) { bad("need a multi-message frame for this test"); goto end; }

    /* Message 0 goes out; everything from message 1 on fails, as it would when the
       socket is full. Those tiles must come back on the next frame. */
    for (uint16_t i = 1; i < msgs; i++) mirror_encode_restore_msg(i);

    uint16_t recovered = mirror_encode_frame(MIRROR_Q_EXACT);
    uint16_t expected = (uint16_t)(t3 - (t3 < MIRROR_TILES_PER_MSG ? t3 : MIRROR_TILES_PER_MSG));
    printf("      after restoring messages 1..%u: %u tiles re-offered (want %u)\n",
           msgs - 1, recovered, expected);

    if (recovered != expected) bad("restored tiles were NOT re-sent - the viewer keeps stale pixels");
    else ok("restored tiles are re-sent");

end:
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
