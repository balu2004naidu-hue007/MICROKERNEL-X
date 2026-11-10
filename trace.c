#include <stdio.h>
#include "trace.h"
#include "scheduler.h"

static TraceEvent trace_buffer[TRACE_BUFFER_SIZE];

static unsigned int trace_write_index = 0;
static unsigned int trace_count = 0;

void Kernel_Trace_Init(void)
{
    trace_write_index = 0;
    trace_count = 0;

    for (unsigned int i = 0; i < TRACE_BUFFER_SIZE; i++)
    {
        trace_buffer[i].tick = 0;
        trace_buffer[i].type = TRACE_TASK_SCHEDULED;
        trace_buffer[i].task_name = NULL;
    }
}

const char* Kernel_Trace_GetEventName(TraceEventType type)
{
    switch (type)
    {
        case TRACE_TASK_SCHEDULED:
            return "TASK_SCHEDULED";

        case TRACE_CONTEXT_SWITCH:
            return "CONTEXT_SWITCH";

        case TRACE_TASK_PROGRESS:
            return "TASK_PROGRESS";

        case TRACE_TASK_FAULT:
            return "TASK_FAULT";

        case TRACE_RECOVERY_START:
            return "RECOVERY_START";

        case TRACE_RECOVERY_SUCCESS:
            return "RECOVERY_SUCCESS";

        case TRACE_RECOVERY_FAILED:
            return "RECOVERY_FAILED";

        case TRACE_TASK_ISOLATED:
            return "TASK_ISOLATED";

        case TRACE_IPC_SEND:
            return "IPC_SEND";

        case TRACE_IPC_RECEIVE:
            return "IPC_RECEIVE";

        case TRACE_MUTEX_LOCK:
            return "MUTEX_LOCK";

        case TRACE_MUTEX_WAIT:
            return "MUTEX_WAIT";

        case TRACE_MUTEX_UNLOCK:
            return "MUTEX_UNLOCK";

        case TRACE_PRIORITY_INHERIT:
    return "PRIORITY_INHERIT";

case TRACE_PRIORITY_RESTORE:
    return "PRIORITY_RESTORE";


        default:
            return "UNKNOWN";
    }
}

void Kernel_Trace(TraceEventType type,
                  TaskControlBlock* task)
{
    TraceEvent* event;

    event = &trace_buffer[trace_write_index];

    event->tick = Scheduler_GetTick();
    event->type = type;

    if (task != NULL)
        event->task_name = task->name;
    else
        event->task_name = "SYSTEM";

    trace_write_index++;

    if (trace_write_index >= TRACE_BUFFER_SIZE)
        trace_write_index = 0;

    if (trace_count < TRACE_BUFFER_SIZE)
        trace_count++;
}

unsigned int Kernel_Trace_Copy(TraceEvent* destination,
                               unsigned int capacity)
{
    if (destination == NULL || capacity == 0U || trace_count == 0U)
    {
        return 0U;
    }

    unsigned int copy_count = trace_count;
    if (copy_count > capacity)
    {
        copy_count = capacity;
    }

    unsigned int start =
        (trace_count < TRACE_BUFFER_SIZE) ? 0U : trace_write_index;
    unsigned int skipped = trace_count - copy_count;
    start = (start + skipped) % TRACE_BUFFER_SIZE;

    for (unsigned int i = 0U; i < copy_count; ++i)
    {
        destination[i] = trace_buffer[(start + i) % TRACE_BUFFER_SIZE];
    }

    return copy_count;
}

void Kernel_Trace_Print(void)
{
    unsigned int start;
    unsigned int index;

    printf("\n========== KERNEL TRACE ==========\n");

    if (trace_count == 0)
    {
        printf("Trace buffer is empty.\n");
        printf("==================================\n");
        return;
    }

    if (trace_count < TRACE_BUFFER_SIZE)
        start = 0;
    else
        start = trace_write_index;

    for (unsigned int i = 0; i < trace_count; i++)
    {
        index = (start + i) % TRACE_BUFFER_SIZE;

        printf("[TRACE] Tick %-4lu | %-18s | %s\n",
               trace_buffer[index].tick,
               Kernel_Trace_GetEventName(trace_buffer[index].type),
               trace_buffer[index].task_name);
    }

    printf("==================================\n");
}
