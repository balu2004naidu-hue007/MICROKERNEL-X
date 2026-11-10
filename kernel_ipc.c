#include <stddef.h>
#include "kernel_ipc.h"
#include "scheduler.h"
#include "kernel_critical.h"
#include "trace.h"

static IPC_MessageQueue kernel_queue;

static TaskControlBlock* waiting_tasks[IPC_MAX_WAITERS];
static int waiting_task_count = 0;

void Kernel_IPC_Init(void)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    IPC_Init(&kernel_queue);

    waiting_task_count = 0;

    for (int i = 0; i < IPC_MAX_WAITERS; i++)
    {
        waiting_tasks[i] = NULL;
    }

    Kernel_CriticalExit(state);
}

int Kernel_IPC_Send(const char* message)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = IPC_Send(&kernel_queue, message);

    if (result == 0)
    {
        Kernel_Trace(TRACE_IPC_SEND, NULL);

        for (int i = 0; i < waiting_task_count; i++)
        {
            if (waiting_tasks[i] != NULL &&
                waiting_tasks[i]->state == TASK_WAITING)
            {
                Kernel_IPC_Wake(waiting_tasks[i]);
                break;
            }
        }
    }

    Kernel_CriticalExit(state);

    return result;
}

int Kernel_IPC_Receive(char* buffer, size_t buffer_size)
{
    if (buffer == NULL)
    {
        return -1;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    int result = IPC_Receive(
        &kernel_queue,
        buffer,
        buffer_size);

    Kernel_CriticalExit(state);

    if (result == 0)
    {
        Kernel_Trace(TRACE_IPC_RECEIVE, NULL);
    }

    return result;
}

int Kernel_IPC_GetCount(void)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = IPC_GetCount(&kernel_queue);

    Kernel_CriticalExit(state);

    return result;
}

int Kernel_IPC_GetWaitingCount(void)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = waiting_task_count;

    Kernel_CriticalExit(state);

    return result;
}

void Kernel_IPC_RegisterReceiver(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    if (task->state != TASK_RUNNING &&
        task->state != TASK_WAITING)
    {
        Kernel_CriticalExit(state);
        return;
    }

    if (waiting_task_count >= IPC_MAX_WAITERS)
    {
        Kernel_CriticalExit(state);
        return;
    }

    for (int i = 0; i < waiting_task_count; i++)
    {
        if (waiting_tasks[i] == task)
        {
            Kernel_CriticalExit(state);
            return;
        }
    }

    waiting_tasks[waiting_task_count] = task;
    waiting_task_count++;

    Kernel_CriticalExit(state);
}

void Kernel_IPC_RemoveReceiver(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    for (int i = 0; i < waiting_task_count; i++)
    {
        if (waiting_tasks[i] == task)
        {
            for (int j = i; j < waiting_task_count - 1; j++)
            {
                waiting_tasks[j] = waiting_tasks[j + 1];
            }

            waiting_tasks[waiting_task_count - 1] = NULL;
            waiting_task_count--;

            break;
        }
    }

    Kernel_CriticalExit(state);
}

void Kernel_IPC_ClearWakeReason(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    task->wake_reason = WAKE_NONE;

    Kernel_CriticalExit(state);
}

void Kernel_IPC_CleanupTask(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    Kernel_IPC_RemoveReceiver(task);
    Kernel_IPC_ClearWakeReason(task);
    task->ipc_timeout_tick = 0;

    /* Recovery controls the task-state transition. */

    Kernel_CriticalExit(state);
}

int Kernel_IPC_IsEmpty(void)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = IPC_IsEmpty(&kernel_queue);

    Kernel_CriticalExit(state);

    return result;
}

void Kernel_IPC_Wait(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    int receiver_index = -1;

    for (int i = 0; i < waiting_task_count; i++)
    {
        if (waiting_tasks[i] == task)
        {
            receiver_index = i;
            break;
        }
    }

    /*
     * A message arrived after registration but
     * before this task entered the waiting state.
     */
    if (IPC_GetCount(&kernel_queue) > 0 &&
        receiver_index >= 0)
    {
        task->wake_reason = WAKE_IPC;
        task->ipc_timeout_tick = 0;

        for (int i = receiver_index;
             i < waiting_task_count - 1;
             i++)
        {
            waiting_tasks[i] = waiting_tasks[i + 1];
        }

        waiting_tasks[waiting_task_count - 1] = NULL;
        waiting_task_count--;

        Kernel_CriticalExit(state);
        return;
    }

    task->wake_reason = WAKE_NONE;
    task->state = TASK_WAITING;

    task->ipc_timeout_tick =
        Scheduler_GetTick() + Kernel_IPC_GetDefaultTimeout();

    Kernel_CriticalExit(state);
}

void Kernel_IPC_Wake(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    if (task->state != TASK_WAITING)
    {
        Kernel_CriticalExit(state);
        return;
    }

    task->state = TASK_READY;
    task->wake_reason = WAKE_IPC;
    task->ipc_timeout_tick = 0;

    for (int i = 0; i < waiting_task_count; i++)
    {
        if (waiting_tasks[i] == task)
        {
            for (int j = i; j < waiting_task_count - 1; j++)
            {
                waiting_tasks[j] = waiting_tasks[j + 1];
            }

            waiting_tasks[waiting_task_count - 1] = NULL;
            waiting_task_count--;

            break;
        }
    }

    Kernel_CriticalExit(state);
}

TaskControlBlock* Kernel_IPC_GetWaitingTask(void)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    TaskControlBlock* task = NULL;

    if (waiting_task_count > 0)
    {
        task = waiting_tasks[0];
    }

    Kernel_CriticalExit(state);

    return task;
}

unsigned long Kernel_IPC_GetDefaultTimeout(void)
{
    return IPC_DEFAULT_TIMEOUT_TICKS;
}

unsigned long Kernel_IPC_GetTaskTimeout(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return 0;
    }

    KernelCriticalState state = Kernel_CriticalEnter();

    unsigned long timeout = task->ipc_timeout_tick;

    Kernel_CriticalExit(state);

    return timeout;
}
