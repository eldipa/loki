#ifndef LOKI_LOCK_H_
#define LOKI_LOCK_H_

// XXX assumption: we are running on an intel x86 CPU
// https://elixir.bootlin.com/linux/v4.5/source/arch/x86/include/asm/processor.h#L560
#define loki_cpu_relax() do { asm volatile("rep; nop" ::: "memory"); } while(0)

#endif
