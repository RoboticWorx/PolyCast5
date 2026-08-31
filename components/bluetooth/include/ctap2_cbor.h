#ifndef CTAP2_CBOR_H
#define CTAP2_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The slice of RFC 8949 CBOR that CTAP2 actually uses.
 *
 * Hand-rolled rather than pulled in as a dependency: CTAP2 needs definite
 * lengths, no tags, no floats and no indefinite strings, which is a small
 * enough subset that a full library would be mostly dead code. The writer emits
 * CTAP2 canonical form, which means shortest-length encoding and map keys in
 * ascending order - the caller is responsible for supplying keys in order. */

/* CBOR major types, in the top three bits of the initial byte */
#define CBOR_MT_UINT  0
#define CBOR_MT_NINT  1
#define CBOR_MT_BYTES 2
#define CBOR_MT_TEXT  3
#define CBOR_MT_ARRAY 4
#define CBOR_MT_MAP   5
#define CBOR_MT_SIMPLE 7

/* ===================== Writer ===================== */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow; // Sticky: once set, every later write is a no-op
} cbor_writer_t;

void cbor_w_init(cbor_writer_t *w, uint8_t *buf, size_t cap);

/**
 * @brief Whether every write so far fit in the buffer
 */
bool cbor_w_ok(const cbor_writer_t *w);

void cbor_w_uint(cbor_writer_t *w, uint64_t v);

/**
 * @brief Write a signed integer, choosing the unsigned or negative major type
 */
void cbor_w_int(cbor_writer_t *w, int64_t v);

void cbor_w_bytes(cbor_writer_t *w, const uint8_t *b, size_t n);
void cbor_w_text(cbor_writer_t *w, const char *s);
void cbor_w_bool(cbor_writer_t *w, bool v);

/**
 * @brief Open an array of exactly n items; the caller writes them next
 */
void cbor_w_array(cbor_writer_t *w, size_t n);

/**
 * @brief Open a map of exactly n pairs; the caller writes key/value in order
 */
void cbor_w_map(cbor_writer_t *w, size_t n);

/**
 * @brief Splice in already-encoded CBOR, used to nest a prepared structure
 */
void cbor_w_raw(cbor_writer_t *w, const uint8_t *b, size_t n);

/* ===================== Reader ===================== */

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} cbor_reader_t;

#define CBOR_OK       0
#define CBOR_ERR_EOF  (-1) // Ran off the end of the buffer
#define CBOR_ERR_TYPE (-2) // Item is not the type the caller asked for
#define CBOR_ERR_BAD  (-3) // Malformed, or a construct CTAP2 does not allow

void cbor_r_init(cbor_reader_t *r, const uint8_t *buf, size_t len);

/**
 * @brief Major type of the next item without consuming it
 */
int cbor_r_peek(const cbor_reader_t *r, uint8_t *major);

int cbor_r_uint(cbor_reader_t *r, uint64_t *v);
int cbor_r_int(cbor_reader_t *r, int64_t *v);
int cbor_r_bool(cbor_reader_t *r, bool *v);

/**
 * @brief Borrow a byte string; the pointer aliases the reader buffer
 */
int cbor_r_bytes(cbor_reader_t *r, const uint8_t **p, size_t *n);

/**
 * @brief Borrow a text string; NOT NUL-terminated, use the returned length
 */
int cbor_r_text(cbor_reader_t *r, const char **p, size_t *n);

int cbor_r_array(cbor_reader_t *r, size_t *n);
int cbor_r_map(cbor_reader_t *r, size_t *n);

/**
 * @brief Consume the next item whatever it is, recursing through containers
 *
 * Used to step over map entries this build does not implement.
 */
int cbor_r_skip(cbor_reader_t *r);

/**
 * @brief Whether every byte has been consumed
 */
bool cbor_r_done(const cbor_reader_t *r);

#endif // CTAP2_CBOR_H
