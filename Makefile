# Turn on/off debug mode (off by default).
#
# Debug mode disables compiler optimizations and
# adds debugging metadata.
DEBUG =

# Turn on/off the debug locks (off by default).
#
# If enabled, the lock free structures will use a traditional
# lock (a mutex). This makes easier to debug problems when the
# issue is in a atomic operation or memory reorder.
LOCK =

# Turn on/off the trace mode (off by default).
#
# If enabled, a global and shared ring buffer is created and
# different part of the system will log to it atomically (but
# see the limitations in the code).
# Use this for debugging and introspection.
TRACE =

# Turn on/off the sanitization mode. (off by default).
# This modes relays in the compiler's
# ability to instrument the code to detect race conditions in runtime.
# It makes the program between 5 and 15 times slower (and consumes around
# 15 times more memory)
# Makes sense to use this only for testing and with DEBUG turned off.
SANITIZE =

CC = gcc
CFLAGS = -std=gnu11 -march=core2
LDFLAGS =
LDLIBS = -lpthread
INCLUDES = -I.

CONFFLAGS = -DLOKI_CPU_RELAX_INSTR_PAUSE

CFLAGS += $(CONFFLAGS)

ifeq (1,$(DEBUG))
	CFLAGS += -O0 -ggdb -DDEBUG
else
	CFLAGS += -O2
endif

ifeq (1,$(LOCK))
	CFLAGS += -DLOKI_ENABLE_DEBUG_LOCK
endif

ifeq (1,$(TRACE))
	CFLAGS += -DLOKI_ENABLE_TRACE
endif

ifeq (1,$(SANITIZE))
	CFLAGS += -fsanitize=thread
	LDFLAGS += -fsanitize=thread
endif

queue-test: loki/*.c loki/*.h tests/queue-test.c
	$(CC) $(INCLUDES) $(CFLAGS) $(LDFLAGS) -o $@ loki/*.c tests/queue-test.c $(LDLIBS)

clean:
	rm -f queue-test
