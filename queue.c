#include "queue.h"

#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>

#include <assert.h>

// XXX assumption: we are running on an intel x86 CPU
// https://elixir.bootlin.com/linux/v4.5/source/arch/x86/include/asm/processor.h#L560
static void lckfree_cpu_relax() {
    asm volatile("rep; nop" ::: "memory");
}

// About memory order
// https://en.cppreference.com/w/cpp/atomic/memory_order
// https://gcc.gnu.org/wiki/Atomic/GCCMM/AtomicSync
//
// http://git.dpdk.org/dpdk/tree/lib/librte_ring/rte_ring_c11_mem.h
//
// Further reading
// http://locklessinc.com/articles/locks/
// https://www.usenix.org/legacy/publications/library/proceedings/als00/2000papers/papers/full_papers/sears/sears_html/index.html

int lckfree_queue__push(struct lckfree_queue *q, uint32_t datum) {
    uint32_t prod_head, cons_tail, prod_next_head;

    assert(datum != UINT32_MAX);

    // Update the prod_head reserving one slot for our datum.
    // Keep trying (CAS loop) until we can reserve it
    do {
        // Here is where we need prod_head and cons_tail to be
        // volatile: the compiler must not optimize them so
        // in each loop we get the freshnest values possible.
        //
        // XXX assumption: uint32_t reads (loads) are atomic
        prod_head = __atomic_load_n(&q->prod_head, __ATOMIC_RELAXED);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        // Here we load the consumer tail with ACQUIRE.
        // This ensures that the reads (loads) that happened
        // before in *other* thread are visible by us. In particular
        // this ensure that the data was read before we try to
        // override them.
        cons_tail = __atomic_load_n(&q->cons_tail, __ATOMIC_ACQUIRE);

        prod_next_head = (prod_head + 1) & q->prod_mask;
        if (prod_next_head == cons_tail) {
            errno = ENOBUFS;
            return -1;
        }
    } while (!__atomic_compare_exchange_n(
                &q->prod_head,          // what we want to update,
                &prod_head,             // asumming that still have this value,
                prod_next_head,         // with this value as the new one.
                false,            // stronger. TODO is a weak version ok too?
                __ATOMIC_RELAXED, // TODO and what about these mem orders?
                __ATOMIC_RELAXED
                ));

    // slot reserved, we are free to store the datum
    // (prod_head is the previous head)
    // See the ACQUIRE-RELEASE semanitcs (see below).
    // That should ensure that any reader will se our datum
    // consistent even if this store is not atomic.
    q->data[prod_head] = datum;
    __atomic_fetch_add(&q->count, 1, __ATOMIC_RELAXED);

    // Now, we cannot update the prod_tail directly. Imagine
    // that there is another thread that is doing a push too.
    // It did the CAS loop but it didn't the store of the datum.
    // If we increse the prod_tail, we will saying "hey, there is
    // a new item here, read it" but it will be *not* our datum
    // but the non-written-yet datum of the other thread.
    // For this reason ww need to loop until all the threads
    // that started before us and are still pushing finish.
    while (q->prod_tail != prod_head) {
        // Tell the CPU that this is busy-loop so he can take a rest
        lckfree_cpu_relax();
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
    // it gets our prod_next_head, then from its point of view, the datum
    // will be there in the array.
    __atomic_store_n(&q->prod_tail, prod_next_head, __ATOMIC_RELEASE);
    return 0;
}

int lckfree_queue__pop(struct lckfree_queue *q, uint32_t *datum) {
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
    uint32_t cons_head, prod_tail, cons_next_head;

    do {
        cons_head = __atomic_load_n(&q->cons_head, __ATOMIC_RELAXED);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        prod_tail = __atomic_load_n(&q->prod_tail, __ATOMIC_ACQUIRE);

        // This is subtle but important. In the push we compare
        // the producer next head with the consumer tail
        // But in the pop we compare the consumer head (not the
        // consumer next head) with the product tail.
        if (cons_head == prod_tail) {
            errno = EINVAL;
            return -1;
        }

        cons_next_head = (cons_head + 1) & q->cons_mask;

    } while (!__atomic_compare_exchange_n(
                &q->cons_head,          // what we want to update,
                &cons_head,             // asumming that still have this value,
                cons_next_head,         // with this value as the new one.
                false,            // TODO is a weak version ok?
                __ATOMIC_RELAXED, // TODO and what about these mem orders?
                __ATOMIC_RELAXED
                ));

    *datum = q->data[cons_head];
    assert(*datum != UINT32_MAX);
    __atomic_fetch_sub(&q->count, 1, __ATOMIC_RELAXED);

    while (q->cons_tail != cons_head) {
        lckfree_cpu_relax();
    }

    __atomic_store_n(&q->cons_tail, cons_next_head, __ATOMIC_RELEASE);
    return 0;
}

int lckfree_queue__init(struct lckfree_queue *q, uint32_t sz) {
    // Power of 2 only
    if (!sz || (sz & (sz-1))) {
        errno = EINVAL;
        return -1;
    }

    q->prod_mask = q->cons_mask = (sz-1);

    q->data = malloc(sizeof(*q->data) * sz);
    if (!q->data)
        return -1;

    for (size_t i = 0; i < sz; ++i)
        q->data[i] = UINT32_MAX;

    q->prod_tail = q->prod_head = 0;
    q->cons_tail = q->cons_head = 0;

    q->count = 0;

    return 0;
}

void lckfree_queue__destroy(struct lckfree_queue *q) {
    free(q->data);
}

uint32_t lckfree_queue__used(struct lckfree_queue *q) {
    // prod_tail - cons_head
    return __atomic_load_n(&q->count, __ATOMIC_RELAXED);
}

uint32_t lckfree_queue__free(struct lckfree_queue *q) {
    // cons_tail - prod_head
    uint32_t mask = q->prod_mask;
    return (mask + 1) - __atomic_load_n(&q->count, __ATOMIC_RELAXED);
}
