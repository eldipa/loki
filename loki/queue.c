#include "loki/queue.h"

#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>

#include <assert.h>

// XXX assumption: we are running on an intel x86 CPU
// https://elixir.bootlin.com/linux/v4.5/source/arch/x86/include/asm/processor.h#L560
static void loki_cpu_relax() {
    asm volatile("rep; nop" ::: "memory");
}

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

    // Sounds weird but remember: we allocated a queue of size sz
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

    // Update the old_prod_head reserving one slot for our datum.
    // Keep trying (CAS loop) until we can reserve it

    // Here is where we need prod_head and cons_tail to be
    // volatile: the compiler must not optimize them so
    // in each loop we get the freshnest values possible.
    //
    // XXX assumption: uint32_t reads (loads) are atomic
    //
    // Note that the CAS instruction will update this
    // if the instruction fails
    old_prod_head = q->prod_head;
    do {

        // Here we load the consumer tail with ACQUIRE.
        // This ensures that the reads (loads) that happened
        // before in *other* thread are visible by us. In particular
        // this ensure that the data was read before we try to
        // override them.
        cons_tail = __atomic_load_n(&q->cons_tail, __ATOMIC_ACQUIRE);

        uint32_t free_entries = (capacity + cons_tail - old_prod_head);

        // the user is happy pushing len or less items so let's
        // try to push as much as we can
        if (flags & LOKI_SOME) {
            n = (free_entries < len) ? free_entries : n;
        }

        if (!free_entries || free_entries < n) {
            errno = ENOBUFS;
            _dbg_mutex_unlock(&q->mx);
            return 0;
        }

        new_prod_head = (old_prod_head + n);

    } while (!__atomic_compare_exchange_n(
                &q->prod_head,          // what we want to update,
                &old_prod_head,             // asumming that still have this value,
                new_prod_head,         // with this value as the new one.
                false,            // stronger. TODO is a weak version ok too?
                __ATOMIC_RELAXED, // TODO and what about these mem orders?
                __ATOMIC_RELAXED
                ));

    assert(n > 0 && n <= len);

    // slots reserved, we are free to store the data
    // (old_prod_head is the previous head)
    // See the ACQUIRE-RELEASE semanitcs (see below).
    // That should ensure that any reader will se our data
    // consistent even if this store is not atomic.
    for (uint32_t i = 0; i < n; ++i)
        q->data[(old_prod_head + i) & mask] = data[i];

    // Now, we cannot update the prod_tail directly. Imagine
    // that there is another thread that is doing a push too.
    // It did the CAS loop but it didn't the store of the datum.
    // If we increse the prod_tail, we will saying "hey, there is
    // a new item here, read it" but it will be *not* our datum
    // but the non-written-yet datum of the other thread.
    // For this reason ww need to loop until all the threads
    // that started before us and are still pushing finish.
    while (q->prod_tail != old_prod_head) {
        // Tell the CPU that this is busy-loop so he can take a rest
        loki_cpu_relax();
    }

    // Okay, it is our turns now, update the prod_tail
    // telling to the world: here is a new datum for you readers!
    // The producer's tail points to the first empty slot: it serves
    // as a mark for the consumers to stop them further.
    //
    // We use a atomic store with RELEASE semantic. This not only
    // makes the store atomic but also forces the compiler and the CPU
    // to preserve a happen-before relationship.
    //
    // Imagine the thread W (us, the writer) and the thread R (the reader).
    // We want that any write done by W that happened before this atomic store,
    // like the store of the datum above, be visible by R when it reads this
    // new prod_tail value.
    //
    // So, if R does __atomic_load_n(&q->prod_tail, __ATOMIC_ACQUIRE) and
    // it gets our new_prod_head, then from its point of view, the datum
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
    // the writer W and the reader R
    //
    // W does a push and loads (ACQUIRE) the consumer tail
    // while R does a pop and stores (RELEASE) the same.
    // By the time that W see the consumer tail value set by R,
    // the datum read by R (store) will be complete. So we don't
    // have the risk of W overriding a datum that has not been read yet.
    //
    // The same happens for the pair R pop's load (ACQUIRE) of
    // the producer tail and the W push's store (RELEASE) of it.
    // When R does a pop, it loads the producer tail ensuring that
    // all the writes that happen before (the push of the datum)
    // are visible by R by the moment of the load ensuring that
    // R will not read garbage.
    uint32_t old_cons_head, prod_tail, new_cons_head;
    uint32_t mask = q->cons_mask;

    uint32_t n = len;
    if (n == 0) {
        errno = EINVAL;
        return 0;
    }

    do {
        old_cons_head = q->cons_head;

        prod_tail = __atomic_load_n(&q->prod_tail, __ATOMIC_ACQUIRE);

        // We now that the prod's tail is always in front of the
        // cons' head (worst case both are at the same position)
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
        if (flags & LOKI_SOME) {
            n = (ready_entries < len) ? ready_entries : n;
        }

        if (!ready_entries || ready_entries < n) {
            errno = EINVAL;
            _dbg_mutex_unlock(&q->mx);
            return 0;
        }

        new_cons_head = (old_cons_head + n);

    } while (!__atomic_compare_exchange_n(
                &q->cons_head,          // what we want to update,
                &old_cons_head,             // asumming that still have this value,
                new_cons_head,         // with this value as the new one.
                false,            // TODO is a weak version ok?
                __ATOMIC_RELAXED, // TODO and what about these mem orders?
                __ATOMIC_RELAXED
                ));

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
