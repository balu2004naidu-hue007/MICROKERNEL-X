#include "memory_pool.h"

#include <stdint.h>
#include <limits.h>

static int MemoryPool_IsValid(const KernelMemoryPool *pool)
{
    if (pool == NULL ||
        pool->storage == NULL ||
        pool->block_size == 0U ||
        pool->block_size % _Alignof(max_align_t) != 0U ||
        (uintptr_t)pool->storage % _Alignof(max_align_t) != 0U ||
        pool->block_count == 0U ||
        pool->block_count > KERNEL_MEMORY_POOL_MAX_BLOCKS)
    {
        return 0;
    }

    if (pool->block_count > SIZE_MAX / pool->block_size)
    {
        return 0;
    }

    return 1;
}

int Kernel_MemoryPool_Init(
    KernelMemoryPool *pool,
    void *storage,
    size_t block_size,
    size_t block_count)
{
    if (pool == NULL ||
        storage == NULL ||
        block_size == 0U ||
        block_size % _Alignof(max_align_t) != 0U ||
        (uintptr_t)storage % _Alignof(max_align_t) != 0U ||
        block_count == 0U ||
        block_count > KERNEL_MEMORY_POOL_MAX_BLOCKS ||
        block_count > SIZE_MAX / block_size)
    {
        return -1;
    }

    pool->storage = (unsigned char *)storage;
    pool->block_size = block_size;
    pool->block_count = block_count;

    if (block_count == KERNEL_MEMORY_POOL_MAX_BLOCKS)
    {
        pool->free_bitmap = UINT32_MAX;
    }
    else
    {
        pool->free_bitmap = (UINT32_C(1) << block_count) - 1U;
    }

    pool->enter_critical = NULL;
    pool->exit_critical = NULL;

    return 0;
}

int Kernel_MemoryPool_SetCriticalSection(
    KernelMemoryPool *pool,
    KernelMemoryPoolEnterCriticalFn enter,
    KernelMemoryPoolExitCriticalFn exit)
{
    if (!MemoryPool_IsValid(pool))
    {
        return -1;
    }

    /* Install both callbacks together, or clear both. */
    if ((enter == NULL) != (exit == NULL))
    {
        return -1;
    }

    pool->enter_critical = enter;
    pool->exit_critical = exit;
    return 0;
}

void *Kernel_MemoryPool_Alloc(KernelMemoryPool *pool)
{
    if (!MemoryPool_IsValid(pool))
    {
        return NULL;
    }

    KernelMemoryPoolEnterCriticalFn enter_critical =
        pool->enter_critical;
    KernelMemoryPoolExitCriticalFn exit_critical =
        pool->exit_critical;
    KernelMemoryPoolCriticalState critical_state = 0;
    void *allocated = NULL;

    if (enter_critical != NULL)
    {
        critical_state = enter_critical();
    }

    for (size_t i = 0U; i < pool->block_count; ++i)
    {
        uint32_t mask = UINT32_C(1) << i;

        if ((pool->free_bitmap & mask) != 0U)
        {
            pool->free_bitmap &= ~mask;
            allocated = pool->storage + (i * pool->block_size);
            break;
        }
    }

    if (exit_critical != NULL)
    {
        exit_critical(critical_state);
    }

    return allocated;
}

int Kernel_MemoryPool_Free(
    KernelMemoryPool *pool,
    void *memory)
{
    if (!MemoryPool_IsValid(pool) || memory == NULL)
    {
        return -1;
    }

    uintptr_t base = (uintptr_t)pool->storage;
    uintptr_t address = (uintptr_t)memory;

    if (address < base)
    {
        return -1;
    }

    uintptr_t offset = address - base;
    size_t total_size = pool->block_size * pool->block_count;

    if (offset >= total_size ||
        offset % pool->block_size != 0U)
    {
        return -1;
    }

    size_t index = (size_t)(offset / pool->block_size);
    uint32_t mask = UINT32_C(1) << index;

    KernelMemoryPoolEnterCriticalFn enter_critical =
        pool->enter_critical;
    KernelMemoryPoolExitCriticalFn exit_critical =
        pool->exit_critical;
    KernelMemoryPoolCriticalState critical_state = 0;
    int result = -1;

    if (enter_critical != NULL)
    {
        critical_state = enter_critical();
    }

    /* Check and update the bitmap atomically with respect to
       callers that use the same critical-section callbacks. */
    if ((pool->free_bitmap & mask) == 0U)
    {
        pool->free_bitmap |= mask;
        result = 0;
    }

    if (exit_critical != NULL)
    {
        exit_critical(critical_state);
    }

    return result;
}

size_t Kernel_MemoryPool_GetFreeCount(
    const KernelMemoryPool *pool)
{
    if (!MemoryPool_IsValid(pool))
    {
        return 0U;
    }

    KernelMemoryPoolEnterCriticalFn enter_critical =
        pool->enter_critical;
    KernelMemoryPoolExitCriticalFn exit_critical =
        pool->exit_critical;
    KernelMemoryPoolCriticalState critical_state = 0;

    if (enter_critical != NULL)
    {
        critical_state = enter_critical();
    }

    size_t free_count = 0U;

    for (size_t i = 0U; i < pool->block_count; ++i)
    {
        if ((pool->free_bitmap & (UINT32_C(1) << i)) != 0U)
        {
            ++free_count;
        }
    }

    if (exit_critical != NULL)
    {
        exit_critical(critical_state);
    }

    return free_count;
}
