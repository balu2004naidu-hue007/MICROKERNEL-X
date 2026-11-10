#include <stdint.h>
#include "trace.h"
#include "scheduler.h"

/*
 * MICROKERNEL-X ARM trace backend.
 *
 * Fixed-size ring buffer.
 * No printf, dynamic allocation, or host runtime dependency.
 * Buffer and counters are exposed for GDB inspection.
 */

TraceEvent arm_trace_buffer[TRACE_BUFFER_SIZE];

volatile unsigned int arm_trace_write_index = 0U;
volatile unsigned int arm_trace_count = 0U;

static uint32_t ARM_Trace_EnterCritical(void)
{
    uint32_t previous_primask;

    __asm volatile (
        "mrs %0, primask\n"
        "cpsid i"
        : "=r" (previous_primask)
        :
        : "memory"
    );

    return previous_primask;
}

static void ARM_Trace_ExitCritical(uint32_t previous_primask)
{
    __asm volatile (
        "msr primask, %0"
        :
        : "r" (previous_primask)
        : "memory"
    );
}

void Kernel_Trace_Init(void)
{
    uint32_t previous_primask = ARM_Trace_EnterCritical();

    arm_trace_write_index = 0U;
    arm_trace_count = 0U;

    for (unsigned int i = 0U; i < TRACE_BUFFER_SIZE; i++)
    {
        arm_trace_buffer[i].tick = 0UL;
        arm_trace_buffer[i].type = TRACE_TASK_SCHEDULED;
        arm_trace_buffer[i].task_name = "SYSTEM";
    }

    ARM_Trace_ExitCritical(previous_primask);
}

void Kernel_Trace(TraceEventType type, TaskControlBlock *task)
{
    uint32_t previous_primask = ARM_Trace_EnterCritical();
    unsigned int index = arm_trace_write_index;

    arm_trace_buffer[index].tick = Scheduler_GetTick();
    arm_trace_buffer[index].type = type;
    arm_trace_buffer[index].task_name =
        (task != NULL && task->name != NULL)
            ? task->name
            : "SYSTEM";

    arm_trace_write_index =
        (index + 1U) % TRACE_BUFFER_SIZE;

    if (arm_trace_count < TRACE_BUFFER_SIZE)
    {
        arm_trace_count++;
    }

    ARM_Trace_ExitCritical(previous_primask);
}

unsigned int Kernel_Trace_Copy(TraceEvent *destination,
                               unsigned int capacity)
{
    if (destination == 0 || capacity == 0U)
    {
        return 0U;
    }

    uint32_t previous_primask = ARM_Trace_EnterCritical();
    unsigned int copy_count = arm_trace_count;
    if (copy_count > capacity)
    {
        copy_count = capacity;
    }

    unsigned int start =
        (arm_trace_count < TRACE_BUFFER_SIZE) ? 0U : arm_trace_write_index;
    unsigned int skipped = arm_trace_count - copy_count;
    start = (start + skipped) % TRACE_BUFFER_SIZE;

    for (unsigned int i = 0U; i < copy_count; ++i)
    {
        destination[i] = arm_trace_buffer[(start + i) % TRACE_BUFFER_SIZE];
    }

    ARM_Trace_ExitCritical(previous_primask);
    return copy_count;
}

const char *Kernel_Trace_GetEventName(TraceEventType type)
{
    switch (type)
    {
        case TRACE_TASK_SCHEDULED:  return "TASK_SCHEDULED";
        case TRACE_CONTEXT_SWITCH:  return "CONTEXT_SWITCH";
        case TRACE_TASK_PROGRESS:   return "TASK_PROGRESS";
        case TRACE_TASK_FAULT:      return "TASK_FAULT";
        case TRACE_RECOVERY_START:  return "RECOVERY_START";
        case TRACE_RECOVERY_SUCCESS:return "RECOVERY_SUCCESS";
        case TRACE_RECOVERY_FAILED: return "RECOVERY_FAILED";
        case TRACE_TASK_ISOLATED:   return "TASK_ISOLATED";
        case TRACE_IPC_SEND:        return "IPC_SEND";
        case TRACE_IPC_RECEIVE:     return "IPC_RECEIVE";
        case TRACE_MUTEX_LOCK:      return "MUTEX_LOCK";
        case TRACE_MUTEX_WAIT:      return "MUTEX_WAIT";
        case TRACE_MUTEX_UNLOCK:    return "MUTEX_UNLOCK";
        case TRACE_PRIORITY_INHERIT:return "PRIORITY_INHERIT";
        case TRACE_PRIORITY_RESTORE:return "PRIORITY_RESTORE";
        default:                    return "UNKNOWN";
    }
}

/* The ARM demo has no console backend. Inspect the buffer through GDB. */
void Kernel_Trace_Print(void)
{
    /* Intentionally empty for the freestanding ARM target. */
}