#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task.h"

#define MAX_TASKS 10
#define TASK_WATCHDOG_THRESHOLD 5
#define MAX_RECOVERY_ATTEMPTS 2
#define KERNEL_RUN_CYCLES 100

typedef enum
{
    RESCHEDULE_NONE,
    RESCHEDULE_TIMESLICE,
    RESCHEDULE_PRIORITY,
    RESCHEDULE_YIELD,
    RESCHEDULE_BLOCK,
    RESCHEDULE_FAULT
} RescheduleReason;

typedef struct
{
    TaskControlBlock* tasks[MAX_TASKS];
    int task_count;
    TaskControlBlock* idle_task;
    int last_selected_index;
    unsigned long scheduler_decisions;
    unsigned long context_switches;
    unsigned long recovery_events;
    unsigned long execution_cycles;
        /* Elapsed-time measurements in nanoseconds. */
    uint64_t task_function_elapsed_ns;
    uint64_t idle_function_elapsed_ns;
    uint64_t scheduler_elapsed_ns;
    uint64_t kernel_wall_ns;
    uint64_t kernel_cpu_ns;
    unsigned long kernel_run_ticks;
    unsigned long kernel_run_execution_cycles;
    TaskControlBlock* current_task;
    int reschedule_pending;
    RescheduleReason reschedule_reason;
    int time_slice_expired;
} TaskRegistry;

void Scheduler_Init(TaskRegistry* registry);
int Scheduler_AddTask(TaskRegistry* registry, TaskControlBlock* task);
TaskControlBlock* Scheduler_SelectNext(TaskRegistry* registry);
void Scheduler_Dispatch(TaskRegistry* registry);
void Scheduler_HandleTaskReturn(TaskRegistry* registry, TaskControlBlock* task);
void Scheduler_Yield(TaskControlBlock* task);
void Scheduler_Block(TaskControlBlock* task);
void Scheduler_Unblock(TaskControlBlock* task);
void Scheduler_Delay(TaskControlBlock* task, unsigned long ticks);
void Scheduler_UpdateBlockedTasks(TaskRegistry* registry);
unsigned long Scheduler_GetTick(void);
/* Manually advances the tick; do not call concurrently with Scheduler_Tick(). */
void Scheduler_AdvanceTick(void);
int Scheduler_GetTaskCount(TaskRegistry* registry);
unsigned long Scheduler_GetDecisionCount(TaskRegistry* registry);
TaskControlBlock* Scheduler_GetCurrentTask(TaskRegistry* registry);
TaskState Scheduler_GetTaskState(TaskControlBlock* task);
const char* Scheduler_GetTaskStateName(TaskControlBlock* task);
void Scheduler_PrintCurrentTask(TaskRegistry* registry);
void Scheduler_PrintStatus(TaskRegistry* registry);
void Scheduler_PrintTaskTable(TaskRegistry* registry);
unsigned long Scheduler_GetTaskRunCount(TaskControlBlock* task);
unsigned long Scheduler_GetTaskRestartCount(TaskControlBlock* task);
int Scheduler_IsTaskRecoveryPending(TaskControlBlock* task);
unsigned long Scheduler_GetTaskExecutionUnits(TaskControlBlock* task);
int Scheduler_GetTaskPriority(TaskControlBlock* task);
void Scheduler_HandleTimeSlice(TaskControlBlock* task);
void Scheduler_Tick(TaskRegistry* registry);
void Kernel_Run(TaskRegistry* registry);
void Idle_Task(TaskControlBlock* task);
unsigned long Scheduler_GetContextSwitchCount(TaskRegistry* registry);
unsigned long Scheduler_GetRecoveryEventCount(TaskRegistry* registry);
unsigned long Scheduler_GetExecutionCycles(TaskRegistry* registry);
float Scheduler_GetLoad(TaskRegistry* registry);
void Scheduler_CheckIdleFallback(TaskRegistry* registry);
void Scheduler_CheckPreemption(TaskRegistry* registry);
TaskWakeReason Scheduler_GetTaskWakeReason(TaskControlBlock* task);
void Scheduler_ClearTaskWakeReason(TaskControlBlock* task);
void Scheduler_CheckTaskWatchdog(TaskRegistry* registry);
void Scheduler_ProcessFaultRecovery(TaskRegistry* registry);
/*
 * Report task liveness to the watchdog.
 * This refreshes the heartbeat timestamp but does not increment
 * the genuine task-progress counter used for recovery verification.
 */
void Scheduler_TaskHeartbeat(TaskControlBlock* task);

/*
 * Report a unit of task progress.
 * This increments the progress counter and updates the watchdog's
 * progress timestamp and execution baseline.
 */
void Scheduler_TaskProgress(TaskControlBlock* task);
unsigned long Scheduler_GetTaskProgressCount(TaskControlBlock* task);
void Scheduler_RecoverFaultedTask(TaskRegistry* registry,TaskControlBlock* task);
const char* Scheduler_GetRescheduleReasonName(RescheduleReason reason);
void Scheduler_VerifyRecovery(TaskRegistry* registry,TaskControlBlock* task);

#endif
