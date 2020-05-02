#ifndef LOKI_DEBUG_H_
#define LOKI_DEBUG_H_

#ifdef LOKI_ENABLE_DEBUG_LOCK
#include <pthread.h>
#include <stdio.h>

#define _dbg_mutex_var(name) pthread_mutex_t name
#define _dbg_mutex_init(name) pthread_mutex_init((name), NULL)
#define _dbg_mutex_destroy(name) pthread_mutex_destroy((name))
#define _dbg_mutex_lock(name) pthread_mutex_lock((name))
#define _dbg_mutex_unlock(name) pthread_mutex_unlock((name))

#define _dbg_warn(txt) fprintf(stderr, "%s\n", (txt))

#else   // else of LOKI_ENABLE_DEBUG_LOCK

#define _dbg_mutex_var(name)
#define _dbg_mutex_init(name)
#define _dbg_mutex_destroy(name)
#define _dbg_mutex_lock(name)
#define _dbg_mutex_unlock(name)

#define _dbg_warn(txt)

#endif // end of LOKI_ENABLE_DEBUG_LOCK


#ifdef LOKI_ENABLE_TRACE
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "loki/common.h"

#ifndef LOKI_TRACE_ENTRY_SZ
#define LOKI_TRACE_ENTRY_SZ 128
#endif

#ifndef LOKI_TRACE_BUF_SZ
#define LOKI_TRACE_BUF_SZ 33554432 /* 32 MB */
#endif

static_assert(
    (LOKI_TRACE_BUF_SZ & (LOKI_TRACE_BUF_SZ-1)) == 0,
    "Trace buffer must have a size power of 2"
    );

static_assert(
    (LOKI_TRACE_BUF_SZ % LOKI_TRACE_ENTRY_SZ) == 0,
    "Trace buffer must be a multiple of the size of an entry"
    );

#define _LOKI_TRACE_ENTRY_CNT  (LOKI_TRACE_BUF_SZ/LOKI_TRACE_ENTRY_SZ)
#define _LOKI_TRACE_BUF_MASK   (_LOKI_TRACE_ENTRY_CNT - 1)

static_assert(
    (_LOKI_TRACE_ENTRY_CNT & (_LOKI_TRACE_ENTRY_CNT-1)) == 0,
    "Trace entry count must be power of 2"
    );

#define _LOKI_TRACE_MSG_SZ (LOKI_TRACE_ENTRY_SZ-8-sizeof(char*))
struct _dbg_trace_entry_t {
    uint32_t id;
    uint32_t seq;
    const char *ptr_msg;
    char msg[_LOKI_TRACE_MSG_SZ];
} __attribute__ ((packed));


extern const uint32_t _dbg_trace_mask;
extern uint32_t _dbg_trace_pos;
extern struct _dbg_trace_entry_t _dbg_trace_buf[_LOKI_TRACE_ENTRY_CNT];

// Reserve space atomically and write the data in the buffer.
// The data may not be visible by others. We'll live with this
// limitation so we can avoid a write barrier/release memory model.
//
// Not matter if we are at the end of the buffer, we'll have
// always space enough to write a full entry (the buffer is
// larger than you think)
#define _dbg_tracef(fmt, ...)  do {                                           \
    uint32_t seq = __atomic_fetch_add(&_dbg_trace_pos, 1, __ATOMIC_RELAXED);  \
    uint32_t pos = seq & _LOKI_TRACE_BUF_MASK;                                \
                                                                              \
    _dbg_trace_buf[pos].id = loki_thread_id();                                \
    _dbg_trace_buf[pos].seq = seq;                                            \
    _dbg_trace_buf[pos].ptr_msg = NULL;                                       \
    snprintf(_dbg_trace_buf[pos].msg, _LOKI_TRACE_MSG_SZ, fmt, __VA_ARGS__);  \
} while (0)

#define _dbg_trace(msg)  do {                                                 \
    uint32_t seq = __atomic_fetch_add(&_dbg_trace_pos, 1, __ATOMIC_RELAXED);  \
    uint32_t pos = seq & _LOKI_TRACE_BUF_MASK;                                \
                                                                              \
    _dbg_trace_buf[pos].id = loki_thread_id();                                \
    _dbg_trace_buf[pos].seq = seq;                                            \
    _dbg_trace_buf[pos].ptr_msg = msg;                                        \
    _dbg_trace_buf[pos].msg[0] = 0;                                           \
} while (0)

uint32_t _dbg_trace_last_entry_at() __attribute__((used));
int  _dbg_trace_dump() __attribute__((used));

#else   // else of LOKI_ENABLE_TRACE

#define _dbg_trace(fmt, ...)

#endif  // else of LOKI_ENABLE_TRACE

#endif
