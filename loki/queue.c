#include "loki/queue.h"
#include "loki/common.h"

#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>

#include <assert.h>

// About memory order
// https://en.cppreference.com/w/cpp/atomic/memory_order
// https://gcc.gnu.org/wiki/Atomic/GCCMM/AtomicSync
// https://fedoraproject.org/wiki/Architectures/ARM/GCCBuiltInAtomicOperations
// http://stdatomic.gforge.inria.fr/
//
// http://git.dpdk.org/dpdk/tree/lib/librte_ring/rte_ring_c11_mem.h
//
// Further reading
// http://locklessinc.com/articles/locks/
// https://www.usenix.org/legacy/publications/library/proceedings/als00/2000papers/papers/full_papers/sears/sears_html/index.html

uint32_t loki_queue__push(
        struct loki_queue *q,
        uint32_t *data,
        uint32_t len,
        int flags
        ) {
    _dbg_mutex_lock(&q->mx);

    uint32_t old_prod_head, cons_tail, new_prod_head;
    uint32_t mask = q->prod_mask;
    int success;

    // We allocated a queue of size sz
    // and by definition the mask is sz-1.
    // Now, the queue always leaves 1 slot empty between the head
    // and the tail to differentiate a full queue from an empty queue
    // so the capacity is also sz-1
    uint32_t capacity = mask;

    // Get a copy. During the CAS loop we will want to change
    // this a few times and len will be our point of reference
    uint32_t n = len;
    if (n == 0) {
        errno = EINVAL;
        return 0;
    }

    // Note that the CAS instruction will update this atomically
    // if the CAS instruction fails.
    // So we need to load this explicitly once. Because
    // multiple producers may write this, we need an atomic load.
    //
    // If there is only one producer, the atomic load is unnecessary
    // in that case. I'm speculating that adding an "if" to check that
    // will be more expensive that an atomic load.
    old_prod_head = __atomic_load_n(&q->prod_head, __ATOMIC_RELAXED);

    // Update the old_prod_head reserving enough entries for our data.
    // Keep trying (CAS loop) until we success
    do {

        // Here we load the consumer tail with ACQUIRE.
        // This ensures that the reads (loads) that happened
        // before in *other* thread are visible by us. In particular
        // this ensure that the data was read before we try to
        // override them.
        // This is complemented with the RELEASE store in the pop()
        cons_tail = __atomic_load_n(&q->cons_tail, __ATOMIC_ACQUIRE);

        uint32_t free_entries = (capacity + cons_tail - old_prod_head);

        // the user is happy pushing len or less items so let's
        // try to push as much as we can
        if (flags & LOKI_SOME_DATA) {
            n = (free_entries < len) ? free_entries : n;
        }

        if (!free_entries || free_entries < n) {
            errno = ENOBUFS;
            _dbg_mutex_unlock(&q->mx);
            return 0;
        }

        new_prod_head = (old_prod_head + n);
        success = 1;
        if (flags & LOKI_SINGLE)
            // single producer, we don't need an atomic store
            q->prod_head = new_prod_head;
        else
            success = __atomic_compare_exchange_n(
                            &q->prod_head,      // what we want to update,
                            &old_prod_head,     // asumming that still have this value,
                            new_prod_head,      // with this value as the new one.
                            false,              // stronger. TODO is a weak version ok too?
                            __ATOMIC_RELAXED,   // TODO and what about these mem orders?
                            __ATOMIC_RELAXED
                        );

    } while (!success);

    assert(n > 0 && n <= len);

    // slots reserved, we are free to store the data
    // (old_prod_head is the previous head)
    // See the ACQUIRE-RELEASE semanitcs (see below).
    // That should ensure that any reader will see our data
    // after she acquire her tail even if thos store is not atomic.
    for (uint32_t i = 0; i < n; ++i)
        q->data[(old_prod_head + i) & mask] = data[i];

    // Now, we cannot update the prod_tail directly. Imagine
    // that there is another thread that is doing a push too.
    // It did the CAS loop but it didn't the store of the data.
    // If we increse the prod_tail, we will saying "hey, there is
    // a new data here, read it" but it will be *not* our data
    // but the non-written-yet data of the other thread.
    //
    // For this reason ww need to loop until all the threads
    // that started before us and are still pushing finish.
    while (q->prod_tail != old_prod_head) {
        // Tell the CPU that this is busy-loop so he can take a rest
        loki_cpu_relax();
    }

    // Okay, it is our turn now, update the prod_tail
    // telling to the world: "here are new data for you consumers!"
    //
    // The producer's tail points to the first empty slot: it serves
    // as a mark for the consumers to stop them further.
    //
    // We use a atomic store with RELEASE semantic. This not only
    // makes the store atomic but also forces the compiler and the CPU
    // to preserve a happen-before relationship.
    //
    // Imagine the thread P (us, the producer) and the thread C (the consumer).
    //
    // We want that any write done by P that happened before this atomic store,
    // like the store of the data above, be visible by C when it reads this
    // new prod_tail value.
    //
    // So, if C does __atomic_load_n(&q->prod_tail, __ATOMIC_ACQUIRE) and
    // it gets our new_prod_head, then from her point of view, the data
    // will be there in the array.
    __atomic_store_n(&q->prod_tail, new_prod_head, __ATOMIC_RELEASE);
    _dbg_mutex_unlock(&q->mx);
    return n;
}

uint32_t loki_queue__pop(
        struct loki_queue *q,
        uint32_t *data,
        uint32_t len,
        int flags
        ) {
    _dbg_mutex_lock(&q->mx);
    // This pop is a symmetric version of the push. See the comments
    // of it.
    //
    // One particular observation are the pairs of load and stores
    // with ACQUIRE/RELEASE semantics and the relationship between
    // the producer P and the consumer C
    //
    // P does a push and loads (ACQUIRE) the consumer tail
    // while C does a pop and stores (RELEASE) the same.
    //
    // By the time that P see the consumer tail value set by C,
    // the data read by C (store) will be completed. So we don't
    // have the risk of P overriding the data that has not been read yet.
    //
    // The same happens for the pair C pop's load (ACQUIRE) of
    // the producer tail and the P push's store (RELEASE) of it.
    //
    // When C does a pop, it loads the producer tail ensuring that
    // all the writes that happen before (the push of the data)
    // are visible by C by the moment of the load ensuring that
    // C will not read garbage.
    uint32_t old_cons_head, prod_tail, new_cons_head;
    uint32_t mask = q->cons_mask;
    int success;

    uint32_t n = len;
    if (n == 0) {
        errno = EINVAL;
        return 0;
    }

    old_cons_head = __atomic_load_n(&q->cons_head, __ATOMIC_RELAXED);
    do {
        prod_tail = __atomic_load_n(&q->prod_tail, __ATOMIC_ACQUIRE);

        // We know that the prod's tail is always in front of the
        // cons' head (worst case both are at the same position)
        //
        // In the case that the prod's tail overflow, the behaviour
        // is well defined for unsigned types and the substraction
        // (a negative value) the same.
        // No "masking" is needed here.
        //
        // This is subtle but important. In the push we compare
        // the producer next head with the consumer tail
        // But in the pop we compare the consumer head (not the
        // consumer next head) with the product tail.
        uint32_t ready_entries = prod_tail - old_cons_head;
        assert(ready_entries < mask + 1);

        // Pop as much as we can
        if (flags & LOKI_SOME_DATA) {
            n = (ready_entries < len) ? ready_entries : n;
        }

        if (!ready_entries || ready_entries < n) {
            errno = EINVAL;
            _dbg_mutex_unlock(&q->mx);
            return 0;
        }

        new_cons_head = (old_cons_head + n);

        success = 1;
        if (flags & LOKI_SINGLE)
            q->cons_head = new_cons_head;
        else
            success = __atomic_compare_exchange_n(
                            &q->cons_head,
                            &old_cons_head,
                            new_cons_head,
                            false,
                            __ATOMIC_RELAXED,
                            __ATOMIC_RELAXED
                        );
    } while (!success);

    assert(n > 0 && n <= len);
    for (uint32_t i = 0; i < n; ++i)
        data[i] = q->data[(old_cons_head + i) & mask];

    while (q->cons_tail != old_cons_head) {
        loki_cpu_relax();
    }

    __atomic_store_n(&q->cons_tail, new_cons_head, __ATOMIC_RELEASE);
    _dbg_mutex_unlock(&q->mx);
    return n;
}

int loki_queue__init(struct loki_queue *q, uint32_t sz) {
    // Power of 2 only
    if (!sz || (sz & (sz-1))) {
        errno = EINVAL;
        return -1;
    }

    q->prod_mask = q->cons_mask = (sz-1);

    q->data = malloc(sizeof(*q->data) * sz);
    if (!q->data)
        return -1;

    q->prod_tail = q->prod_head = 0;
    q->cons_tail = q->cons_head = 0;

    _dbg_mutex_init(&q->mx);
    return 0;
}

void loki_queue__destroy(struct loki_queue *q) {
    _dbg_mutex_destroy(&q->mx);
    free(q->data);
}

uint32_t loki_queue__ready(struct loki_queue *q) {
    return q->prod_tail - q->cons_head;
}

uint32_t loki_queue__free(struct loki_queue *q) {
    uint32_t capacity = q->prod_mask;
    return (capacity - q->cons_tail - q->prod_head);
}
