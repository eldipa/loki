#ifndef LCKFREE_QUEUE_H_
#define LCKFREE_QUEUE_H_

#include <stdint.h>

#ifdef ENABLE_DEBUG_LOCK
#include <pthread.h>

#define _dbg_mutex_var(name) pthread_mutex_t name
#define _dbg_mutex_init(name) pthread_mutex_init((name), NULL)
#define _dbg_mutex_destroy(name) pthread_mutex_destroy((name))
#define _dbg_mutex_lock(name) pthread_mutex_lock((name))
#define _dbg_mutex_unlock(name) pthread_mutex_unlock((name))

#else

#define _dbg_mutex_var(name)
#define _dbg_mutex_init(name)
#define _dbg_mutex_destroy(name)
#define _dbg_mutex_lock(name)
#define _dbg_mutex_unlock(name)

#endif

//
// Multi Producer - Multi Consumer Bounded Queue
//
// This is a lock free structure that allows multiple
// threads to push (produce) and pop (consume) data
// in a FIFO way.
//
// The current implementation has uint32_t as the unit datum
// to save but other data types including pointers can be used
// hacking this structure a little.
//
// The queue is bounded with a size of N where N must be a
// power of 2. The implementation however allows to store
// only a maximum of N-1 elements.
//
// References:
//  - https://svnweb.freebsd.org/base/release/8.0.0/sys/sys/buf_ring.h?revision=199625&amp;view=markup
//  - https://doc.dpdk.org/guides-19.05/prog_guide/ring_lib.html
//
struct lckfree_queue {
    // On push (enqueue), the thread works as a producer:
    //  - it produces a new datum moving the head forward
    //  - and the datum enables the readers (consumers) to read it
    //    moving the tail forward to (yes, the push moves the tail too).
    volatile uint32_t prod_head;
    volatile uint32_t prod_tail;

    // The queue is memory-bounded. Instead of saving the
    // size of the queue we save the bit mask: assuming
    // a size power of 2 N, we can compute X % N as
    // X & mask for any integer. (where & is faster than %).
    uint32_t prod_mask;

    // Pad between producer and consumer attributes. This
    // avoids the "false sharing" problem: when we modify
    // and attribute, the whole L1/L2 cache line needs to be
    // updated in all the cores. If at the same time other
    // thread is accessing to the other attributes a conflict
    // will arise. The CPU will know how to fix it but it is
    // going to have a penalty.
    //
    // TODO How much?
    //
    // XXX assuming that the L1 and L2 cache lines are of 64 bytes
    uint32_t _pad1[13];

    // On pop (dequeue), the thread works as a consumer:
    //  - it consumes a datum moving the tail forward
    //  - and moves the head forward too, let the writers (producers)
    //    know that there is a new free slot there.
    volatile uint32_t cons_head;
    volatile uint32_t cons_tail;
    // Why again the mask? Having two copies of the mask, each next
    // to the respective head/tail ensures that the head, the tail
    // and the mask of the producer will be in its own L2 cache line
    // avoiding "false sharings"
    uint32_t cons_mask;

    uint32_t _pad2[13];

    _dbg_mutex_var(mx);

    // Where the data live
    uint32_t *data;
};

int lckfree_queue__push(struct lckfree_queue *q, uint32_t datum);
int lckfree_queue__pop(struct lckfree_queue *q, uint32_t *datum);

int lckfree_queue__init(struct lckfree_queue *q, uint32_t sz);
void lckfree_queue__destroy(struct lckfree_queue *q);

uint32_t lckfree_queue__ready(struct lckfree_queue *q);
uint32_t lckfree_queue__free(struct lckfree_queue *q);
#endif
