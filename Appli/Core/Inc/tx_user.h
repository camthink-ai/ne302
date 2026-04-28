#ifndef TX_USER_H
#define TX_USER_H

// #define TX_MAX_PRIORITIES 64

// #define TX_TIMER_TICKS_PER_SECOND (1000UL)

// #ifndef SYSTEM_CLOCK
// #define SYSTEM_CLOCK 800000000
// #endif

// #ifndef TICK_FREQ
// #define TICK_FREQ TX_TIMER_TICKS_PER_SECOND
// #endif

#define RTOS2_BYTE_POOL_STACK_SIZE              64 * 1024

#define RTOS2_BYTE_POOL_HEAP_SIZE               32 * 1024



#define TX_MAX_PRIORITIES           64

#define TX_TIMER_TICKS_PER_SECOND   (1000UL)

#ifndef SYSTEM_CLOCK
/* Used at link time by tx_initialize_low_level.S; must be a compile-time constant.
 * main.c calls threadx_resync_systick_from_rcc() after SystemClock_Config() so
 * SysTick matches HAL_RCC_GetCpuClockFreq() at runtime. */
#define SYSTEM_CLOCK                800000000UL
#endif /* SYSTEM_CLOCK */

#ifndef TICK_FREQ
#define TICK_FREQ TX_TIMER_TICKS_PER_SECOND
#endif

#define TX_THREAD_USER_EXTENSION ULONG tx_thread_detached_joinable; ULONG tx_thread_alloc_flags; VOID *txfr_thread_ptr;

/* Thread resource ownership flags for CMSIS-RTOS2 wrapper */
#define TX_THREAD_ALLOC_OWNS_STACK   (1UL << 0)
#define TX_THREAD_ALLOC_OWNS_CB      (1UL << 1)


#endif
