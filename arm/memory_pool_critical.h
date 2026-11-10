#ifndef MICROKERNEL_ARM_MEMORY_POOL_CRITICAL_H
#define MICROKERNEL_ARM_MEMORY_POOL_CRITICAL_H

#include "memory_pool.h"

KernelMemoryPoolCriticalState ARM_MemoryPool_EnterCritical(void);

void ARM_MemoryPool_ExitCritical(
    KernelMemoryPoolCriticalState state
);

#endif
