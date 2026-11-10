#ifndef MICROKERNEL_MEMORY_POOL_H
#define MICROKERNEL_MEMORY_POOL_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_MEMORY_POOL_MAX_BLOCKS 32U

typedef uintptr_t KernelMemoryPoolCriticalState;

typedef KernelMemoryPoolCriticalState
    (*KernelMemoryPoolEnterCriticalFn)(void);

typedef void
    (*KernelMemoryPoolExitCriticalFn)(
        KernelMemoryPoolCriticalState state
    );

typedef struct
{
    unsigned char *storage;
    size_t block_size;
    size_t block_count;
    uint32_t free_bitmap;

    KernelMemoryPoolEnterCriticalFn enter_critical;
    KernelMemoryPoolExitCriticalFn exit_critical;
} KernelMemoryPool;

int Kernel_MemoryPool_Init(
    KernelMemoryPool *pool,
    void *storage,
    size_t block_size,
    size_t block_count
);

int Kernel_MemoryPool_SetCriticalSection(
    KernelMemoryPool *pool,
    KernelMemoryPoolEnterCriticalFn enter,
    KernelMemoryPoolExitCriticalFn exit
);

void *Kernel_MemoryPool_Alloc(KernelMemoryPool *pool);

int Kernel_MemoryPool_Free(
    KernelMemoryPool *pool,
    void *memory
);

size_t Kernel_MemoryPool_GetFreeCount(
    const KernelMemoryPool *pool
);

#endif
