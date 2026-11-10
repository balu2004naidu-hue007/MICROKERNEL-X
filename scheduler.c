#include <stdio.h>
#include <stddef.h>
#include "scheduler.h"
#include "kernel_critical.h"
#include "kernel_ipc.h"
#include "trace.h"
#include "mutex.h"

#ifdef MICROKERNEL_ARM
#define printf(...) ((void)0)
#endif
#ifndef MICROKERNEL_ARM
#include <time.h>
#endif

static volatile unsigned long system_tick = 0;

#ifndef MICROKERNEL_ARM

static int Scheduler_ReadMonotonicNs(uint64_t* result)
{
    struct timespec ts;

    if (result == NULL ||
        clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0;
    }

    *result = ((uint64_t)ts.tv_sec * 1000000000ULL)
              + (uint64_t)ts.tv_nsec;

    return 1;
}

static int Scheduler_ReadProcessCpuNs(uint64_t* result)
{
    if (result == NULL)
    {
        return 0;
    }

    clock_t cpu_ticks = clock();

    if (cpu_ticks == (clock_t)-1 || CLOCKS_PER_SEC <= 0)
    {
        return 0;
    }

    *result = ((uint64_t)cpu_ticks * 1000000000ULL)
              / (uint64_t)CLOCKS_PER_SEC;

    return 1;
}


#endif

void Scheduler_Init(TaskRegistry* registry)
{
   if (registry == NULL)
    {
        return;
    }

    registry->task_count = 0;
    registry->idle_task = NULL;
    registry->last_selected_index = -1;
    registry->scheduler_decisions = 0;
    registry->context_switches = 0;
    registry->recovery_events = 0;
    registry->execution_cycles = 0;
    registry->task_function_elapsed_ns = 0;
    registry->idle_function_elapsed_ns = 0;
    registry->scheduler_elapsed_ns = 0;
    registry->kernel_wall_ns = 0;
    registry->kernel_cpu_ns = 0;
    registry->kernel_run_ticks = 0;
    registry->kernel_run_execution_cycles = 0;
    registry->current_task = NULL;
    registry->reschedule_pending = 0;
    registry->reschedule_reason = RESCHEDULE_NONE;
    registry->time_slice_expired = 0;
}

int Scheduler_AddTask(TaskRegistry* registry, TaskControlBlock* task)
{
    if (registry == NULL || task == NULL)
    {
        return -1;
    }

    KernelCriticalState critical_state = Kernel_CriticalEnter();

    int result = -1;

    if (task->task_function != NULL &&
        task->priority >= 0 &&
        registry->task_count < MAX_TASKS)
    {
        int duplicate = 0;

        for (int i = 0; i < registry->task_count; i++)
        {
            if (registry->tasks[i] == task)
            {
                duplicate = 1;
                break;
            }
        }

        if (!duplicate)
        {
            /* Initialize the TCB before publishing it in the registry. */
            task->state = TASK_READY;

            registry->tasks[registry->task_count] = task;
            registry->task_count++;

            result = 0;
        }
    }

    Kernel_CriticalExit(critical_state);

    return result;
}

TaskControlBlock* Scheduler_SelectNext(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return NULL;
    }

    TaskControlBlock* best_task = NULL;
    registry->scheduler_decisions++;

    if (registry->current_task != NULL &&
        registry->current_task->state == TASK_RUNNING &&
        !registry->time_slice_expired &&
        !registry->reschedule_pending)
    {
       return registry->current_task;
    }

if (registry->reschedule_pending &&
    registry->current_task != NULL &&
    !registry->current_task->is_idle)
{
    #ifndef MICROKERNEL_ARM
    printf("Scheduler: Rescheduling from %s | Reason: %s\n",
           registry->current_task->name,
           Scheduler_GetRescheduleReasonName(
               registry->reschedule_reason));
    #endif

    if (registry->current_task->state == TASK_RUNNING)
    {
        registry->current_task->state = TASK_READY;
    }
}

    int start_index = registry->last_selected_index + 1;

    if (start_index >= registry->task_count)
    {
        start_index = 0;
    }

    for (int offset = 0; offset < registry->task_count; offset++)
    {
        int i = (start_index + offset) % registry->task_count;

        TaskControlBlock* current_task = registry->tasks[i];

        if (current_task == NULL)
        {
            continue;
        }

        if (current_task == registry->idle_task)
        {
            continue;
        }

        if (current_task->state == TASK_FAULTED ||
    current_task->isolated)
{
    continue;
}

        if (current_task->state != TASK_READY)
{
    continue;
}


if (best_task == NULL ||
    current_task->priority > best_task->priority)
{
    best_task = current_task;
}

 }

    if (best_task != NULL)
    {
        if (best_task->state == TASK_READY)
        {
#ifndef MICROKERNEL_ARM
            printf("Scheduler: Selecting ready task -> %s\n",
                   best_task->name);
#endif
            Kernel_Trace(TRACE_TASK_SCHEDULED, best_task);
        }

        if (registry->current_task != NULL &&
            registry->current_task != best_task)
        {
            registry->context_switches++;
            Kernel_Trace(TRACE_CONTEXT_SWITCH, best_task);
        }

        registry->current_task = best_task;

        if (best_task->time_slice == 0)
    {
        best_task->time_slice = DEFAULT_TIME_SLICE;
    }

        best_task->state = TASK_RUNNING;

        for (int i = 0; i < registry->task_count; i++)
        {
            if (registry->tasks[i] == best_task)
            {
                registry->last_selected_index = i;
                break;
            }
        }

        return best_task;
    }

    if (registry->idle_task != NULL)
    {
        if (registry->current_task != NULL &&
        registry->current_task != registry->idle_task)
    {
        registry->context_switches++;
    }

        registry->current_task = registry->idle_task;

        registry->idle_task->state = TASK_RUNNING;
        return registry->idle_task;
    }

    return NULL;
}


void Scheduler_HandleTaskReturn(
    TaskRegistry* registry,
    TaskControlBlock* task)
{
    if (registry == NULL || task == NULL)
    {
        return;
    }

    int task_yielded = 0;
    int task_blocked = 0;

    KernelCriticalState critical_state = Kernel_CriticalEnter();

    if (task->yield_requested)
    {
        task->yield_requested = 0;

        if (task->state == TASK_RUNNING)
        {
            task->state = TASK_READY;
        }

        registry->reschedule_pending = 1;
        registry->reschedule_reason = RESCHEDULE_YIELD;
        task_yielded = 1;
    }

    if (task->state == TASK_BLOCKED)
    {
        task->time_slice = DEFAULT_TIME_SLICE;
        registry->reschedule_pending = 1;
        registry->reschedule_reason = RESCHEDULE_BLOCK;
        task_blocked = 1;
    }

    if (task->state == TASK_RUNNING && task->time_slice == 0)
    {
        task->state = TASK_READY;
        task->time_slice = DEFAULT_TIME_SLICE;
    }

    Kernel_CriticalExit(critical_state);

    if (task_yielded)
    {
        printf("Scheduler: Task yield -> %s\n", task->name);
        printf("Scheduler: Reschedule reason set -> %s\n",
               Scheduler_GetRescheduleReasonName(RESCHEDULE_YIELD));
    }

    if (task_blocked)
    {
        printf("Scheduler: Task blocked -> %s\n", task->name);
        printf("Scheduler: Reschedule reason set -> %s\n",
               Scheduler_GetRescheduleReasonName(RESCHEDULE_BLOCK));
    }
}

void Scheduler_Dispatch(TaskRegistry* registry)
{
    if (registry == NULL || registry->current_task == NULL)
    {
        return;
    }

    TaskControlBlock* task = registry->current_task;

    if (task->task_function == NULL || task->state != TASK_RUNNING)
    {
        return;
    }

        task->run_count++;

#ifndef MICROKERNEL_ARM
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;

    int start_valid = Scheduler_ReadMonotonicNs(&start_ns);
    task->task_function(task);
    int end_valid = Scheduler_ReadMonotonicNs(&end_ns);

    if (start_valid && end_valid && end_ns >= start_ns)
    {
        uint64_t elapsed_ns = end_ns - start_ns;

        if (task == registry->idle_task || task->is_idle)
        {
            registry->idle_function_elapsed_ns += elapsed_ns;
        }
        else
        {
            registry->task_function_elapsed_ns += elapsed_ns;
        }
    }
#else
    task->task_function(task);
#endif

    Scheduler_HandleTaskReturn(registry, task);
}

void Scheduler_Yield(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    KernelCriticalState critical_state = Kernel_CriticalEnter();

    if (task->state != TASK_RUNNING)
    {
        Kernel_CriticalExit(critical_state);
        return;
    }

    task->yield_requested = 1;

    Kernel_CriticalExit(critical_state);
}

void Scheduler_Block(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    if (task->is_idle)
    {
        return;
    }

    task->state = TASK_BLOCKED;

    printf("Scheduler: Task blocked -> %s\n",
           task->name);
}

void Scheduler_Unblock(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    task->state = TASK_READY;
}

unsigned long Scheduler_GetTick(void)
{
    return system_tick;
}

void Scheduler_AdvanceTick(void)
{
    system_tick++;
}

int Scheduler_GetTaskCount(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return 0;
    }

    return registry->task_count;
}

unsigned long Scheduler_GetDecisionCount(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return 0;
    }

    return registry->scheduler_decisions;
}

TaskControlBlock* Scheduler_GetCurrentTask(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return NULL;
    }

    return registry->current_task;
}

TaskState Scheduler_GetTaskState(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return TASK_BLOCKED;
    }

    return task->state;
}

void Scheduler_HandleTimeSlice(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    if (task->state == TASK_RUNNING &&
        task->time_slice > 0)
    {
        task->time_slice--;

        printf("Time slice: %s -> %lu\n",
       task->name,
       task->time_slice);

        if (task->time_slice == 0)
        {
            task->state = TASK_READY;

            printf("Time slice expired: %s\n", task->name);
        }
    }
}

void Scheduler_Tick(TaskRegistry* registry)
{
     if (registry == NULL)
    {
        return;
    }
    registry->time_slice_expired = 0;

    system_tick++;
    Scheduler_UpdateBlockedTasks(registry);
    Scheduler_CheckPreemption(registry);

    if (registry->current_task != NULL &&
    registry->current_task->state == TASK_RUNNING)
{
    unsigned long previous_time_slice =
        registry->current_task->time_slice;

    Scheduler_HandleTimeSlice(registry->current_task);

    if (previous_time_slice > 0 &&
        registry->current_task->time_slice == 0 &&
        registry->current_task->state == TASK_READY)
    {
        registry->time_slice_expired = 1;
    }
}

    if (registry->time_slice_expired)
    {
        registry->reschedule_pending = 1;
        registry->reschedule_reason = RESCHEDULE_TIMESLICE;
    }
}

void Scheduler_Delay(TaskControlBlock* task, unsigned long ticks)
{
    if (task == NULL)
    {
        return;
    }

    if (task->is_idle)
    {
        return;
    }

    if (ticks == 0)
    {
        return;
    }

    KernelCriticalState critical_state = Kernel_CriticalEnter();

    task->wakeup_tick = system_tick + ticks;
    task->state = TASK_BLOCKED;

    Kernel_CriticalExit(critical_state);
}

void Scheduler_UpdateBlockedTasks(TaskRegistry* registry)
{

    if (registry == NULL)
    {
        return;
    }

    for (int i = 0; i < registry->task_count; i++)
    {
        TaskControlBlock* task = registry->tasks[i];
          if (task == NULL)
          {
              continue;
          }

            if (task->state == TASK_WAITING &&
            task->ipc_timeout_tick != 0 &&
            system_tick >= task->ipc_timeout_tick)
        {
            task->state = TASK_READY;
            task->wake_reason = WAKE_TIMEOUT;
            task->ipc_timeout_tick = 0;
            Kernel_IPC_RemoveReceiver(task);

#ifndef MICROKERNEL_ARM
            printf("Scheduler: IPC timeout -> %s\n", task->name);
#endif
        }

    if (task->state == TASK_BLOCKED &&
    task->wakeup_tick != 0 &&
    system_tick >= task->wakeup_tick)
        {
            task->state = TASK_READY;
            task->time_slice = DEFAULT_TIME_SLICE;

            printf("Scheduler: Task unblocked -> %s\n",
                 task->name);
        }

    }
}

const char* Scheduler_GetTaskStateName(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return "INVALID";
    }

    switch (task->state)
    {
        case TASK_READY:
            return "READY";

        case TASK_RUNNING:
            return "RUNNING";

        case TASK_BLOCKED:
            return "BLOCKED";

        case TASK_WAITING:
            return "WAITING";

        case TASK_FAULTED:
            return "FAULTED";

        default:
            return "UNKNOWN";
    }
}

void Scheduler_PrintCurrentTask(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return;
    }

    TaskControlBlock* current_task =
        Scheduler_GetCurrentTask(registry);

    if (current_task == NULL)
    {
        printf("Current Task: NONE\n");
        return;
    }

    printf("Current Task: %s | State: %s\n",
           current_task->name,
           Scheduler_GetTaskStateName(current_task));
}

void Scheduler_PrintStatus(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return;
    }

    TaskControlBlock* current_task =
        Scheduler_GetCurrentTask(registry);

    printf("Scheduler Status | Tick: %lu | Tasks: %d | Decisions: %lu | Context Switches: %lu | Recovery Events: %lu | Execution Cycles: %lu\n",
           Scheduler_GetTick(),
           Scheduler_GetTaskCount(registry),
           Scheduler_GetDecisionCount(registry),
           Scheduler_GetContextSwitchCount(registry),
           Scheduler_GetRecoveryEventCount(registry),
           Scheduler_GetExecutionCycles(registry));

    if (current_task != NULL)
    {
        printf("Scheduler Current: %s | State: %s\n",
               current_task->name,
               Scheduler_GetTaskStateName(current_task));
    }
    else
    {
        printf("Scheduler Current: NONE\n");
    }
}

void Scheduler_PrintTaskTable(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return;
    }

    printf("Task Table\n");
    printf("ID | Name | Priority | State | Runs | Restarts | Recovery | Execution Units | Progress\n");

    for (int i = 0; i < registry->task_count; i++)
    {
        TaskControlBlock* task = registry->tasks[i];

        if (task == NULL)
        {
            continue;
        }

        printf("%d | %s | %d | %s | %lu | %lu | %d | %lu | %lu\n",
               task->id,
               task->name,
               Scheduler_GetTaskPriority(task),
               Scheduler_GetTaskStateName(task),
               Scheduler_GetTaskRunCount(task),
               Scheduler_GetTaskRestartCount(task),
               Scheduler_IsTaskRecoveryPending(task),
               Scheduler_GetTaskExecutionUnits(task),
                Scheduler_GetTaskProgressCount(task));
    }
}

unsigned long Scheduler_GetTaskRunCount(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return 0;
    }

    return task->run_count;
}

unsigned long Scheduler_GetTaskRestartCount(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return 0;
    }

    return task->restart_count;
}

int Scheduler_IsTaskRecoveryPending(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return 0;
    }

    return task->recovery_pending;
}

unsigned long Scheduler_GetTaskExecutionUnits(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return 0;
    }

    return task->execution_units;
}

unsigned long Scheduler_GetContextSwitchCount(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return 0;
    }

    return registry->context_switches;
}

unsigned long Scheduler_GetRecoveryEventCount(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return 0;
    }

    return registry->recovery_events;
}

const char* Scheduler_GetRescheduleReasonName(RescheduleReason reason)
{

    switch (reason)
    {
        case RESCHEDULE_TIMESLICE:
            return "TIMESLICE";

        case RESCHEDULE_PRIORITY:
            return "PRIORITY";

        case RESCHEDULE_YIELD:
            return "YIELD";

        case RESCHEDULE_BLOCK:
            return "BLOCK";

        case RESCHEDULE_FAULT:
            return "FAULT";

        case RESCHEDULE_NONE:
        default:
            return "NONE";
    }
}

unsigned long Scheduler_GetExecutionCycles(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return 0;
    }
    return registry->execution_cycles;
}

int Scheduler_GetTaskPriority(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return -1;
    }

    return task->priority;
}

void Kernel_Run(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return;
    }

    unsigned long run_start_tick = Scheduler_GetTick();
    unsigned long run_start_execution_cycles = registry->execution_cycles;

#ifndef MICROKERNEL_ARM
    uint64_t cpu_start_ns = 0;
    uint64_t wall_start_ns = 0;

    int cpu_timer_started =
        Scheduler_ReadProcessCpuNs(&cpu_start_ns);

    int wall_timer_started =
        Scheduler_ReadMonotonicNs(&wall_start_ns);

    printf("CPU timer start success: %d, value: %llu ns\n",
       cpu_timer_started,
       (unsigned long long)cpu_start_ns);

#endif

    for (int i = 0; i < KERNEL_RUN_CYCLES; i++)
    {
        Scheduler_ProcessFaultRecovery(registry);

        Scheduler_Tick(registry);
        Scheduler_SelectNext(registry);

        printf("Kernel Tick: %lu\n", Scheduler_GetTick());
        printf("Scheduler Decisions: %lu\n",
               Scheduler_GetDecisionCount(registry));

        Scheduler_CheckIdleFallback(registry);

        if (registry->reschedule_pending)
{
    registry->reschedule_pending = 0;
    registry->reschedule_reason = RESCHEDULE_NONE;
}

        Scheduler_PrintStatus(registry);
        Scheduler_PrintTaskTable(registry);

           TaskControlBlock* current_task =
              Scheduler_GetCurrentTask(registry);

           if (current_task != NULL)
        {
            Scheduler_PrintCurrentTask(registry);

            if (!current_task->is_idle)
        {
            registry->execution_cycles++;
            current_task->execution_units++;
        }

          Scheduler_Dispatch(registry);
          Scheduler_VerifyRecovery(registry, current_task);
          Scheduler_CheckTaskWatchdog(registry);
        }
     }

    /* Capture scheduler activity for this Kernel_Run() interval. */
    registry->kernel_run_ticks =
        Scheduler_GetTick() - run_start_tick;

    registry->kernel_run_execution_cycles =
        registry->execution_cycles - run_start_execution_cycles;

#ifndef MICROKERNEL_ARM
    uint64_t cpu_end_ns = 0;
uint64_t wall_end_ns = 0;

int cpu_timer_ended = Scheduler_ReadProcessCpuNs(&cpu_end_ns);

if (cpu_timer_started &&
    cpu_timer_ended &&
    cpu_end_ns >= cpu_start_ns)
{
    registry->kernel_cpu_ns += cpu_end_ns - cpu_start_ns;
}

printf("CPU timer end success: %d, value: %llu ns\n",
       cpu_timer_ended,
       (unsigned long long)cpu_end_ns);

int wall_timer_ended = Scheduler_ReadMonotonicNs(&wall_end_ns);

if (wall_timer_started &&
    wall_timer_ended &&
    wall_end_ns >= wall_start_ns)
{
    registry->kernel_wall_ns += wall_end_ns - wall_start_ns;
}

#endif
}

void Idle_Task(TaskControlBlock* task)
{
    (void)task;
    printf("IdleTask is running...\n");

}

float Scheduler_GetLoad(TaskRegistry* registry)
{
    if (registry == NULL || registry->kernel_run_ticks == 0)
    {
        return 0.0f;
    }

    return ((float)registry->kernel_run_execution_cycles /
            (float)registry->kernel_run_ticks) * 100.0f;
}

void Scheduler_CheckIdleFallback(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return;
    }

    TaskControlBlock* current_task =
        Scheduler_GetCurrentTask(registry);

    if (current_task == registry->idle_task)
    {
        printf("Scheduler Diagnostic: No runnable task - IdleTask selected.\n");
    }
}

void Scheduler_CheckPreemption(TaskRegistry* registry)
{
    if (registry == NULL ||
        registry->current_task == NULL)
    {
        return;
    }

    TaskControlBlock* current = registry->current_task;

    if (current->state != TASK_RUNNING ||
        current->is_idle)
    {
        return;
    }

    for (int i = 0; i < registry->task_count; i++)
    {
        TaskControlBlock* task = registry->tasks[i];

        if (task == NULL ||
            task == current ||
            task->is_idle)
        {
            continue;
        }

        if (task->state == TASK_READY &&
            task->priority > current->priority)
        {
            printf("Scheduler: Higher-priority task ready -> %s\n",
                   task->name);

            registry->reschedule_pending = 1;
            registry->reschedule_reason = RESCHEDULE_PRIORITY;
            printf("Scheduler: Reschedule reason set -> %s\n",
       Scheduler_GetRescheduleReasonName(registry->reschedule_reason));

            return;
        }
    }
}

TaskWakeReason Scheduler_GetTaskWakeReason(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return WAKE_NONE;
    }

    return task->wake_reason;
}

void Scheduler_ClearTaskWakeReason(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    task->wake_reason = WAKE_NONE;
}

void Scheduler_CheckTaskWatchdog(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return;
    }

    for (int i = 0; i < registry->task_count; i++)
    {
        TaskControlBlock* task = registry->tasks[i];

        if (task == NULL)
        {
            continue;
        }

#ifndef MICROKERNEL_ARM
        int watchdog_active = 0;
        int task_faulted = 0;
        const char* task_name = NULL;
        unsigned long current_tick = 0UL;
        unsigned long progress_count = 0UL;
        unsigned long progress_tick = 0UL;
        unsigned long progress_age = 0UL;
#endif

        KernelCriticalState critical_state = Kernel_CriticalEnter();

        unsigned long tick = Scheduler_GetTick();
        unsigned long age = tick - task->last_progress_tick;

        if (!task->is_idle &&
            task->progress_enabled &&
            (task->state == TASK_RUNNING ||
             (task->state == TASK_READY &&
              task->execution_units >
                  task->watchdog_last_execution_units)))
        {
#ifndef MICROKERNEL_ARM
            watchdog_active = 1;
            task_name = task->name;
            current_tick = tick;
            progress_count = task->progress_count;
            progress_tick = task->last_progress_tick;
            progress_age = age;
#endif

            if (!task->progress_reported &&
                age > TASK_WATCHDOG_THRESHOLD)
            {
                Kernel_Trace(TRACE_TASK_FAULT, task);

                task->fault_type = FAULT_WATCHDOG;
                task->fault_count++;
                task->recovery_failed = 0;
                task->state = TASK_FAULTED;

                registry->reschedule_pending = 1;
                registry->reschedule_reason = RESCHEDULE_FAULT;

#ifndef MICROKERNEL_ARM
                task_faulted = 1;
#endif
            }

            task->progress_reported = 0;
        }

        Kernel_CriticalExit(critical_state);

#ifndef MICROKERNEL_ARM
        if (watchdog_active)
        {
            printf("Watchdog: %s | Tick: %lu | Last Progress Count: %lu | Last Progress Tick: %lu | Age: %lu\n",
                   task_name,
                   current_tick,
                   progress_count,
                   progress_tick,
                   progress_age);
        }

        if (task_faulted)
        {
            printf("WATCHDOG: Task stuck -> %s\n", task_name);
        }
#endif
    }
}

void Scheduler_ProcessFaultRecovery(TaskRegistry* registry)
{
    if (registry == NULL)
    {
        return;
    }

    for (int i = 0; i < registry->task_count; i++)
    {
        TaskControlBlock* task = registry->tasks[i];

        if (task != NULL &&
            task->state == TASK_FAULTED)
        {
            Scheduler_RecoverFaultedTask(registry, task);
        }
    }
}

void Scheduler_RecoverFaultedTask(TaskRegistry* registry,
                                  TaskControlBlock* task)
{
    if (registry == NULL || task == NULL)
    {
        return;
    }

    int isolate_task = 0;

#ifndef MICROKERNEL_ARM
    const char* task_name = task->name;
    unsigned long recovery_attempt = 0UL;
    unsigned long recovery_progress = 0UL;
#endif

    /*
     * Atomically reserve this task's recovery operation.
     */
    KernelCriticalState critical_state = Kernel_CriticalEnter();

    if (task->state != TASK_FAULTED ||
        task->isolated ||
        task->recovery_in_progress)
    {
        Kernel_CriticalExit(critical_state);
        return;
    }

    if (task->recovery_attempts >= MAX_RECOVERY_ATTEMPTS)
    {
        task->isolated = 1;
        task->recovery_failed = 1;
        task->recovery_pending = 0;
        task->recovery_in_progress = 0;
        task->state = TASK_FAULTED;
        isolate_task = 1;
    }
    else
    {
        task->recovery_in_progress = 1;
        task->recovery_start_progress_count = task->progress_count;
        task->recovery_attempts++;
        task->restart_count++;
        task->recovery_pending = 1;

        registry->recovery_events++;

#ifndef MICROKERNEL_ARM
        recovery_attempt = task->recovery_attempts;
        recovery_progress = task->recovery_start_progress_count;
#endif
    }

    Kernel_CriticalExit(critical_state);

    if (isolate_task)
    {
#ifndef MICROKERNEL_ARM
        printf("RECOVERY: Maximum attempts reached -> %s\n",
               task_name);
        printf("ISOLATION: Task permanently isolated -> %s\n",
               task_name);
#endif

        Kernel_Trace(TRACE_TASK_ISOLATED, task);
        return;
    }

#ifndef MICROKERNEL_ARM
    printf("RECOVERY: Restarting task -> %s | Attempt: %lu | Progress at Recovery Start: %lu\n",
           task_name,
           recovery_attempt,
           recovery_progress);
#endif

    Kernel_Trace(TRACE_RECOVERY_START, task);

    /*
     * Keep the reservation active while resource cleanup and
     * context rebuilding execute outside the critical section.
     */
    Kernel_IPC_CleanupTask(task);

    int mutex_cleanup_result = Kernel_Mutex_CleanupTask(task);

    if (mutex_cleanup_result != 0)
    {
        critical_state = Kernel_CriticalEnter();

        task->recovery_pending = 0;
        task->recovery_failed = 1;
        task->recovery_in_progress = 0;
        registry->reschedule_pending = 1;
        registry->reschedule_reason = RESCHEDULE_FAULT;

        Kernel_CriticalExit(critical_state);

#ifndef MICROKERNEL_ARM
        printf("RECOVERY: Mutex cleanup failed (error=%d) -> %s\n",
               mutex_cleanup_result,
               task_name);
#endif

        Kernel_Trace(TRACE_RECOVERY_FAILED, task);
        return;
    }

    if (Task_RestartContext(task) != 0)
    {
        critical_state = Kernel_CriticalEnter();

        task->recovery_pending = 0;
        task->recovery_failed = 1;
        task->recovery_in_progress = 0;
        registry->reschedule_pending = 1;
        registry->reschedule_reason = RESCHEDULE_FAULT;

        Kernel_CriticalExit(critical_state);

#ifndef MICROKERNEL_ARM
        printf("RECOVERY: Context restart failed -> %s\n", task_name);
#endif

        Kernel_Trace(TRACE_RECOVERY_FAILED, task);
        return;
    }

    /*
     * Publish the rebuilt task as READY and release its
     * recovery reservation atomically.
     */
    critical_state = Kernel_CriticalEnter();

    task->state = TASK_READY;
    task->wake_reason = WAKE_NONE;

    task->last_progress_tick = Scheduler_GetTick();
    task->last_progress_count = task->progress_count;
    task->watchdog_last_execution_units = task->execution_units;
    task->progress_reported = 0;

    task->time_slice = DEFAULT_TIME_SLICE;
    task->wakeup_tick = 0;
    task->ipc_timeout_tick = 0;

    task->fault_injection = FAULT_INJECT_NONE;
    task->fault_injection_enabled = 0;
    task->recovery_failed = 0;
    task->recovery_in_progress = 0;

    Kernel_CriticalExit(critical_state);
}

void Scheduler_TaskHeartbeat(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    if (!task->progress_enabled)
    {
        return;
    }

    task->last_heartbeat_tick = Scheduler_GetTick();

    printf("Heartbeat: %s | Heartbeat Tick: %lu\n",
           task->name,
           task->last_heartbeat_tick);
}

void Scheduler_TaskProgress(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return;
    }

    if (!task->progress_enabled)
    {
        return;
    }

    KernelCriticalState critical_state = Kernel_CriticalEnter();

    task->progress_count++;
    task->last_progress_count = task->progress_count;
    task->last_progress_tick = Scheduler_GetTick();
    task->watchdog_last_execution_units = task->execution_units;
    task->progress_reported = 1;

#ifndef MICROKERNEL_ARM
    unsigned long progress_count = task->progress_count;
    unsigned long progress_tick = task->last_progress_tick;
#endif

    Kernel_CriticalExit(critical_state);

    Kernel_Trace(TRACE_TASK_PROGRESS, task);

#ifndef MICROKERNEL_ARM
    printf("Progress: %s | Count: %lu | Tick: %lu\n",
           task->name,
           progress_count,
           progress_tick);
#endif
}

unsigned long Scheduler_GetTaskProgressCount(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return 0;
    }

    return task->progress_count;
}

void Scheduler_VerifyRecovery(TaskRegistry* registry,
                              TaskControlBlock* task)
{
    if (registry == NULL || task == NULL)
    {
        return;
    }

    int recovery_succeeded = 0;

#ifndef MICROKERNEL_ARM
    const char* task_name = task->name;
    unsigned long recovery_start_count = 0UL;
    unsigned long verified_progress_count = 0UL;
#endif

    /* Check eligibility and publish recovery success atomically. */
    KernelCriticalState critical_state = Kernel_CriticalEnter();

    if (task->recovery_pending &&
        task->state == TASK_RUNNING &&
        task->progress_count > task->recovery_start_progress_count)
    {
#ifndef MICROKERNEL_ARM
        recovery_start_count = task->recovery_start_progress_count;
        verified_progress_count = task->progress_count;
#endif

        task->successful_recoveries++;
        task->recovery_pending = 0;
        task->fault_type = FAULT_NONE;
        task->recovery_attempts = 0;
        task->recovery_failed = 0;
        recovery_succeeded = 1;
    }

    Kernel_CriticalExit(critical_state);

    if (!recovery_succeeded)
    {
        /*
         * No progress has been demonstrated yet.
         * Keep recovery pending and allow another dispatch.
         * The watchdog remains responsible for detecting a stall.
         */
        return;
    }

    Kernel_Trace(TRACE_RECOVERY_SUCCESS, task);

#ifndef MICROKERNEL_ARM
    printf("RECOVERY: Verification successful -> %s | "
           "Progress %lu -> %lu\n",
           task_name,
           recovery_start_count,
           verified_progress_count);
#endif
}
