#ifndef KERNEL_CRITICAL_H
#define KERNEL_CRITICAL_H

#include <stdint.h>

typedef uintptr_t KernelCriticalState;

static inline KernelCriticalState Kernel_CriticalEnter(void)
{
#ifdef MICROKERNEL_ARM
    uint32_t primask;

    __asm volatile (
        "mrs %0, primask\n\t"
        "cpsid i"
        : "=r" (primask)
        :
        : "memory"
    );

    return (KernelCriticalState)primask;
#else
    /*
     * The current host runtime is single-threaded.
     * This is not a host-thread synchronization primitive.
     */
    return 0;
#endif
}

static inline void Kernel_CriticalExit(KernelCriticalState state)
{
#ifdef MICROKERNEL_ARM
    uint32_t primask = (uint32_t)state;

    __asm volatile (
        "msr primask, %0"
        :
        : "r" (primask)
        : "memory"
    );
#else
    (void)state;
#endif
}

#endif
