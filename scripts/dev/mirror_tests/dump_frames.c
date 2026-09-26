/* Emit real encoder output plus the canvas it should produce, so the browser decoder can
 * be checked against the firmware rather than against a second copy of my assumptions. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "mirror.h"
#include "mirror_priv.h"
#include "mirror_proto.h"

static uint16_t truth[MIRROR_SCR_W * MIRROR_SCR_H];
static uint8_t msgbuf[MIRROR_MSG_MAX_BYTES];
static FILE *fmsgs;

static uint32_t rng = 987654321u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void push_screen(const uint16_t *s)
{
    for (int y = 0; y < MIRROR_SCR_H; y += 20) {
        const int y2 = (y + 19 < MIRROR_SCR_H) ? y + 19 : MIRROR_SCR_H - 1;
        mirror_capture(0, (int16_t)y, MIRROR_SCR_W - 1, (int16_t)y2, &s[(size_t)y * MIRROR_SCR_W]);
    }
}

static void emit(mirror_quality_t q, int keyframe)
{
    if (mirror_encode_frame(q) == 0) return;

    const uint16_t msgs = mirror_encode_msg_count();

    for (uint16_t i = 0; i < msgs; i++) {
        const size_t n = mirror_encode_build_msg(i, 7, keyframe, q, msgbuf, sizeof(msgbuf));
        if (!n) continue;

        const uint32_t len = (uint32_t)n;
        fwrite(&len, 4, 1, fmsgs);
        fwrite(msgbuf, 1, n, fmsgs);
    }
}

/* Mixed content: flat bands, sharp text-like edges, a smooth gradient and pure noise, so
 * every tile mode gets exercised in one pass. */
static void gen(uint16_t *s, int phase)
{
    for (int y = 0; y < MIRROR_SCR_H; y++) {
        for (int x = 0; x < MIRROR_SCR_W; x++) {
            uint16_t c;

            if (y < 30) {
                c = 0x001F; /* solid */
            } else if (y < 60) {
                c = ((x / 4 + phase) % 3) ? 0xFFFF : 0x0000; /* few colours */
            } else if (y < 100) {
                const int v = (x * 255 / MIRROR_SCR_W + phase) & 0xFF;
                c = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3)); /* gradient */
            } else {
                c = (uint16_t)rnd(); /* noise */
            }

            s[y * MIRROR_SCR_W + x] = c;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: dump_frames <frames.bin> <expect.raw>\n"); return 2; }

    mirror_state = MIRROR_LIVE;
    fmsgs = fopen(argv[1], "wb");
    if (!fmsgs) { perror("open frames"); return 1; }

    /* One keyframe at each quality level, then an incremental frame, so the stream covers
     * both full repaints and diffs, and half scale as well as full. */
    for (int q = 0; q < MIRROR_Q_COUNT; q++) {
        mirror_force_keyframe();
        gen(truth, q);
        push_screen(truth);
        emit((mirror_quality_t)q, 1);

        gen(truth, q + 1);
        push_screen(truth);
        emit((mirror_quality_t)q, 0);
    }

    /* Finish at full quality with a known screen so the canvas can be compared exactly. */
    mirror_force_keyframe();
    gen(truth, 3);
    push_screen(truth);
    emit(MIRROR_Q_EXACT, 1);

    fclose(fmsgs);

    FILE *fe = fopen(argv[2], "wb");
    if (!fe) { perror("open expect"); return 1; }
    fwrite(truth, sizeof(uint16_t), MIRROR_SCR_W * MIRROR_SCR_H, fe);
    fclose(fe);

    printf("wrote %s and %s\n", argv[1], argv[2]);
    return 0;
}
