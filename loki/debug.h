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

#else

#define _dbg_mutex_var(name)
#define _dbg_mutex_init(name)
#define _dbg_mutex_destroy(name)
#define _dbg_mutex_lock(name)
#define _dbg_mutex_unlock(name)

#define _dbg_warn(txt)

#endif
#endif
