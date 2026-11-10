#include <stddef.h>
#include <stdint.h>
#include "task.h"
#include "scheduler.h"
#include "mutex.h"
#include "kernel_ipc.h"
#include "memory_pool.h"
#include "trace.h"

#define DEMO_TASKS 5
#define POOL_BLOCK_SIZE 32U
#define POOL_BLOCKS 8U

static TaskControlBlock tasks[DEMO_TASKS];
static TaskControlBlock *task_refs[DEMO_TASKS];
static TaskRegistry registry;
static KernelMutex demo_mutex;
static KernelMemoryPool pool;
static union {
    max_align_t align;
    unsigned char bytes[POOL_BLOCK_SIZE * POOL_BLOCKS];
} pool_storage;
static void *allocations[POOL_BLOCKS];
static TraceEvent event_snapshot[TRACE_BUFFER_SIZE];
static unsigned int event_snapshot_count;
static unsigned long demo_dispatches[DEMO_TASKS];

/*
 * Give the browser demonstration a realistic mix of short-running and
 * periodically blocked tasks. This changes only the demo workload; the
 * production scheduler's priority policy remains untouched.
 */
static void DemoTask(TaskControlBlock *task) {
    if (task == NULL || task->id < 1 || task->id > DEMO_TASKS) return;

    unsigned int index = (unsigned int)(task->id - 1);
    demo_dispatches[index]++;

    /* Higher-priority sensor/communication tasks periodically wait for input. */
    if ((index < 2 && demo_dispatches[index] % 2U == 0U) ||
        (index >= 2 && demo_dispatches[index] % 2U == 0U)) {
        Scheduler_Delay(task, index < 2 ? 12U : 3U);
    }
}

static void RefreshEvents(void) {
    event_snapshot_count = Kernel_Trace_Copy(event_snapshot, TRACE_BUFFER_SIZE);
}

int MK_Init(void) {
    Kernel_Trace_Init();
    Kernel_IPC_Init();
    Scheduler_Init(&registry);
    Kernel_Mutex_Init(&demo_mutex);
    for (int i = 0; i < DEMO_TASKS; ++i) {
        demo_dispatches[i] = 0;
        static const char *names[DEMO_TASKS] = {
            "SensorTask", "CommunicationTask", "WorkerTask",
            "RecoveryTask", "IsolationProbe"
        };
        static const int priorities[DEMO_TASKS] = {4, 4, 2, 1, 1};
        Task_Init(&tasks[i], i + 1, priorities[i], names[i], DemoTask);
        tasks[i].progress_enabled = 1;
        task_refs[i] = &tasks[i];
        if (Scheduler_AddTask(&registry, &tasks[i]) != 0) return -1;
        allocations[i] = NULL;
    }
    if (Kernel_MemoryPool_Init(&pool, pool_storage.bytes,
                               POOL_BLOCK_SIZE, POOL_BLOCKS) != 0) return -2;
    event_snapshot_count = 0;
    return 0;
}

void MK_Step(void) {
    Scheduler_Tick(&registry);
    TaskControlBlock *next = Scheduler_SelectNext(&registry);
    if (next != NULL) {
        next->execution_units++;
        Scheduler_TaskProgress(next);
        Scheduler_Dispatch(&registry);
        Scheduler_VerifyRecovery(&registry, next);
    }
    Scheduler_CheckTaskWatchdog(&registry);
    RefreshEvents();
}

int MK_MutexScenario(void) {
    int a = Kernel_Mutex_Lock(&demo_mutex, &tasks[2]);
    int b = Kernel_Mutex_Lock(&demo_mutex, &tasks[0]);
    int c = Kernel_Mutex_Unlock(&demo_mutex, &tasks[2]);
    RefreshEvents();
    return (a == 0 && b == 1 && c == 0) ? 0 : -1;
}

int MK_IPCScenario(void) {
    char message[IPC_MESSAGE_SIZE];
    tasks[1].state = TASK_RUNNING;
    Kernel_IPC_RegisterReceiver(&tasks[1]);
    Kernel_IPC_Wait(&tasks[1]);
    int sent = Kernel_IPC_Send("sensor:temperature=24.8");
    int received = Kernel_IPC_Receive(message, sizeof(message));
    RefreshEvents();
    return (sent == 0 && received == 0 && tasks[1].state == TASK_READY) ? 0 : -1;
}

int MK_InjectWatchdog(void) {
    TaskControlBlock *task = &tasks[3];
    task->state = TASK_RUNNING;
    task->progress_enabled = 1;
    task->last_progress_tick = Scheduler_GetTick();
    task->progress_reported = 0;
    for (unsigned int i = 0; i <= TASK_WATCHDOG_THRESHOLD; ++i)
        Scheduler_AdvanceTick();
    Scheduler_CheckTaskWatchdog(&registry);
    if (task->state != TASK_FAULTED) return -1;
    Scheduler_RecoverFaultedTask(&registry, task);
    if (!task->recovery_pending) return -2;
    task->state = TASK_RUNNING;
    Scheduler_TaskProgress(task);
    Scheduler_VerifyRecovery(&registry, task);
    if (task->successful_recoveries == 0) return -3;

    /*
     * The demo performs recovery verification outside the normal scheduler
     * dispatch path. Publish the recovered task as READY afterwards so it
     * re-enters normal scheduling instead of remaining an orphan RUNNING
     * task that the watchdog can no longer see making progress.
     */
    task->state = TASK_READY;
    RefreshEvents();
    return 0;
}

int MK_InjectIsolation(void) {
    TaskControlBlock *task = &tasks[4];
    task->state = TASK_RUNNING;
    task->progress_enabled = 1;
    task->last_progress_tick = Scheduler_GetTick();
    task->progress_reported = 0;
    task->recovery_attempts = MAX_RECOVERY_ATTEMPTS;
    for (unsigned int i = 0; i <= TASK_WATCHDOG_THRESHOLD; ++i)
        Scheduler_AdvanceTick();
    Scheduler_CheckTaskWatchdog(&registry);
    if (task->state != TASK_FAULTED) return -1;
    Scheduler_RecoverFaultedTask(&registry, task);
    RefreshEvents();
    return task->isolated ? 0 : -2;
}

int MK_AllocateBlock(void) {
    for (int i = 0; i < (int)POOL_BLOCKS; ++i) {
        if (allocations[i] == NULL) {
            allocations[i] = Kernel_MemoryPool_Alloc(&pool);
            RefreshEvents();
            return allocations[i] != NULL ? i : -1;
        }
    }
    return -1;
}

int MK_FreeBlock(int index) {
    if (index < 0 || index >= (int)POOL_BLOCKS || allocations[index] == NULL)
        return -1;
    int result = Kernel_MemoryPool_Free(&pool, allocations[index]);
    if (result == 0) allocations[index] = NULL;
    RefreshEvents();
    return result;
}

int MK_GetTaskCount(void) { return registry.task_count; }
const char *MK_GetTaskName(int i) { return (i >= 0 && i < DEMO_TASKS) ? tasks[i].name : ""; }
const char *MK_GetTaskState(int i) { return (i >= 0 && i < DEMO_TASKS) ? Scheduler_GetTaskStateName(&tasks[i]) : "INVALID"; }
int MK_GetTaskPriority(int i) { return (i >= 0 && i < DEMO_TASKS) ? tasks[i].priority : 0; }
unsigned long MK_GetTaskProgress(int i) { return (i >= 0 && i < DEMO_TASKS) ? tasks[i].progress_count : 0; }
unsigned long MK_GetTaskRestarts(int i) { return (i >= 0 && i < DEMO_TASKS) ? tasks[i].restart_count : 0; }
unsigned long MK_GetTaskRecoveries(int i) { return (i >= 0 && i < DEMO_TASKS) ? tasks[i].successful_recoveries : 0; }
int MK_GetTaskIsolated(int i) { return (i >= 0 && i < DEMO_TASKS) ? tasks[i].isolated : 0; }
unsigned long MK_GetTick(void) { return Scheduler_GetTick(); }
unsigned long MK_GetContextSwitches(void) { return Scheduler_GetContextSwitchCount(&registry); }
unsigned long MK_GetDecisions(void) { return Scheduler_GetDecisionCount(&registry); }
int MK_GetFreeBlocks(void) { return (int)Kernel_MemoryPool_GetFreeCount(&pool); }
int MK_GetTotalBlocks(void) { return (int)POOL_BLOCKS; }
int MK_GetMutexLocked(void) { return Kernel_Mutex_IsLocked(&demo_mutex); }
const char *MK_GetMutexOwner(void) {
    TaskControlBlock *owner = Kernel_Mutex_GetOwner(&demo_mutex);
    return owner != NULL ? owner->name : "None";
}
int MK_GetIPCQueued(void) { return Kernel_IPC_GetCount(); }
int MK_GetIPCWaiting(void) { return Kernel_IPC_GetWaitingCount(); }
int MK_GetEventCount(void) { RefreshEvents(); return (int)event_snapshot_count; }
const char *MK_GetEventType(int i) { return (i >= 0 && (unsigned int)i < event_snapshot_count) ? Kernel_Trace_GetEventName(event_snapshot[i].type) : ""; }
const char *MK_GetEventTask(int i) { return (i >= 0 && (unsigned int)i < event_snapshot_count) ? event_snapshot[i].task_name : ""; }
unsigned long MK_GetEventTick(int i) { return (i >= 0 && (unsigned int)i < event_snapshot_count) ? event_snapshot[i].tick : 0; }
