#include <assert.h>
#include <stdio.h>
#include <stddef.h>

#include "memory_pool.h"

static unsigned int critical_enter_count;
static unsigned int critical_exit_count;
static KernelMemoryPoolCriticalState last_critical_state;

static KernelMemoryPoolCriticalState TestEnterCritical(void)
{
    ++critical_enter_count;
    return (KernelMemoryPoolCriticalState)0x5AU;
}

static void TestExitCritical(KernelMemoryPoolCriticalState state)
{
    ++critical_exit_count;
    last_critical_state = state;
}

int main(void)
{
    enum { BLOCK_SIZE = 16, BLOCK_COUNT = 4 };

    union
    {
        max_align_t alignment;
        unsigned char bytes[BLOCK_SIZE * BLOCK_COUNT];
    } storage;

    KernelMemoryPool pool = {0};
    KernelMemoryPool invalid_pool = {0};

    /* Reject a block size that violates max_align_t alignment. */
    assert(Kernel_MemoryPool_Init(
        &invalid_pool, storage.bytes, 1U, BLOCK_COUNT) == -1);

    /* Reject storage that is deliberately misaligned by one byte. */
    assert(Kernel_MemoryPool_Init(
        &invalid_pool, storage.bytes + 1, BLOCK_SIZE, BLOCK_COUNT) == -1);

    assert(Kernel_MemoryPool_Init(
        &pool, storage.bytes, BLOCK_SIZE, BLOCK_COUNT) == 0);

    /* Reject an incomplete callback pair. */
    assert(Kernel_MemoryPool_SetCriticalSection(
        &pool, TestEnterCritical, NULL) == -1);

    assert(Kernel_MemoryPool_SetCriticalSection(
        &pool, TestEnterCritical, TestExitCritical) == 0);

    assert(Kernel_MemoryPool_GetFreeCount(&pool) == BLOCK_COUNT);
    assert(critical_enter_count == 1U);
    assert(critical_exit_count == 1U);
    assert(last_critical_state ==
           (KernelMemoryPoolCriticalState)0x5AU);

    void *blocks[BLOCK_COUNT];

    for (size_t i = 0; i < BLOCK_COUNT; ++i)
    {
        blocks[i] = Kernel_MemoryPool_Alloc(&pool);
        assert(blocks[i] != NULL);
        assert(Kernel_MemoryPool_GetFreeCount(&pool) ==
               BLOCK_COUNT - i - 1);
    }

    assert(Kernel_MemoryPool_Alloc(&pool) == NULL);

    for (size_t i = 0; i < BLOCK_COUNT; ++i)
    {
        for (size_t j = i + 1; j < BLOCK_COUNT; ++j)
        {
            assert(blocks[i] != blocks[j]);
        }
    }

    assert(Kernel_MemoryPool_Free(&pool, blocks[1]) == 0);
    assert(Kernel_MemoryPool_GetFreeCount(&pool) == 1);

    assert(Kernel_MemoryPool_Free(&pool, blocks[1]) == -1);
    assert(Kernel_MemoryPool_Free(&pool, storage.bytes + 1) == -1);
    assert(Kernel_MemoryPool_Free(&pool, NULL) == -1);

    void *reused = Kernel_MemoryPool_Alloc(&pool);
    assert(reused == blocks[1]);
    assert(Kernel_MemoryPool_GetFreeCount(&pool) == 0);

    for (size_t i = 0; i < BLOCK_COUNT; ++i)
    {
        assert(Kernel_MemoryPool_Free(&pool, blocks[i]) == 0);
    }

    assert(Kernel_MemoryPool_GetFreeCount(&pool) == BLOCK_COUNT);

    assert(critical_enter_count == critical_exit_count);
    assert(critical_enter_count > 1U);
    assert(last_critical_state ==
           (KernelMemoryPoolCriticalState)0x5AU);

    /* Test the maximum pool size, including bitmap bit 31. */
    union
    {
        max_align_t alignment;
        unsigned char bytes[BLOCK_SIZE * KERNEL_MEMORY_POOL_MAX_BLOCKS];
    } max_storage;

    KernelMemoryPool max_pool = {0};
    void *max_blocks[KERNEL_MEMORY_POOL_MAX_BLOCKS];

    assert(Kernel_MemoryPool_Init(
        &max_pool,
        max_storage.bytes,
        BLOCK_SIZE,
        KERNEL_MEMORY_POOL_MAX_BLOCKS) == 0);

    assert(Kernel_MemoryPool_GetFreeCount(&max_pool) ==
           KERNEL_MEMORY_POOL_MAX_BLOCKS);

    for (size_t i = 0; i < KERNEL_MEMORY_POOL_MAX_BLOCKS; ++i)
    {
        max_blocks[i] = Kernel_MemoryPool_Alloc(&max_pool);
        assert(max_blocks[i] != NULL);

        for (size_t j = 0; j < i; ++j)
        {
            assert(max_blocks[i] != max_blocks[j]);
        }
    }

    assert(Kernel_MemoryPool_Alloc(&max_pool) == NULL);
    assert(Kernel_MemoryPool_GetFreeCount(&max_pool) == 0U);

    /* Free and reuse the final block, corresponding to bitmap bit 31. */
    size_t last = KERNEL_MEMORY_POOL_MAX_BLOCKS - 1U;

    assert(Kernel_MemoryPool_Free(&max_pool, max_blocks[last]) == 0);
    assert(Kernel_MemoryPool_GetFreeCount(&max_pool) == 1U);
    assert(Kernel_MemoryPool_Alloc(&max_pool) == max_blocks[last]);

    for (size_t i = 0; i < KERNEL_MEMORY_POOL_MAX_BLOCKS; ++i)
    {
        assert(Kernel_MemoryPool_Free(&max_pool, max_blocks[i]) == 0);
    }

    assert(Kernel_MemoryPool_GetFreeCount(&max_pool) ==
           KERNEL_MEMORY_POOL_MAX_BLOCKS);


    /* Reinitialization resets allocation state and callbacks. */
    assert(Kernel_MemoryPool_SetCriticalSection(
        &max_pool, TestEnterCritical, TestExitCritical) == 0);

    void *pre_reinit = Kernel_MemoryPool_Alloc(&max_pool);
    assert(pre_reinit == max_blocks[0]);

    assert(Kernel_MemoryPool_Init(
        &max_pool,
        max_storage.bytes,
        BLOCK_SIZE,
        KERNEL_MEMORY_POOL_MAX_BLOCKS) == 0);

    assert(max_pool.enter_critical == NULL);
    assert(max_pool.exit_critical == NULL);
    assert(Kernel_MemoryPool_GetFreeCount(&max_pool) ==
           KERNEL_MEMORY_POOL_MAX_BLOCKS);

    assert(Kernel_MemoryPool_Alloc(&max_pool) == max_blocks[0]);
    assert(Kernel_MemoryPool_Free(&max_pool, max_blocks[0]) == 0);
    assert(Kernel_MemoryPool_GetFreeCount(&max_pool) ==
           KERNEL_MEMORY_POOL_MAX_BLOCKS);

    puts("PASS: allocation, exhaustion, reuse, invalid-free,");
    puts("      and critical-section callback tests");
    puts("      double-free, and free-count tests");

    return 0;
}
