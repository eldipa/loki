#ifndef LOKI_LOCK_H_
#define LOKI_LOCK_H_

// XXX assumption: we are running on an intel x86 CPU
// https://c9x.me/x86/html/file_module_x86_id_232.html
#if defined (LOKI_CPU_RELAX_INSTR_PAUSE)
#define loki_cpu_relax() do { asm volatile("pause\n": : :"memory"); } while(0)

// XXX assumption: we are running on an intel x86 CPU
// https://elixir.bootlin.com/linux/v4.5/source/arch/x86/include/asm/processor.h#L560
#elif defined (LOKI_CPU_RELAX_INSTR_REP_NOP)
#define loki_cpu_relax() do { asm volatile("rep; nop" ::: "memory"); } while(0)

// Assume that the user provided one impl, fail otherwise
#else
#if !defined (loki_cpu_relax)
#error "No loki_cpu_relax macro was defined. You need to provide one of choose one of the valid options for LOKI_CPU_RELAX_INSTR (predefined loki_cpu_relax macros)"
#endif

#endif
#endif
