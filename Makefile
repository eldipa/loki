
CC = gcc
CFLAGS = -std=gnu11 -march=core2
LDLIBS = -lpthread
INCLUDES = -I.

CONFFLAGS = -DLOKI_CPU_RELAX_INSTR_PAUSE

CFLAGS += $(CONFFLAGS)

ifeq (1,$(DEBUG))
	CFLAGS += -O0 -ggdb -DLOKI_ENABLE_DEBUG_LOCK -DDEBUG
else
	CFLAGS += -O2
endif

queue-test: loki/*.c loki/*.h tests/queue-test.c
	$(CC) $(INCLUDES) $(CFLAGS) -o $@ loki/*.c tests/queue-test.c $(LDLIBS)
