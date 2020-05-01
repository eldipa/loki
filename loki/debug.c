#include "loki/debug.h"

#include <stdint.h>

#ifdef LOKI_ENABLE_TRACE

uint32_t _dbg_trace_pos = 0;
struct _dbg_trace_entry_t _dbg_trace_buf[_LOKI_TRACE_ENTRY_CNT] = {0};

uint32_t _dbg_trace_last_entry_at() {
    uint32_t seq = __atomic_load_n(&_dbg_trace_pos, __ATOMIC_RELAXED);
    seq--;
    return seq & _LOKI_TRACE_BUF_MASK;
}

int  _dbg_trace_dump() {
    FILE *f = fopen("dbg_trace_buf", "wt");
    if (!f)
        return -1;

    for (uint32_t i = 0; i < _LOKI_TRACE_ENTRY_CNT; ++i) {
        struct _dbg_trace_entry_t cur = _dbg_trace_buf[i];
        fprintf(f, "%08x %08x %s\n", cur.seq, cur.id, cur.msg);
    }

    fclose(f);
}

#endif
