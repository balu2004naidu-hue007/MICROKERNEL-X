#include <stddef.h>
#include "task.h"
#define TASK_STACK_GUARD_WORDS 4U
#define TASK_STACK_WARNING_PERCENT 75U
#define TASK_STACK_WARNING_THRESHOLD_BYTES \
    (((TASK_STACK_SIZE - TASK_STACK_GUARD_WORDS) * sizeof(uint32_t) * \
      TASK_STACK_WARNING_PERCENT) / 100U)
#define TASK_STACK_GUARD_PATTERN 0xC0DEFACEUL
#define TASK_STACK_FILL_PATTERN 0xA5A5A5A5UL

void Task_Init(TaskControlBlock* task, int id, int priority, const char* name, void (*task_function)(TaskControlBlock*))
{
  task->id= id;

/* Fill the stack so unused space can be measured later. */
for (size_t i = 0; i < TASK_STACK_SIZE; ++i)
{
    task->stack[i] = TASK_STACK_FILL_PATTERN;
}

/* Reserve the lowest stack words for the guard. */
for (size_t i = 0; i < TASK_STACK_GUARD_WORDS; ++i)
{
    task->stack[i] = TASK_STACK_GUARD_PATTERN;
}

task->context.sp =
    (uintptr_t)&task->stack[TASK_STACK_SIZE - 16];
task->context.lr = 0;
task->context.pc = (uintptr_t)task_function;
task->context.xpsr = 0x01000000UL;
  task->base_priority = priority;
  task->priority= priority;
  task->name= name;
  task->state= TASK_READY;
  task->wake_reason = WAKE_NONE;
  task->is_idle = 0;
  task->run_count = 0;
  task->restart_count = 0;
  task->recovery_pending = 0;
  task->recovery_in_progress = 0;
  task->fault_type = FAULT_NONE;
  task->fault_count = 0;
  task->recovery_attempts = 0;
  task->successful_recoveries = 0;
  task->recovery_failed = 0;
  task->isolated = 0;
  task->fault_injection = FAULT_INJECT_NONE;
  task->fault_injection_enabled = 0;
  task->recovery_failure_injection_enabled = 0;
  task->execution_units = 0;
  task->watchdog_last_execution_units = 0;
  task->progress_count = 0;
  task->mutex_hold_active = 0;
  task->mutex_hold_phase = 0;
  task->mutex_waiting = 0;
  task->waiting_mutex = NULL;
  task->owned_mutexes = NULL;
  task->recovery_start_progress_count = 0;
  task->last_progress_tick = 0;
  task->last_heartbeat_tick = 0;
  task->last_progress_count = 0;
  task->progress_enabled = 1;
  task->progress_reported = 0;
  task->time_slice = DEFAULT_TIME_SLICE;
  task->wakeup_tick= 0;
  task->ipc_timeout_tick = 0;
  task->context.lr = 0;
  task->context.pc = (uintptr_t)task_function;
  task->context.xpsr = 0x01000000UL;


/* Software-saved registers: R4-R11 */
task->stack[TASK_STACK_SIZE - 16] = 0; /* R4 */
task->stack[TASK_STACK_SIZE - 15] = 0; /* R5 */
task->stack[TASK_STACK_SIZE - 14] = 0; /* R6 */
task->stack[TASK_STACK_SIZE - 13] = 0; /* R7 */
task->stack[TASK_STACK_SIZE - 12] = 0; /* R8 */
task->stack[TASK_STACK_SIZE - 11] = 0; /* R9 */
task->stack[TASK_STACK_SIZE - 10] = 0; /* R10 */
task->stack[TASK_STACK_SIZE - 9]  = 0; /* R11 */

/* Hardware exception frame: R0-R3, R12, LR, PC, xPSR */
task->stack[TASK_STACK_SIZE - 8] = 0; /* R0 */
task->stack[TASK_STACK_SIZE - 7] = 0; /* R1 */
task->stack[TASK_STACK_SIZE - 6] = 0; /* R2 */
task->stack[TASK_STACK_SIZE - 5] = 0; /* R3 */
task->stack[TASK_STACK_SIZE - 4] = 0; /* R12 */
task->stack[TASK_STACK_SIZE - 3] = 0; /* LR */
task->stack[TASK_STACK_SIZE - 2] =
    (uint32_t)(uintptr_t)task_function; /* PC */
task->stack[TASK_STACK_SIZE - 1] = 0x01000000UL; /* xPSR */

task->stack_low_watermark = task->context.sp;
task->task_function = task_function;
task->stack_warning_reported = 0;
task->initial_stack_argument = 0;
task->initial_stack_return_handler = 0;
task->yield_requested = 0;

}

int Task_RestartContext(TaskControlBlock* task)
{
    if (task == NULL || task->task_function == NULL)
    {
        return -1;
    }

    /* Refill the stack before rebuilding its initial frame. */
    for (size_t i = 0; i < TASK_STACK_SIZE; ++i)
    {
        task->stack[i] = TASK_STACK_FILL_PATTERN;
    }

    for (size_t i = 0; i < TASK_STACK_GUARD_WORDS; ++i)
    {
        task->stack[i] = TASK_STACK_GUARD_PATTERN;
    }

    /* Rebuild R4-R11 and the hardware exception frame. */
    task->context.sp =
        (uintptr_t)&task->stack[TASK_STACK_SIZE - 16];
    task->context.lr = 0;
    task->context.pc = (uintptr_t)task->task_function;
    task->context.xpsr = 0x01000000UL;

    for (size_t i = TASK_STACK_SIZE - 16;
         i < TASK_STACK_SIZE; ++i)
    {
        task->stack[i] = 0U;
    }

    task->stack[TASK_STACK_SIZE - 8] =
        (uint32_t)task->initial_stack_argument;
    task->stack[TASK_STACK_SIZE - 3] =
        (uint32_t)task->initial_stack_return_handler;
    task->stack[TASK_STACK_SIZE - 2] =
        (uint32_t)(uintptr_t)task->task_function;
    task->stack[TASK_STACK_SIZE - 1] = 0x01000000UL;

    task->stack_low_watermark = task->context.sp;
    task->stack_warning_reported = 0;

    return Task_ValidateContext(task);
}

int Task_ValidateContext(const TaskControlBlock* task)
{
    if (task == NULL)
    {
        return -1;
    }

    if (task->context.sp == 0)
    {
        return -2;
    }

    if (task->context.pc == 0)
    {
        return -3;
    }

    if (task->context.xpsr != 0x01000000UL)
    {
        return -4;
    }

    const uintptr_t stack_base =
        (uintptr_t)&task->stack[0];

    const uintptr_t stack_end =
        (uintptr_t)&task->stack[TASK_STACK_SIZE];

    const uintptr_t saved_sp =
        task->context.sp;

    const uintptr_t guard_bytes =
        TASK_STACK_GUARD_WORDS * sizeof(uint32_t);

    const uintptr_t frame_bytes =
        16U * sizeof(uint32_t);

    /* Keep the saved stack pointer above the reserved guard. */
    if (saved_sp < stack_base + guard_bytes ||
        saved_sp > stack_end - frame_bytes)
    {
        return -5;
    }

    if ((saved_sp & 0x7U) != 0U)
    {
        return -6;
    }

    /* Detect corruption of the reserved stack-guard words. */
    for (size_t i = 0; i < TASK_STACK_GUARD_WORDS; ++i)
    {
        if (task->stack[i] != TASK_STACK_GUARD_PATTERN)
        {
            return -7;
        }
    }

    return 0;
}

size_t Task_GetStackHighWaterUsage(const TaskControlBlock* task)
{
    if (task == NULL || Task_ValidateContext(task) != 0)
    {
        return 0U;
    }

    const uintptr_t stack_base =
        (uintptr_t)&task->stack[0];

    const uintptr_t stack_end =
        (uintptr_t)&task->stack[TASK_STACK_SIZE];

    const uintptr_t guard_end =
        stack_base + TASK_STACK_GUARD_WORDS * sizeof(uint32_t);

    const uintptr_t low_watermark =
        task->stack_low_watermark;

    if (low_watermark < guard_end ||
        low_watermark > stack_end ||
        (low_watermark & 0x7U) != 0U)
    {
        return 0U;
    }

    /* Estimate usage from the deepest saved stack pointer. */
    size_t watermark_usage =
        (size_t)(stack_end - low_watermark);

    /* Also scan the fill pattern for overwritten stack words. */
    size_t first_used_word = TASK_STACK_GUARD_WORDS;

    while (first_used_word < TASK_STACK_SIZE &&
           task->stack[first_used_word] == TASK_STACK_FILL_PATTERN)
    {
        first_used_word++;
    }

    size_t fill_usage =
        (TASK_STACK_SIZE - first_used_word) * sizeof(uint32_t);

    /* Return whichever method reports greater usage. */
    return (watermark_usage > fill_usage)
        ? watermark_usage
        : fill_usage;
}

void Task_UpdateStackHighWater(TaskControlBlock* task)
{
    if (task == NULL || Task_ValidateContext(task) != 0)
    {
        return;
    }

    uintptr_t saved_sp = task->context.sp;

    if (task->stack_low_watermark == 0U ||
        saved_sp < task->stack_low_watermark)
    {
        task->stack_low_watermark = saved_sp;
    }
}

int Task_IsStackUsageWarning(const TaskControlBlock* task)
{
    if (task == NULL || Task_ValidateContext(task) != 0)
    {
        return -1;
    }

    size_t usage = Task_GetStackHighWaterUsage(task);
    if (usage == 0U)
    {
        return -1;
    }

    return (usage >= TASK_STACK_WARNING_THRESHOLD_BYTES) ? 1 : 0;
}
