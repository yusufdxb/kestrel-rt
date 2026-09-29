/* Latest-value mailbox (single writer, any number of readers) using a sequence lock.
 *
 * The writer never blocks and never fails: a new value always replaces the old one (this is the
 * declared policy for state-like channels such as "latest sensor estimate"). Readers get either a
 * consistent snapshot or KMB_BUSY after a bounded number of retries (bounded WCET for readers).
 * Portable; host-tested.
 */
#ifndef KESTREL_MAILBOX_H
#define KESTREL_MAILBOX_H

#include <stdatomic.h>
#include <stdint.h>

#define KMB_MAX_RETRIES 4u

typedef enum { KMB_OK = 0, KMB_EMPTY = 1, KMB_BUSY = 2 } kmb_status_t;

typedef struct {
    _Atomic uint32_t seq; /* odd while a write is in progress; 0 = never written */
    uint32_t size;
    volatile uint8_t *data;
    _Atomic uint32_t read_retries;
    _Atomic uint32_t read_busy;
} kmailbox_t;

#define KMAILBOX_DEFINE(name, type)                                                                \
    static type name##_data;                                                                       \
    static kmailbox_t name = {0, sizeof(type), (volatile uint8_t *)&name##_data, 0, 0}

static inline void kmb_write(kmailbox_t *mb, const void *value)
{
    const uint8_t *src = value;
    uint32_t s = atomic_load_explicit(&mb->seq, memory_order_relaxed);
    atomic_store_explicit(&mb->seq, s + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (uint32_t i = 0; i < mb->size; i++) {
        mb->data[i] = src[i];
    }
    atomic_store_explicit(&mb->seq, s + 2u, memory_order_release);
}

/* Returns the sequence number of the snapshot in *seq_out (even, increases by 2 per write). */
static inline kmb_status_t kmb_read(kmailbox_t *mb, void *out, uint32_t *seq_out)
{
    uint8_t *dst = out;
    for (uint32_t attempt = 0; attempt < KMB_MAX_RETRIES; attempt++) {
        uint32_t s1 = atomic_load_explicit(&mb->seq, memory_order_acquire);
        if (s1 == 0u) {
            return KMB_EMPTY;
        }
        if ((s1 & 1u) == 0u) {
            for (uint32_t i = 0; i < mb->size; i++) {
                dst[i] = mb->data[i];
            }
            atomic_thread_fence(memory_order_acquire);
            uint32_t s2 = atomic_load_explicit(&mb->seq, memory_order_relaxed);
            if (s1 == s2) {
                if (seq_out != 0) {
                    *seq_out = s1;
                }
                return KMB_OK;
            }
        }
        atomic_fetch_add_explicit(&mb->read_retries, 1u, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&mb->read_busy, 1u, memory_order_relaxed);
    return KMB_BUSY;
}

#endif
