/* Lock-free single-producer / single-consumer queue with fixed-size elements.
 *
 * Portable (host-tested under ASan/UBSan/TSan). Storage is supplied statically by the caller.
 * head is written only by the producer, tail only by the consumer; indices run freely and wrap
 * modulo 2^32, capacity is a power of two, so (head - tail) is always the fill level.
 *
 * Full policy is REJECT: a push into a full queue fails, is counted in `rejected`, and the caller
 * decides (usually trace + continue). Latest-value semantics use kestrel mailboxes instead, because
 * overwrite-oldest cannot be done by the producer alone without breaking single-writer ownership of
 * `tail`.
 */
#ifndef KESTREL_SPSC_H
#define KESTREL_SPSC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    _Atomic uint32_t head; /* producer-owned */
    _Atomic uint32_t tail; /* consumer-owned */
    uint32_t mask;         /* capacity - 1 */
    uint32_t elem_size;
    uint8_t *storage; /* capacity * elem_size bytes */
    _Atomic uint32_t rejected;
    _Atomic uint32_t high_water;
} kq_spsc_t;

#define KQ_SPSC_DEFINE(name, type, capacity)                                                       \
    _Static_assert(((capacity) & ((capacity)-1u)) == 0u, "capacity must be a power of two");       \
    static type name##_storage[(capacity)];                                                        \
    static kq_spsc_t name = {0, 0, (capacity)-1u, sizeof(type), (uint8_t *)name##_storage, 0, 0}

static inline uint32_t kq_spsc_count(kq_spsc_t *q)
{
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
    return h - t;
}

static inline bool kq_spsc_push(kq_spsc_t *q, const void *elem)
{
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if ((h - t) > q->mask) {
        atomic_fetch_add_explicit(&q->rejected, 1u, memory_order_relaxed);
        return false;
    }
    memcpy(&q->storage[(h & q->mask) * q->elem_size], elem, q->elem_size);
    atomic_store_explicit(&q->head, h + 1u, memory_order_release);
    uint32_t fill = h + 1u - t;
    if (fill > atomic_load_explicit(&q->high_water, memory_order_relaxed)) {
        atomic_store_explicit(&q->high_water, fill, memory_order_relaxed);
    }
    return true;
}

static inline bool kq_spsc_pop(kq_spsc_t *q, void *elem)
{
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (h == t) {
        return false;
    }
    memcpy(elem, &q->storage[(t & q->mask) * q->elem_size], q->elem_size);
    atomic_store_explicit(&q->tail, t + 1u, memory_order_release);
    return true;
}

static inline void kq_spsc_reset(kq_spsc_t *q, uint32_t start_index)
{
    /* Test hook: position both indices near a chosen value (e.g. 0xFFFFFFF0) to exercise wrap. */
    atomic_store(&q->head, start_index);
    atomic_store(&q->tail, start_index);
}

#endif
