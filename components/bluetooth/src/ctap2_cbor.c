#include <string.h>

#include "ctap2_cbor.h"

/* ===================== Writer ===================== */

void cbor_w_init(cbor_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

bool cbor_w_ok(const cbor_writer_t *w)
{
    return !w->overflow;
}

static void w_put(cbor_writer_t *w, const uint8_t *b, size_t n)
{
    if (w->overflow) {
        return;
    }
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, b, n);
    w->len += n;
}

/**
 * Write an initial byte plus its argument in the shortest form that fits, which
 * is what CTAP2 canonical CBOR requires.
 */
static void w_head(cbor_writer_t *w, uint8_t major, uint64_t val)
{
    uint8_t hdr[9];
    uint8_t mt = (uint8_t)(major << 5);

    if (val < 24) {
        hdr[0] = (uint8_t)(mt | val);
        w_put(w, hdr, 1);
    } else if (val <= 0xFF) {
        hdr[0] = (uint8_t)(mt | 24);
        hdr[1] = (uint8_t)val;
        w_put(w, hdr, 2);
    } else if (val <= 0xFFFF) {
        hdr[0] = (uint8_t)(mt | 25);
        hdr[1] = (uint8_t)(val >> 8);
        hdr[2] = (uint8_t)val;
        w_put(w, hdr, 3);
    } else if (val <= 0xFFFFFFFFu) {
        hdr[0] = (uint8_t)(mt | 26);
        hdr[1] = (uint8_t)(val >> 24);
        hdr[2] = (uint8_t)(val >> 16);
        hdr[3] = (uint8_t)(val >> 8);
        hdr[4] = (uint8_t)val;
        w_put(w, hdr, 5);
    } else {
        hdr[0] = (uint8_t)(mt | 27);
        for (int i = 0; i < 8; i++) {
            hdr[1 + i] = (uint8_t)(val >> (56 - 8 * i));
        }
        w_put(w, hdr, 9);
    }
}

void cbor_w_uint(cbor_writer_t *w, uint64_t v)
{
    w_head(w, CBOR_MT_UINT, v);
}

void cbor_w_int(cbor_writer_t *w, int64_t v)
{
    if (v < 0) {
        // Negative integers encode -1 - n, so -7 becomes an argument of 6
        w_head(w, CBOR_MT_NINT, (uint64_t)(-(v + 1)));
    } else {
        w_head(w, CBOR_MT_UINT, (uint64_t)v);
    }
}

void cbor_w_bytes(cbor_writer_t *w, const uint8_t *b, size_t n)
{
    w_head(w, CBOR_MT_BYTES, n);
    w_put(w, b, n);
}

void cbor_w_text(cbor_writer_t *w, const char *s)
{
    size_t n = strlen(s);
    w_head(w, CBOR_MT_TEXT, n);
    w_put(w, (const uint8_t *)s, n);
}

void cbor_w_bool(cbor_writer_t *w, bool v)
{
    // Simple values: 20 is false, 21 is true
    uint8_t b = (uint8_t)((CBOR_MT_SIMPLE << 5) | (v ? 21 : 20));
    w_put(w, &b, 1);
}

void cbor_w_array(cbor_writer_t *w, size_t n)
{
    w_head(w, CBOR_MT_ARRAY, n);
}

void cbor_w_map(cbor_writer_t *w, size_t n)
{
    w_head(w, CBOR_MT_MAP, n);
}

void cbor_w_raw(cbor_writer_t *w, const uint8_t *b, size_t n)
{
    w_put(w, b, n);
}

/* ===================== Reader ===================== */

void cbor_r_init(cbor_reader_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

bool cbor_r_done(const cbor_reader_t *r)
{
    return r->pos >= r->len;
}

int cbor_r_peek(const cbor_reader_t *r, uint8_t *major)
{
    if (r->pos >= r->len) {
        return CBOR_ERR_EOF;
    }
    *major = (uint8_t)(r->buf[r->pos] >> 5);
    return CBOR_OK;
}

/**
 * Read one initial byte and its argument. Rejects indefinite lengths, which
 * CTAP2 forbids, and the reserved additional-information values.
 */
static int r_head(cbor_reader_t *r, uint8_t *major, uint64_t *val)
{
    if (r->pos >= r->len) {
        return CBOR_ERR_EOF;
    }

    uint8_t ib = r->buf[r->pos++];
    *major = (uint8_t)(ib >> 5);
    uint8_t ai = (uint8_t)(ib & 0x1F);

    if (ai < 24) {
        *val = ai;
        return CBOR_OK;
    }

    size_t nbytes;
    switch (ai) {
    case 24: nbytes = 1; break;
    case 25: nbytes = 2; break;
    case 26: nbytes = 4; break;
    case 27: nbytes = 8; break;
    default: return CBOR_ERR_BAD; // 28-30 reserved, 31 indefinite
    }

    if (r->pos + nbytes > r->len) {
        return CBOR_ERR_EOF;
    }

    uint64_t v = 0;
    for (size_t i = 0; i < nbytes; i++) {
        v = (v << 8) | r->buf[r->pos++];
    }
    *val = v;
    return CBOR_OK;
}

int cbor_r_uint(cbor_reader_t *r, uint64_t *v)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;

    int rc = r_head(r, &major, &val);
    if (rc != CBOR_OK) {
        r->pos = save;
        return rc;
    }
    if (major != CBOR_MT_UINT) {
        r->pos = save;
        return CBOR_ERR_TYPE;
    }

    *v = val;
    return CBOR_OK;
}

int cbor_r_int(cbor_reader_t *r, int64_t *v)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;

    int rc = r_head(r, &major, &val);
    if (rc != CBOR_OK) {
        r->pos = save;
        return rc;
    }

    if (major == CBOR_MT_UINT) {
        if (val > (uint64_t)INT64_MAX) {
            r->pos = save;
            return CBOR_ERR_BAD;
        }
        *v = (int64_t)val;
        return CBOR_OK;
    }
    if (major == CBOR_MT_NINT) {
        if (val > (uint64_t)INT64_MAX) {
            r->pos = save;
            return CBOR_ERR_BAD;
        }
        *v = -1 - (int64_t)val;
        return CBOR_OK;
    }

    r->pos = save;
    return CBOR_ERR_TYPE;
}

int cbor_r_bool(cbor_reader_t *r, bool *v)
{
    if (r->pos >= r->len) {
        return CBOR_ERR_EOF;
    }

    uint8_t ib = r->buf[r->pos];
    if (ib == ((CBOR_MT_SIMPLE << 5) | 20)) {
        *v = false;
    } else if (ib == ((CBOR_MT_SIMPLE << 5) | 21)) {
        *v = true;
    } else {
        return CBOR_ERR_TYPE;
    }

    r->pos++;
    return CBOR_OK;
}

/**
 * Shared body for byte and text strings: both carry a length then that many
 * bytes, and both are borrowed from the reader buffer rather than copied.
 */
static int r_str(cbor_reader_t *r, uint8_t want, const uint8_t **p, size_t *n)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;

    int rc = r_head(r, &major, &val);
    if (rc != CBOR_OK) {
        r->pos = save;
        return rc;
    }
    if (major != want) {
        r->pos = save;
        return CBOR_ERR_TYPE;
    }
    if (val > r->len - r->pos) {
        r->pos = save;
        return CBOR_ERR_EOF;
    }

    *p = r->buf + r->pos;
    *n = (size_t)val;
    r->pos += (size_t)val;
    return CBOR_OK;
}

int cbor_r_bytes(cbor_reader_t *r, const uint8_t **p, size_t *n)
{
    return r_str(r, CBOR_MT_BYTES, p, n);
}

int cbor_r_text(cbor_reader_t *r, const char **p, size_t *n)
{
    return r_str(r, CBOR_MT_TEXT, (const uint8_t **)p, n);
}

static int r_container(cbor_reader_t *r, uint8_t want, size_t *n)
{
    size_t save = r->pos;
    uint8_t major;
    uint64_t val;

    int rc = r_head(r, &major, &val);
    if (rc != CBOR_OK) {
        r->pos = save;
        return rc;
    }
    if (major != want) {
        r->pos = save;
        return CBOR_ERR_TYPE;
    }

    /* A container cannot hold more items than there are bytes left to hold
     * them. A map needs two items per pair, bounded before doubling so a count
     * near 2^63 cannot wrap past the check. */
    uint64_t remaining = (uint64_t)(r->len - r->pos);
    if (val > remaining || (want == CBOR_MT_MAP && val * 2 > remaining)) {
        r->pos = save;
        return CBOR_ERR_BAD;
    }

    *n = (size_t)val;
    return CBOR_OK;
}

int cbor_r_array(cbor_reader_t *r, size_t *n)
{
    return r_container(r, CBOR_MT_ARRAY, n);
}

int cbor_r_map(cbor_reader_t *r, size_t *n)
{
    return r_container(r, CBOR_MT_MAP, n);
}

int cbor_r_skip(cbor_reader_t *r)
{
    /* Iterative rather than recursive: a hostile request could otherwise nest
     * containers deeply enough to run the task stack out. The counter is the
     * number of items still owed at the current depth, flattened. */
    size_t owed = 1;

    while (owed > 0) {
        uint8_t major;
        uint64_t val;

        int rc = r_head(r, &major, &val);
        if (rc != CBOR_OK) {
            return rc;
        }

        owed--;

        switch (major) {
        case CBOR_MT_UINT:
        case CBOR_MT_NINT:
        case CBOR_MT_SIMPLE:
            break;

        case CBOR_MT_BYTES:
        case CBOR_MT_TEXT:
            if (val > r->len - r->pos) {
                return CBOR_ERR_EOF;
            }
            r->pos += (size_t)val;
            break;

        case CBOR_MT_ARRAY:
        case CBOR_MT_MAP: {
            /* Bound the declared count before doubling it for a map. Doubling
             * first lets a count near 2^63 wrap to something small, which would
             * pass the check and skip the container as if it were empty. */
            uint64_t remaining = (uint64_t)(r->len - r->pos);
            if (val > remaining) {
                return CBOR_ERR_BAD; // More items than bytes remaining
            }

            uint64_t items = (major == CBOR_MT_MAP) ? val * 2 : val;
            if (items > remaining) {
                return CBOR_ERR_BAD;
            }

            owed += (size_t)items;
            break;
        }

        default:
            return CBOR_ERR_BAD; // Tags are not used anywhere in CTAP2
        }
    }

    return CBOR_OK;
}
