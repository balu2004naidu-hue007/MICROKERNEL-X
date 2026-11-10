#include "memory_pool_critical.h"

KernelMemoryPoolCriticalState ARM_MemoryPool_EnterCritical(void)
{
    uint32_t primask;

    __asm volatile (
        "mrs %0, primask\n\t"
        "cpsid i"
        : "=r" (primask)
        :
        : "memory"
    );

    return (KernelMemoryPoolCriticalState)primask;
}

void ARM_MemoryPool_ExitCritical(
    KernelMemoryPoolCriticalState state)
{
    uint32_t primask = (uint32_t)state;

    __asm volatile (
        "msr primask, %0"
        :
        : "r" (primask)
        : "memory"
    );
}
