#include <stdio.h>
#include <string.h>
#include "task.h"
#include "scheduler.h"
#include "kernel_ipc.h"
#include "trace.h"
#include "mutex.h"

KernelMutex shared_resource_mutex;


static void IPC_TestTask(TaskControlBlock* task)
{
    (void)task;
}

static int IPC_RegressionTests(void)
{
    IPC_MessageQueue queue;
    char buffer[IPC_MESSAGE_SIZE];

    printf("\n=== IPC REGRESSION TESTS ===\n");

    IPC_Init(&queue);
    if (!IPC_IsEmpty(&queue) || IPC_GetCount(&queue) != 0 ||
        IPC_Receive(&queue, buffer, sizeof(buffer)) != -1)
        goto queue_fail;

    if (IPC_Send(NULL, "TEST") != -1 ||
        IPC_Send(&queue, NULL) != -1 ||
        IPC_Receive(NULL, buffer, sizeof(buffer)) != -1 ||
        IPC_Receive(&queue, NULL, sizeof(buffer)) != -1)
        goto queue_fail;

    if (IPC_Send(&queue, "BUFFER_TEST") != 0 ||
        IPC_Receive(&queue, buffer, 3) != -2 ||
        IPC_GetCount(&queue) != 1 ||
        IPC_Receive(&queue, buffer, sizeof(buffer)) != 0 ||
        strcmp(buffer, "BUFFER_TEST") != 0)
        goto queue_fail;

    IPC_Init(&queue);
    for (int i = 0; i < IPC_QUEUE_SIZE; i++)
    {
        char message[8];
        snprintf(message, sizeof(message), "%d", i);
        if (IPC_Send(&queue, message) != 0)
            goto queue_fail;
    }

    if (!IPC_IsFull(&queue) || IPC_Send(&queue, "OVERFLOW") != -1)
        goto queue_fail;

    for (int i = 0; i < 3; i++)
    {
        char expected[8];
        snprintf(expected, sizeof(expected), "%d", i);
        if (IPC_Receive(&queue, buffer, sizeof(buffer)) != 0 ||
            strcmp(buffer, expected) != 0)
            goto queue_fail;
    }

    for (int i = 8; i < 11; i++)
    {
        char message[8];
        snprintf(message, sizeof(message), "%d", i);
        if (IPC_Send(&queue, message) != 0)
            goto queue_fail;
    }

    for (int i = 3; i < 11; i++)
    {
        char expected[8];
        snprintf(expected, sizeof(expected), "%d", i);
        if (IPC_Receive(&queue, buffer, sizeof(buffer)) != 0 ||
            strcmp(buffer, expected) != 0)
            goto queue_fail;
    }

    {
        char long_message[40];
        memset(long_message, 'X', sizeof(long_message) - 1);
        long_message[sizeof(long_message) - 1] = '\0';

        if (IPC_Send(&queue, long_message) != 0 ||
            IPC_Receive(&queue, buffer, sizeof(buffer)) != 0 ||
            strlen(buffer) != IPC_MESSAGE_SIZE - 1)
            goto queue_fail;

        for (size_t i = 0; i < IPC_MESSAGE_SIZE - 1; i++)
            if (buffer[i] != 'X')
                goto queue_fail;
    }

    printf("IPC TEST PASSED: Queue operations\n");

    {
        TaskControlBlock tasks[5];

        for (int i = 0; i < 5; i++)
        {
            Task_Init(&tasks[i], 200 + i, 2,
                      "IPCTestTask", IPC_TestTask);
            tasks[i].state = TASK_RUNNING;
        }

        Kernel_IPC_Init();

        for (int i = 0; i < IPC_MAX_WAITERS; i++)
            Kernel_IPC_RegisterReceiver(&tasks[i]);

        Kernel_IPC_RegisterReceiver(&tasks[0]);
        Kernel_IPC_RegisterReceiver(&tasks[4]);

        if (Kernel_IPC_GetWaitingCount() != IPC_MAX_WAITERS ||
            Kernel_IPC_GetWaitingTask() != &tasks[0])
            goto kernel_fail;

        Kernel_IPC_RemoveReceiver(&tasks[1]);
        if (Kernel_IPC_GetWaitingCount() != 3 ||
            Kernel_IPC_GetWaitingTask() != &tasks[0])
            goto kernel_fail;

        Kernel_IPC_Init();

        tasks[0].state = TASK_RUNNING;
        tasks[1].state = TASK_RUNNING;
        Kernel_IPC_RegisterReceiver(&tasks[0]);
        Kernel_IPC_RegisterReceiver(&tasks[1]);
        Kernel_IPC_Wait(&tasks[0]);
        Kernel_IPC_Wait(&tasks[1]);

        if (tasks[0].state != TASK_WAITING ||
            tasks[1].state != TASK_WAITING ||
            Kernel_IPC_Send("WAKE_TEST") != 0 ||
            tasks[0].state != TASK_READY ||
            tasks[0].wake_reason != WAKE_IPC ||
            tasks[0].ipc_timeout_tick != 0 ||
            tasks[1].state != TASK_WAITING ||
            Kernel_IPC_GetWaitingCount() != 1 ||
            Kernel_IPC_GetCount() != 1)
            goto kernel_fail;

        if (Kernel_IPC_Receive(buffer, sizeof(buffer)) != 0 ||
            strcmp(buffer, "WAKE_TEST") != 0)
            goto kernel_fail;

        Kernel_IPC_Init();

        tasks[0].state = TASK_RUNNING;
        tasks[1].state = TASK_RUNNING;
        Kernel_IPC_RegisterReceiver(&tasks[0]);
        Kernel_IPC_RegisterReceiver(&tasks[1]);
        Kernel_IPC_Wait(&tasks[1]);

        if (Kernel_IPC_Send("SKIP_TEST") != 0 ||
            tasks[0].state != TASK_RUNNING ||
            tasks[1].state != TASK_READY ||
            tasks[1].wake_reason != WAKE_IPC)
            goto kernel_fail;

        Kernel_IPC_Init();

        tasks[0].state = TASK_RUNNING;
        tasks[0].wake_reason = WAKE_NONE;
        tasks[0].ipc_timeout_tick = 0;
        Kernel_IPC_RegisterReceiver(&tasks[0]);

        if (Kernel_IPC_Send("EARLY_MESSAGE") != 0)
            goto kernel_fail;

        Kernel_IPC_Wait(&tasks[0]);

        if (tasks[0].state != TASK_RUNNING ||
            tasks[0].wake_reason != WAKE_IPC ||
            tasks[0].ipc_timeout_tick != 0 ||
            Kernel_IPC_GetWaitingCount() != 0 ||
            Kernel_IPC_Receive(buffer, sizeof(buffer)) != 0 ||
            strcmp(buffer, "EARLY_MESSAGE") != 0)
            goto kernel_fail;
    }

        {
        TaskControlBlock timeout_task;
        TaskRegistry timeout_registry;

        Scheduler_Init(&timeout_registry);
        Task_Init(&timeout_task, 210, 2,
                  "IPCTimeoutTest", IPC_TestTask);

        Kernel_IPC_Init();

        if (Scheduler_AddTask(&timeout_registry, &timeout_task) != 0)
            goto kernel_fail;

        timeout_task.state = TASK_RUNNING;
        Kernel_IPC_RegisterReceiver(&timeout_task);
        Kernel_IPC_Wait(&timeout_task);

        if (timeout_task.state != TASK_WAITING ||
            timeout_task.ipc_timeout_tick == 0 ||
            Kernel_IPC_GetWaitingCount() != 1)
            goto kernel_fail;

        /* Advance to the IPC timeout deadline. */
        while (Scheduler_GetTick() < timeout_task.ipc_timeout_tick)
       {
          Scheduler_Tick(&timeout_registry);
       }

            if (timeout_task.state != TASK_READY ||
            timeout_task.wake_reason != WAKE_TIMEOUT ||
            timeout_task.ipc_timeout_tick != 0 ||
            Kernel_IPC_GetWaitingCount() != 0)
        {
            printf("TIMEOUT DEBUG: state=%d reason=%d deadline=%lu tick=%lu waiting=%d\n",
                   timeout_task.state,
                   timeout_task.wake_reason,
                   timeout_task.ipc_timeout_tick,
                   Scheduler_GetTick(),
                   Kernel_IPC_GetWaitingCount());
            goto kernel_fail;
        }

        printf("IPC TEST PASSED: Timeout handling\n");
    }

    Kernel_IPC_Init();
    printf("IPC TEST PASSED: Kernel IPC operations\n");
    printf("IPC REGRESSION TESTS PASSED\n");
    return 0;

queue_fail:
    printf("IPC TEST FAILED: Queue operations\n");
    return 1;

kernel_fail:
    printf("IPC TEST FAILED: Kernel IPC operations\n");
    Kernel_IPC_Init();
    return 1;
}

void Sensor_Task(TaskControlBlock* task)
{
    printf("SensorTask is running...\n");

    if (!task->mutex_waiting &&
    Kernel_Mutex_GetOwner(&shared_resource_mutex) != task)
{
    printf("SensorTask: Mutex lock requested\n");

    int mutex_result =
        Kernel_Mutex_Lock(&shared_resource_mutex, task);

    if (mutex_result == 1)
    {
        printf("SensorTask: Blocked waiting for mutex\n");
        return;
    }

    if (mutex_result != 0)
    {
        printf("SensorTask: Mutex lock failed\n");
        return;
    }

    printf("SensorTask: Mutex acquired\n");
}
else if (Kernel_Mutex_GetOwner(&shared_resource_mutex) == task)
{
    printf("SensorTask: Mutex ownership transferred\n");
}

    Scheduler_TaskProgress(task);

    if (Kernel_IPC_Send("SENSOR_DATA_READY") == 0)
    {
        printf("SensorTask: IPC message sent.\n");
    }

    printf("SensorTask: Mutex unlock requested\n");

    if (Kernel_Mutex_Unlock(&shared_resource_mutex, task) == 0)
    {
        printf("SensorTask: Mutex released\n");
    }

    Scheduler_Delay(task, 5);
}

void Communication_Task(TaskControlBlock* task)
{
    printf("CommunicationTask is running...\n");

    /*
     * Multiple-waiter mutex test.
     *
     * CommunicationTask has priority 2.
     * CPUTask has base priority 1.
     *
     * When CPUTask owns the mutex, CommunicationTask
     * should block and cause priority inheritance:
     *
     *     CPUTask: 1 -> 2
     */

    if (!task->mutex_waiting &&
        Kernel_Mutex_GetOwner(&shared_resource_mutex) != task)
    {
        printf("CommunicationTask: Mutex lock requested\n");

        int mutex_result =
            Kernel_Mutex_Lock(&shared_resource_mutex, task);

        if (mutex_result == 1)
        {
            printf("CommunicationTask: Blocked waiting for mutex\n");
            return;
        }

        if (mutex_result != 0)
        {
            printf("CommunicationTask: Mutex lock failed\n");
            return;
        }

        printf("CommunicationTask: Mutex acquired\n");
    }

    if (Kernel_Mutex_GetOwner(&shared_resource_mutex) == task)
    {
        printf("CommunicationTask: Mutex ownership transferred/acquired\n");

        Scheduler_TaskProgress(task);

        printf("CommunicationTask: Mutex unlock requested\n");

        if (Kernel_Mutex_Unlock(&shared_resource_mutex, task) == 0)
        {
            printf("CommunicationTask: Mutex released\n");
        }
    }
}

void Mutex_Task(TaskControlBlock* task)
{
    printf("MutexTask is running...\n");

    if (!task->mutex_waiting &&
        Kernel_Mutex_GetOwner(&shared_resource_mutex) != task)
    {
        printf("MutexTask: Mutex lock requested\n");

        int mutex_result =
            Kernel_Mutex_Lock(&shared_resource_mutex, task);

        if (mutex_result == 1)
        {
            printf("MutexTask: Blocked waiting for mutex\n");
            return;
        }

        if (mutex_result != 0)
        {
            printf("MutexTask: Mutex lock failed\n");
            return;
        }

        printf("MutexTask: Mutex acquired\n");
    }

    if (Kernel_Mutex_GetOwner(&shared_resource_mutex) == task)
    {
        printf("MutexTask: Mutex ownership acquired\n");

        Scheduler_TaskProgress(task);

        printf("MutexTask: Mutex unlock requested\n");

        if (Kernel_Mutex_Unlock(&shared_resource_mutex, task) == 0)
        {
            printf("MutexTask: Mutex released\n");
        }
    }
}

void CPU_Task(TaskControlBlock* task)
{
    printf("CPUTask is running...");

    /*
     * Persistent watchdog fault injection.
     *
     * While the injected fault is active, this task deliberately
     * performs no progress reporting. The watchdog must detect this.
     *
     * The fault remains active until the recovery mechanism clears it.
     */
    if (task->fault_injection == FAULT_INJECT_WATCHDOG)
    {
        printf("FAULT INJECTION: Watchdog stall -> %s\n",
               task->name);

        return;
    }

    if (task->recovery_failure_injection_enabled &&
    task->restart_count > 0)
{
    printf("FAULT INJECTION: Recovery failure -> %s\n",
           task->name);

    return;
}

    if (!task->mutex_hold_active)
{
    printf("CPUTask: Mutex lock requested\n");

    int mutex_result =
        Kernel_Mutex_Lock(&shared_resource_mutex, task);

    if (mutex_result == 1)
    {
        printf("CPUTask: Blocked waiting for mutex\n");
        return;
    }

    if (mutex_result != 0)
    {
        printf("CPUTask: Mutex lock failed\n");
        return;
    }

    printf("CPUTask: Mutex acquired\n");

    task->mutex_hold_active = 1;
    task->mutex_hold_phase = 0;
}

    Scheduler_TaskProgress(task);

    if (Kernel_IPC_Send("CPU_STATUS_READY") == 0)
    {
        printf("CPUTask: IPC message sent.\n");
    }

    if (task->mutex_hold_active &&
    Kernel_Mutex_GetOwner(&shared_resource_mutex) == task)
{
    if (task->mutex_hold_phase == 0)
    {
        printf("CPUTask: Holding mutex across yield\n");

        task->mutex_hold_phase = 1;

        Scheduler_Yield(task);
        return;
    }

    printf("CPUTask: Mutex unlock requested\n");

    if (Kernel_Mutex_Unlock(&shared_resource_mutex, task) == 0)
    {
        printf("CPUTask: Mutex released\n");

        task->mutex_hold_active = 0;
        task->mutex_hold_phase = 0;
    }
}

    if (Scheduler_GetTick() == 3)
    {
        printf("CPUTask: SensorTask should be ready.\n");
    }
}

static void RecoveryYieldProbe_Task(TaskControlBlock* task)
{
    Scheduler_TaskProgress(task);

    if (task->run_count == 1UL)
    {
        Scheduler_Yield(task);
    }
}


static void RecoveryHeartbeatProbe_Task(TaskControlBlock* task)
{
    Scheduler_TaskHeartbeat(task);
}

int main(void)
{
 TaskControlBlock sensor_task;
 TaskControlBlock communication_task;
 TaskControlBlock mutex_task;
 TaskControlBlock cpu_task;
 TaskControlBlock idle_task;
 TaskRegistry registry;

 Scheduler_Init(&registry);
 Kernel_IPC_Init();
 if (IPC_RegressionTests() != 0)
 {
     printf("IPC regression tests failed. Stopping.\n");
     return 1;
 }
 Kernel_Trace_Init();
 Kernel_Mutex_Init(&shared_resource_mutex);
 KernelMutex mutex_a;
 KernelMutex mutex_b;
 printf("Initial Tick: %lu\n", Scheduler_GetTick());

 Task_Init(&sensor_task,1,3, "SensorTask", Sensor_Task);
 Task_Init(&communication_task,2,2, "communicationtask", Communication_Task);
 Task_Init(&mutex_task,4,2,"MutexTask",Mutex_Task);
 Task_Init(&cpu_task,3,1, "CPUTask", CPU_Task);
 /* Verify initialization clears a previous stack-warning latch. */
 cpu_task.stack_warning_reported = 1;
 Task_Init(&cpu_task, 3, 1, "CPUTask", CPU_Task);

 if (cpu_task.stack_warning_reported == 0)
 {
     printf("Step 17.5F: Warning latch reset by Task_Init\n");
 }
 else
 {
     printf("Step 17.5F: ERROR - Warning latch not reset\n");
     return 1;
 }

 cpu_task.fault_injection = FAULT_INJECT_WATCHDOG;
 cpu_task.fault_injection_enabled = 1;
 cpu_task.progress_enabled = 1;
 cpu_task.recovery_failure_injection_enabled = 0;
 Task_Init(&idle_task,0,0, "IdleTask", Idle_Task);
 Kernel_Mutex_Init(&mutex_a);
 Kernel_Mutex_Init(&mutex_b);
 idle_task.is_idle = 1;

/* Test 1: CPUTask becomes the legitimate owner. */
if (Kernel_Mutex_Lock(&shared_resource_mutex, &cpu_task) == 0)
{
    printf("CPUTask acquired mutex\n");
}

if (cpu_task.owned_mutexes == &shared_resource_mutex &&
    shared_resource_mutex.next_owned == NULL)
{
    printf("Step 20: Owned-mutex list records acquisition\n");
}
else
{
    printf("Step 20: ERROR - Mutex ownership list incorrect\n");
    return 1;
}

/* Test 2: Non-owner must not be able to unlock. */
if (Kernel_Mutex_Unlock(&shared_resource_mutex, &sensor_task) != 0)
{
    printf("Non-owner unlock correctly rejected\n");
}

/* Test 3: Owner must not recursively lock the same mutex. */
if (Kernel_Mutex_Lock(&shared_resource_mutex, &cpu_task) != 0)
{
    printf("Recursive lock correctly rejected\n");
}

/* Test 4: Legitimate owner releases the mutex. */
if (Kernel_Mutex_Unlock(&shared_resource_mutex, &cpu_task) == 0)
{
    printf("Owner unlock succeeded\n");
}

if (cpu_task.owned_mutexes == NULL &&
    shared_resource_mutex.next_owned == NULL)
{
    printf("Step 21: Unlock clears owned-mutex list\n");
}
else
{
    printf("Step 21: ERROR - Owned-mutex list not cleared\n");
    return 1;
}

/* Test 5: Mutex must now be completely free. */
if (!Kernel_Mutex_IsLocked(&shared_resource_mutex) &&
    Kernel_Mutex_GetOwner(&shared_resource_mutex) == NULL)
{
    printf("Mutex state correctly cleared\n");
}

printf("\nTesting ownership transfer\n");

/* Test 6: CPUTask becomes owner again. */
if (Kernel_Mutex_Lock(&shared_resource_mutex, &cpu_task) == 0)
{
    printf("CPUTask acquired mutex for transfer test\n");
}

/* SensorTask must wait because CPUTask owns the mutex. */
if (Kernel_Mutex_Lock(&shared_resource_mutex, &sensor_task) == 1)
{
    printf("SensorTask correctly blocked as waiter\n");
}

/* Verify CPUTask is still the owner. */
if (Kernel_Mutex_GetOwner(&shared_resource_mutex) == &cpu_task)
{
    printf("CPUTask confirmed as owner\n");
}

/* CPUTask releases the mutex. Ownership should transfer. */
if (Kernel_Mutex_Unlock(&shared_resource_mutex, &cpu_task) == 0)
{
    printf("CPUTask released mutex\n");
}

/* Verify SensorTask became the new owner. */
if (Kernel_Mutex_GetOwner(&shared_resource_mutex) == &sensor_task)
{
    printf("Ownership transferred to SensorTask\n");
}

if (cpu_task.owned_mutexes == NULL &&
    sensor_task.owned_mutexes == &shared_resource_mutex &&
    shared_resource_mutex.next_owned == NULL)
{
    printf("Step 22: Ownership list updated after handoff\n");
}
else
{
    printf("Step 22: ERROR - Ownership list incorrect after handoff\n");
    return 1;
}

/* Old owner must no longer be allowed to unlock. */
if (Kernel_Mutex_Unlock(&shared_resource_mutex, &cpu_task) != 0)
{
    printf("Old owner unlock correctly rejected\n");
}

/* Current owner releases the mutex. */
if (Kernel_Mutex_Unlock(&shared_resource_mutex, &sensor_task) == 0)
{
    printf("New owner unlock succeeded\n");
}

/* Final state must again be completely free. */
if (!Kernel_Mutex_IsLocked(&shared_resource_mutex) &&
    Kernel_Mutex_GetOwner(&shared_resource_mutex) == NULL)
{
    printf("Final mutex state correctly cleared\n");
}

printf("\n=== STEP 23: TASK MUTEX CLEANUP TEST ===\n");

TaskControlBlock cleanup_task;
TaskControlBlock cleanup_owner;
TaskControlBlock cleanup_waiter;
KernelMutex cleanup_owned_mutex;
KernelMutex cleanup_waiting_mutex;

Task_Init(&cleanup_task, 81, 3, "CleanupTask", CPU_Task);
Task_Init(&cleanup_owner, 82, 1, "CleanupOwner", CPU_Task);
Task_Init(&cleanup_waiter, 83, 4, "CleanupWaiter", CPU_Task);

Kernel_Mutex_Init(&cleanup_owned_mutex);
Kernel_Mutex_Init(&cleanup_waiting_mutex);

int cleanup_setup_ok =
    Kernel_Mutex_Lock(&cleanup_owned_mutex, &cleanup_task) == 0 &&
    Kernel_Mutex_Lock(&cleanup_owned_mutex, &cleanup_waiter) == 1 &&
    Kernel_Mutex_Lock(&cleanup_waiting_mutex, &cleanup_owner) == 0 &&
    Kernel_Mutex_Lock(&cleanup_waiting_mutex, &cleanup_task) == 1;

if (cleanup_setup_ok)
{
    cleanup_task.state = TASK_FAULTED;

    int cleanup_result = Kernel_Mutex_CleanupTask(&cleanup_task);

    if (cleanup_result == 0 &&
        cleanup_task.state == TASK_FAULTED &&
        cleanup_task.owned_mutexes == NULL &&
        cleanup_task.waiting_mutex == NULL &&
        cleanup_task.mutex_waiting == 0 &&
        cleanup_task.mutex_hold_active == 0 &&
        cleanup_owned_mutex.owner == &cleanup_waiter &&
        cleanup_waiter.owned_mutexes == &cleanup_owned_mutex &&
        cleanup_waiting_mutex.waiter_count == 0 &&
        cleanup_waiting_mutex.owner == &cleanup_owner &&
        cleanup_owner.priority == cleanup_owner.base_priority &&
        cleanup_waiter.state == TASK_READY &&
        Kernel_Mutex_ValidateState(&cleanup_owned_mutex) == 0 &&
        Kernel_Mutex_ValidateState(&cleanup_waiting_mutex) == 0)
    {
        printf("Step 23: Faulted-task mutex cleanup passed\n");
    }
    else
    {
        printf("Step 23: ERROR - Faulted-task mutex cleanup failed\n");
        return 1;
    }
}
else
{
    printf("Step 23: ERROR - Cleanup test setup failed\n");
    return 1;
}

printf("\n=== STEP 24: IPC TASK CLEANUP TEST ===\n");

TaskControlBlock ipc_cleanup_task;
Task_Init(&ipc_cleanup_task, 84, 2, "IPCCleanupTask", CPU_Task);

int ipc_waiting_before = Kernel_IPC_GetWaitingCount();
ipc_cleanup_task.state = TASK_RUNNING;
Kernel_IPC_RegisterReceiver(&ipc_cleanup_task);

if (Kernel_IPC_GetWaitingCount() != ipc_waiting_before + 1)
{
    printf("Step 24: ERROR - IPC receiver registration failed\n");
    return 1;
}

Kernel_IPC_Wait(&ipc_cleanup_task);

/* Simulate stale IPC metadata when a task faults. */
ipc_cleanup_task.wake_reason = WAKE_TIMEOUT;
ipc_cleanup_task.ipc_timeout_tick = Scheduler_GetTick() + 99;
ipc_cleanup_task.state = TASK_FAULTED;

Kernel_IPC_CleanupTask(&ipc_cleanup_task);

if (ipc_cleanup_task.state == TASK_FAULTED &&
    ipc_cleanup_task.wake_reason == WAKE_NONE &&
    ipc_cleanup_task.ipc_timeout_tick == 0 &&
    Kernel_IPC_GetWaitingCount() == ipc_waiting_before)
{
    printf("Step 24: Faulted-task IPC cleanup passed\n");
}
else
{
    printf("Step 24: ERROR - Faulted-task IPC cleanup failed\n");
    return 1;
}

printf("\n=== STEP 12: MUTEX DEADLOCK DETECTION TEST ===\n");

/*
 * Task A owns Mutex A.
 * Task B owns Mutex B.
 */
Kernel_Mutex_Lock(&mutex_a, &cpu_task);
Kernel_Mutex_Lock(&mutex_b, &mutex_task);

printf("Step 12: CPUTask owns Mutex A\n");
printf("Step 12: MutexTask owns Mutex B\n");

/*
 * Create circular wait:
 *
 * CPUTask -> Mutex B -> MutexTask
 * MutexTask -> Mutex A -> CPUTask
 */

/* CPUTask waits for Mutex B */
Kernel_Mutex_Lock(&mutex_b, &cpu_task);

/* MutexTask waits for Mutex A.
 * This should be detected as a deadlock.
 */
int deadlock_result =
    Kernel_Mutex_Lock(&mutex_a, &mutex_task);

if (deadlock_result == -4)
{
    printf("Step 12: Circular deadlock correctly detected\n");

    /*
     * Step 13: recover from the deadlock.
     */
    if (Kernel_Mutex_AbortWait(&mutex_a, &mutex_task) == 0)
    {
        printf("Step 13: Deadlock wait successfully aborted\n");

        if (mutex_task.waiting_mutex == NULL &&
            mutex_task.mutex_waiting == 0 &&
            mutex_task.state == TASK_READY)
        {
            printf("Step 13: Task successfully recovered\n");
        }
        else
        {
            printf("Step 13: ERROR - Task recovery state invalid\n");
        }
    /*
 * Step 14: Validate mutex state after recovery.
 */
int mutex_a_state =
    Kernel_Mutex_ValidateState(&mutex_a);

int mutex_b_state =
    Kernel_Mutex_ValidateState(&mutex_b);

if (mutex_a_state == 0 &&
    mutex_b_state == 0 &&
    mutex_a.locked &&
    mutex_a.owner == &cpu_task &&
    mutex_b.locked &&
    mutex_b.owner == &mutex_task &&
    mutex_a.waiter_count == 0 &&
    mutex_b.waiter_count == 1)
{
    printf("Step 14: Mutex state valid after recovery\n");
}
else
{
    printf("Step 14: ERROR - Mutex state corrupted after recovery\n");
}

    }
    else
    {
        printf("Step 13: ERROR - Deadlock recovery failed\n");
    }
}
else
{
    printf("Step 12: ERROR - Deadlock was NOT detected\n");
}

/*
 * Clean up the intentional deadlock test.
 */

printf("\n=== STEP 15: MULTIPLE WAITER PRIORITY INHERITANCE TEST ===\n");

KernelMutex mutex_c;

Kernel_Mutex_Init(&mutex_c);

/*
 * Reset the test tasks.
 */
cpu_task.state = TASK_READY;
sensor_task.state = TASK_READY;
mutex_task.state = TASK_READY;

cpu_task.priority = cpu_task.base_priority;
sensor_task.priority = sensor_task.base_priority;
mutex_task.priority = mutex_task.base_priority;

cpu_task.mutex_waiting = 0;
sensor_task.mutex_waiting = 0;
mutex_task.mutex_waiting = 0;

cpu_task.waiting_mutex = NULL;
sensor_task.waiting_mutex = NULL;
mutex_task.waiting_mutex = NULL;

/*
 * CPUTask owns Mutex C.
 */
Kernel_Mutex_Lock(&mutex_c, &cpu_task);

/*
 * SensorTask waits for Mutex C.
 * Expected owner priority: 1 -> 2
 */

Kernel_Mutex_Lock(&mutex_c, &sensor_task);

if (cpu_task.priority == sensor_task.priority)
{
    printf("Step 15: First priority inheritance correct\n");
}
else
{
    printf("Step 15: ERROR - First priority inheritance failed\n");
}

/*
 * MutexTask also waits.
 *
 * SensorTask has priority 3.
 * MutexTask has priority 2.
 *
 * Therefore CPUTask must remain at
 * the highest waiter priority: 3.
 */
Kernel_Mutex_Lock(&mutex_c, &mutex_task);

if (cpu_task.priority ==
    (sensor_task.priority > mutex_task.priority ?
     sensor_task.priority :
     mutex_task.priority))
{
    printf("Step 15: Highest waiter priority retained correctly\n");
}
else
{
    printf("Step 15: ERROR - Highest waiter priority handling failed\n");
}

/*
 * Remove the highest-priority waiter.
 * Owner should fall from priority 3 -> 2.
 */
if (Kernel_Mutex_RemoveWaiter(
        &mutex_c,
        &mutex_task) == 0)
{
    if (cpu_task.priority == sensor_task.priority)
    {
        printf("Step 15: Priority correctly recalculated after waiter removal\n");
    }
    else
    {
        printf("Step 15: ERROR - Priority recalculation failed\n");
    }
}
else
{
    printf("Step 15: ERROR - Highest-priority waiter removal failed\n");
}

/*
 * Remove the remaining waiter.
 * Owner should return to base priority 1.
 */
if (Kernel_Mutex_RemoveWaiter(
        &mutex_c,
        &sensor_task) == 0)
{
    if (cpu_task.priority == cpu_task.base_priority)
    {
        printf("Step 15: Priority correctly restored to base priority\n");
    }
    else
    {
        printf("Step 15: ERROR - Base priority restoration failed\n");
    }
}
else
{
    printf("Step 15: ERROR - Remaining waiter removal failed\n");
}

/*
 * Final mutex consistency check.
 */
if (Kernel_Mutex_ValidateState(&mutex_c) == 0 &&
    mutex_c.locked &&
    mutex_c.owner == &cpu_task &&
    mutex_c.waiter_count == 0)
{
    printf("Step 15: Multiple-waiter mutex state valid\n");
}
else
{
    printf("Step 15: ERROR - Multiple-waiter mutex state invalid\n");
}

printf("\n=== STEP 16: MUTEX-SCHEDULER PRIORITY INTEGRATION TEST ===\n");

TaskRegistry step16_registry;

Scheduler_Init(&step16_registry);

Scheduler_AddTask(&step16_registry, &cpu_task);
Scheduler_AddTask(&step16_registry, &sensor_task);

KernelMutex mutex_d;

Kernel_Mutex_Init(&mutex_d);

/*
 * Reset task scheduling state.
 */
cpu_task.state = TASK_READY;
sensor_task.state = TASK_READY;

cpu_task.priority = cpu_task.base_priority;
sensor_task.priority = sensor_task.base_priority;

cpu_task.mutex_waiting = 0;
sensor_task.mutex_waiting = 0;

cpu_task.waiting_mutex = NULL;
sensor_task.waiting_mutex = NULL;

/*
 * CPUTask owns Mutex D.
 */
if (Kernel_Mutex_Lock(&mutex_d, &cpu_task) == 0)
{
    printf("Step 16: CPUTask acquired mutex\n");
}
else
{
    printf("Step 16: ERROR - CPUTask failed to acquire mutex\n");
}

/*
 * SensorTask requests the same mutex.
 *
 * SensorTask priority = 3.
 * CPUTask base priority = 1.
 *
 * SensorTask must become blocked and
 * CPUTask must inherit priority 3.
 */

int sensor_lock_result =
    Kernel_Mutex_Lock(&mutex_d, &sensor_task);

if (sensor_lock_result > 0 &&
    sensor_task.state == TASK_BLOCKED &&
    sensor_task.mutex_waiting &&
    sensor_task.waiting_mutex == &mutex_d)
{
    printf("Step 16: SensorTask mutex request processed\n");
}
else
{
    printf("Step 16: ERROR - SensorTask mutex request failed\n");
}

if (sensor_task.state == TASK_BLOCKED &&
    sensor_task.mutex_waiting &&
    sensor_task.waiting_mutex == &mutex_d &&
    cpu_task.priority == sensor_task.priority)
{
    printf("Step 16: Blocking and priority inheritance correct\n");
}
else
{
    printf("Step 16: ERROR - Blocking or priority inheritance invalid\n");
}

/*
 * Scheduler should continue with CPUTask because
 * SensorTask is blocked.
 */
step16_registry.current_task = &sensor_task;
sensor_task.state = TASK_BLOCKED;

cpu_task.state = TASK_READY;

step16_registry.reschedule_pending = 1;
step16_registry.reschedule_reason = RESCHEDULE_BLOCK;

TaskControlBlock* selected_task =
    Scheduler_SelectNext(&step16_registry);

if (selected_task == &cpu_task)
{
    printf("Step 16: Scheduler correctly selected mutex owner\n");
}
else
{
    printf("Step 16: ERROR - Scheduler selected wrong task\n");
}

/*
 * CPUTask releases the mutex.
 *
 * Ownership should transfer to SensorTask,
 * which becomes READY.
 */
if (Kernel_Mutex_Unlock(&mutex_d, &cpu_task) == 0)
{
    printf("Step 16: Mutex released successfully\n");
}
else
{
    printf("Step 16: ERROR - Mutex release failed\n");
}

if (mutex_d.owner == &sensor_task &&
    sensor_task.state == TASK_READY &&
    sensor_task.mutex_waiting == 0 &&
    sensor_task.waiting_mutex == NULL)
{
    printf("Step 16: Waiting task correctly awakened\n");
}
else
{
    printf("Step 16: ERROR - Waiting task was not awakened correctly\n");
}

/*
 * SensorTask is now READY with priority 3.
 * CPUTask is READY with priority 1.
 *
 * Scheduler must select SensorTask.
 */
step16_registry.current_task = &cpu_task;
cpu_task.state = TASK_RUNNING;
sensor_task.state = TASK_READY;

step16_registry.reschedule_pending = 1;
step16_registry.reschedule_reason = RESCHEDULE_PRIORITY;

selected_task = Scheduler_SelectNext(&step16_registry);

if (selected_task == &sensor_task)
{
    printf("Step 16: Scheduler correctly selected awakened high-priority task\n");
}
else
{
    printf("Step 16: ERROR - Scheduler failed priority rescheduling\n");
}

/*
 * Final mutex consistency check.
 */
if (Kernel_Mutex_ValidateState(&mutex_d) == 0 &&
    mutex_d.locked &&
    mutex_d.owner == &sensor_task &&
    mutex_d.waiter_count == 0)
{
    printf("Step 16: Final mutex-scheduler state valid\n");
}
else
{
    printf("Step 16: ERROR - Final mutex-scheduler state invalid\n");
}

printf("\n=== STEP 17.5: TASK CONTEXT VALIDATION TEST ===\n");

int context_result =
    Task_ValidateContext(&cpu_task);

if (context_result == 0)
{
    printf("Step 17.5: CPUTask context is valid\n");
}
else
{
    printf("Step 17.5: ERROR - CPUTask context validation failed: %d\n",
           context_result);
}

printf("\n=== STEP 17.5A: INVALID STACK POINTER TEST ===\n");

uintptr_t valid_saved_sp = cpu_task.context.sp;

cpu_task.context.sp =
    (uintptr_t)&cpu_task.stack[0] - sizeof(uint32_t);

int invalid_low_sp_result =
    Task_ValidateContext(&cpu_task);

/* Restore the real stack pointer immediately. */
cpu_task.context.sp = valid_saved_sp;

if (invalid_low_sp_result == -5)
{
    printf("Step 17.5A: Out-of-bounds stack pointer correctly rejected\n");
}
else
{
    printf("Step 17.5A: ERROR - Invalid stack pointer was not rejected: %d\n",
           invalid_low_sp_result);
}

printf("\n=== STEP 17.5B: STACK GUARD CORRUPTION TEST ===\n");

uint32_t original_guard = cpu_task.stack[0];

/* Deliberately corrupt one guard word. */
cpu_task.stack[0] = 0;

int guard_result = Task_ValidateContext(&cpu_task);

/* Restore the guard immediately. */
cpu_task.stack[0] = original_guard;

if (guard_result == -7)
{
    printf("Step 17.5B: Corrupted stack guard correctly detected\n");
}
else
{
    printf("Step 17.5B: ERROR - Stack guard corruption not detected: %d\n",
           guard_result);
}

printf("\n=== STEP 17.5C: STACK HIGH-WATER TEST ===\n");

size_t stack_high_water =
    Task_GetStackHighWaterUsage(&cpu_task);

if (stack_high_water == 16U * sizeof(uint32_t))
{
    printf("Step 17.5C: Initial stack high-water estimate valid (%zu bytes)\n",
           stack_high_water);
}
else
{
    printf("Step 17.5C: ERROR - Unexpected stack high-water estimate: %zu bytes\n",
           stack_high_water);
}

printf("\n=== STEP 17.5D: STACK WATERMARK UPDATE TEST ===\n");

uintptr_t original_context_sp = cpu_task.context.sp;
uintptr_t original_stack_low_watermark =
    cpu_task.stack_low_watermark;

/* Simulate a deeper, aligned saved SP, 96 bytes from stack end. */
cpu_task.context.sp =
    (uintptr_t)&cpu_task.stack[TASK_STACK_SIZE - 24];

Task_UpdateStackHighWater(&cpu_task);

size_t deeper_stack_usage =
    Task_GetStackHighWaterUsage(&cpu_task);

/* Restore the original test state immediately. */
cpu_task.context.sp = original_context_sp;
cpu_task.stack_low_watermark = original_stack_low_watermark;

if (deeper_stack_usage == 96U)
{
    printf("Step 17.5D: Deeper stack watermark correctly measured (%zu bytes)\n",
           deeper_stack_usage);
}
else
{
    printf("Step 17.5D: ERROR - Unexpected watermark: %zu bytes\n",
           deeper_stack_usage);
}

printf("\n=== STEP 17.5E: STACK WARNING API TEST ===\n");

uintptr_t warning_original_sp = cpu_task.context.sp;
uintptr_t warning_original_watermark = cpu_task.stack_low_watermark;

/* 752 bytes is below the 756-byte warning threshold. */
cpu_task.context.sp =
    (uintptr_t)&cpu_task.stack[TASK_STACK_SIZE - 188];
Task_UpdateStackHighWater(&cpu_task);
int warning_below_threshold =
    Task_IsStackUsageWarning(&cpu_task);

/* 760 bytes is the first 8-byte-aligned usage above 756 bytes. */
cpu_task.context.sp =
    (uintptr_t)&cpu_task.stack[TASK_STACK_SIZE - 190];
Task_UpdateStackHighWater(&cpu_task);
int warning_at_threshold =
    Task_IsStackUsageWarning(&cpu_task);
size_t warning_high_usage =
    Task_GetStackHighWaterUsage(&cpu_task);

/* Restore the original state before continuing other tests. */
cpu_task.context.sp = warning_original_sp;
cpu_task.stack_low_watermark = warning_original_watermark;

if (warning_below_threshold == 0)
{
    printf("Step 17.5E: Below-threshold usage correctly ignored (752 bytes)\n");
}
else
{
    printf("Step 17.5E: ERROR - Below-threshold result was %d\n",
           warning_below_threshold);
}

if (warning_at_threshold == 1 && warning_high_usage >= 756U)
{
    printf("Step 17.5E: Stack warning correctly raised (%zu bytes)\n",
           warning_high_usage);
}
else
{
    printf("Step 17.5E: ERROR - Warning result=%d, usage=%zu bytes\n",
           warning_at_threshold, warning_high_usage);
}

if (Task_IsStackUsageWarning(NULL) == -1)
{
    printf("Step 17.5E: Invalid task input correctly rejected\n");
}
else
{
    printf("Step 17.5E: ERROR - Invalid input was not rejected\n");
}


printf("\n=== STEP 17.5G: TASK CONTEXT RESTART TEST ===\n");

TaskControlBlock restart_probe;
Task_Init(&restart_probe, 77, 4, "RestartProbe", CPU_Task);

/* Preserve test metadata and configure ARM-style initial frame values. */
restart_probe.initial_stack_argument = (uintptr_t)0x12345678UL;
restart_probe.initial_stack_return_handler = (uintptr_t)0x87654320UL;
restart_probe.restart_count = 3UL;
restart_probe.successful_recoveries = 2UL;

/* Simulate stale runtime measurements and a previously reported warning. */
restart_probe.context.sp =
    (uintptr_t)&restart_probe.stack[TASK_STACK_SIZE - 32];
restart_probe.stack_low_watermark = restart_probe.context.sp;
restart_probe.stack_warning_reported = 1;

/* Save a known fill value, then damage the guard and a stack word. */
uint32_t expected_refilled_word = restart_probe.stack[20];
restart_probe.stack[0] = 0U;
restart_probe.stack[20] = 0xDEADBEEFUL;

int restart_result = Task_RestartContext(&restart_probe);

if (restart_result == 0 &&
    Task_ValidateContext(&restart_probe) == 0 &&
    restart_probe.context.sp ==
        (uintptr_t)&restart_probe.stack[TASK_STACK_SIZE - 16] &&
    restart_probe.stack_low_watermark == restart_probe.context.sp &&
    restart_probe.stack_warning_reported == 0 &&
    restart_probe.stack[20] == expected_refilled_word &&
    restart_probe.stack[TASK_STACK_SIZE - 8] == 0x12345678UL &&
    restart_probe.stack[TASK_STACK_SIZE - 3] == 0x87654320UL &&
    restart_probe.stack[TASK_STACK_SIZE - 2] ==
        (uint32_t)(uintptr_t)CPU_Task &&
    restart_probe.stack[TASK_STACK_SIZE - 1] == 0x01000000UL &&
    restart_probe.id == 77 &&
    restart_probe.priority == 4 &&
    restart_probe.restart_count == 3UL &&
    restart_probe.successful_recoveries == 2UL)
{
    printf("Step 17.5G: Stack context rebuilt and metadata preserved\n");
}
else
{
    printf("Step 17.5G: ERROR - Context restart verification failed "
           "(result=%d, validation=%d)\n",
           restart_result, Task_ValidateContext(&restart_probe));
}

if (Task_RestartContext(NULL) == -1)
{
    printf("Step 17.5G: Null task input correctly rejected\n");
}
else
{
    printf("Step 17.5G: ERROR - Null task input was not rejected\n");
}


printf("\n=== STEP 17.5H: SCHEDULER RECOVERY CONTEXT TEST ===\n");

TaskControlBlock recovery_probe;
TaskRegistry recovery_probe_registry;
Scheduler_Init(&recovery_probe_registry);
Task_Init(&recovery_probe, 78, 2, "RecoveryProbe", CPU_Task);

if (Scheduler_AddTask(&recovery_probe_registry, &recovery_probe) == 0)
{
    recovery_probe.initial_stack_argument = (uintptr_t)0x12345678UL;
    recovery_probe.initial_stack_return_handler = (uintptr_t)0x23456789UL;
    recovery_probe.state = TASK_FAULTED;
    recovery_probe.fault_type = FAULT_RUNTIME;
    recovery_probe.stack_warning_reported = 1;
    recovery_probe.stack_low_watermark =
        (uintptr_t)&recovery_probe.stack[TASK_STACK_SIZE - 32];
    uint32_t recovery_expected_fill_word = recovery_probe.stack[20];
    recovery_probe.stack[0] = 0U;
    recovery_probe.stack[20] = 0xDEADBEEFUL;
    recovery_probe.stack[TASK_STACK_SIZE - 2] = 0U;

    Scheduler_RecoverFaultedTask(
        &recovery_probe_registry, &recovery_probe);

    if (recovery_probe.state == TASK_READY &&
        recovery_probe.recovery_pending == 1 &&
        recovery_probe.recovery_in_progress == 0 &&
        recovery_probe.recovery_failed == 0 &&
        recovery_probe.recovery_attempts == 1UL &&
        recovery_probe.restart_count == 1UL &&
        recovery_probe_registry.recovery_events == 1UL &&
        Task_ValidateContext(&recovery_probe) == 0 &&
        recovery_probe.context.sp ==
            (uintptr_t)&recovery_probe.stack[TASK_STACK_SIZE - 16] &&
        recovery_probe.stack_low_watermark == recovery_probe.context.sp &&
        recovery_probe.stack_warning_reported == 0 &&
        recovery_probe.stack[0] != 0U &&
        recovery_probe.stack[20] == recovery_expected_fill_word &&
        recovery_probe.stack[TASK_STACK_SIZE - 8] == 0x12345678UL &&
        recovery_probe.stack[TASK_STACK_SIZE - 3] == 0x23456789UL &&
        recovery_probe.stack[TASK_STACK_SIZE - 2] ==
            (uint32_t)(uintptr_t)CPU_Task &&
        recovery_probe.stack[TASK_STACK_SIZE - 1] == 0x01000000UL)
    {
        printf("Step 17.5H: Scheduler recovery rebuilt context before READY\n");
    }
    else
    {
        printf("Step 17.5H: ERROR - Scheduler recovery context verification failed\n");
    }
}
else
{
    printf("Step 17.5H: ERROR - Recovery probe registration failed\n");
}

printf("\n=== STEP 17.5I: FAILED CONTEXT RESTART SAFETY TEST ===\n");

TaskControlBlock failed_recovery_probe;
TaskRegistry failed_recovery_registry;
Scheduler_Init(&failed_recovery_registry);
Task_Init(&failed_recovery_probe, 79, 2, "FailedRecoveryProbe", CPU_Task);

if (Scheduler_AddTask(&failed_recovery_registry, &failed_recovery_probe) == 0)
{
    failed_recovery_probe.task_function = NULL;
    failed_recovery_probe.state = TASK_FAULTED;
    failed_recovery_probe.fault_type = FAULT_RUNTIME;

    Scheduler_RecoverFaultedTask(
        &failed_recovery_registry, &failed_recovery_probe);

    if (failed_recovery_probe.state == TASK_FAULTED &&
        failed_recovery_probe.recovery_pending == 0 &&
        failed_recovery_probe.recovery_in_progress == 0 &&
        failed_recovery_probe.recovery_failed == 1 &&
        failed_recovery_registry.reschedule_pending == 1 &&
        failed_recovery_registry.reschedule_reason == RESCHEDULE_FAULT)
    {
        printf("Step 17.5I: Failed restart kept task faulted\n");
    }
    else
    {
        printf("Step 17.5I: ERROR - Failed restart safety check failed\n");
    }
}
else
{
    printf("Step 17.5I: ERROR - Failure probe registration failed\n");
}


printf("\n=== STEP 17.5J: SCHEDULER RESOURCE CLEANUP RECOVERY TEST ===\n");

TaskControlBlock resource_recovery_task;
TaskControlBlock resource_recovery_waiter;
TaskControlBlock resource_recovery_owner;
TaskRegistry resource_recovery_registry;
KernelMutex resource_recovery_owned_mutex;
KernelMutex resource_recovery_waiting_mutex;

Scheduler_Init(&resource_recovery_registry);

Task_Init(&resource_recovery_task, 85, 3,
          "ResourceRecoveryTask", CPU_Task);
Task_Init(&resource_recovery_waiter, 86, 4,
          "ResourceRecoveryWaiter", CPU_Task);
Task_Init(&resource_recovery_owner, 87, 1,
          "ResourceRecoveryOwner", CPU_Task);

Kernel_Mutex_Init(&resource_recovery_owned_mutex);
Kernel_Mutex_Init(&resource_recovery_waiting_mutex);

int resource_ipc_waiting_before = Kernel_IPC_GetWaitingCount();

int resource_setup_ok =
    Scheduler_AddTask(&resource_recovery_registry,
                      &resource_recovery_task) == 0 &&
    Kernel_Mutex_Lock(&resource_recovery_owned_mutex,
                      &resource_recovery_task) == 0 &&
    Kernel_Mutex_Lock(&resource_recovery_owned_mutex,
                      &resource_recovery_waiter) == 1 &&
    Kernel_Mutex_Lock(&resource_recovery_waiting_mutex,
                      &resource_recovery_owner) == 0 &&
    Kernel_Mutex_Lock(&resource_recovery_waiting_mutex,
                      &resource_recovery_task) == 1;

if (!resource_setup_ok)
{
    printf("Step 17.5J: ERROR - Resource recovery setup failed\n");
    return 1;
}

/* Register the task as an IPC receiver and establish stale wait metadata. */
resource_recovery_task.state = TASK_RUNNING;
Kernel_IPC_RegisterReceiver(&resource_recovery_task);

if (Kernel_IPC_GetWaitingCount() != resource_ipc_waiting_before + 1)
{
    printf("Step 17.5J: ERROR - IPC receiver registration failed\n");
    return 1;
}

Kernel_IPC_Wait(&resource_recovery_task);
resource_recovery_task.wake_reason = WAKE_TIMEOUT;
resource_recovery_task.ipc_timeout_tick = Scheduler_GetTick() + 99;
resource_recovery_task.state = TASK_FAULTED;
resource_recovery_task.fault_type = FAULT_RUNTIME;

Scheduler_RecoverFaultedTask(
    &resource_recovery_registry, &resource_recovery_task);

/* Recovery must clean resources before making the task READY again. */
if (resource_recovery_task.state == TASK_READY &&
    resource_recovery_task.recovery_pending == 1 &&
    resource_recovery_task.recovery_in_progress == 0 &&
    resource_recovery_task.recovery_failed == 0 &&
    resource_recovery_task.recovery_attempts == 1UL &&
    resource_recovery_task.restart_count == 1UL &&
    resource_recovery_registry.recovery_events == 1UL &&
    Task_ValidateContext(&resource_recovery_task) == 0 &&
    resource_recovery_task.owned_mutexes == NULL &&
    resource_recovery_task.waiting_mutex == NULL &&
    resource_recovery_task.mutex_waiting == 0 &&
    resource_recovery_task.mutex_hold_active == 0 &&
    resource_recovery_task.wake_reason == WAKE_NONE &&
    resource_recovery_task.ipc_timeout_tick == 0 &&
    Kernel_IPC_GetWaitingCount() == resource_ipc_waiting_before &&
    resource_recovery_owned_mutex.owner == &resource_recovery_waiter &&
    resource_recovery_task.owned_mutexes == NULL &&
    resource_recovery_waiter.owned_mutexes ==
        &resource_recovery_owned_mutex &&
    resource_recovery_waiting_mutex.waiter_count == 0 &&
    resource_recovery_waiting_mutex.owner == &resource_recovery_owner &&
    resource_recovery_owner.priority ==
        resource_recovery_owner.base_priority &&
    resource_recovery_waiter.state == TASK_READY &&
    Kernel_Mutex_ValidateState(&resource_recovery_owned_mutex) == 0 &&
    Kernel_Mutex_ValidateState(&resource_recovery_waiting_mutex) == 0)
{
    printf("Step 17.5J: Scheduler recovery cleaned mutex and IPC resources\n");
}
else
{
    printf("Step 17.5J: ERROR - Resource recovery integration check failed\n");
    return 1;
}

printf("\n=== STEP 17.5K: DUPLICATE RECOVERY RESERVATION TEST ===\n");

TaskControlBlock duplicate_recovery_probe;
TaskRegistry duplicate_recovery_registry;

Scheduler_Init(&duplicate_recovery_registry);

Task_Init(
    &duplicate_recovery_probe,
    88,
    2,
    "DuplicateRecoveryProbe",
    CPU_Task
);

if (Scheduler_AddTask(
        &duplicate_recovery_registry,
        &duplicate_recovery_probe) == 0)
{
    /* Simulate another recovery service already owning this task. */
    duplicate_recovery_probe.state = TASK_FAULTED;
    duplicate_recovery_probe.fault_type = FAULT_RUNTIME;
    duplicate_recovery_probe.recovery_in_progress = 1;
    duplicate_recovery_probe.recovery_pending = 0;
    duplicate_recovery_probe.recovery_attempts = 1UL;
    duplicate_recovery_probe.restart_count = 1UL;
    duplicate_recovery_probe.recovery_failed = 0;
    duplicate_recovery_registry.recovery_events = 1UL;

    Scheduler_ProcessFaultRecovery(&duplicate_recovery_registry);

    if (duplicate_recovery_probe.state == TASK_FAULTED &&
        duplicate_recovery_probe.recovery_in_progress == 1 &&
        duplicate_recovery_probe.recovery_pending == 0 &&
        duplicate_recovery_probe.recovery_attempts == 1UL &&
        duplicate_recovery_probe.restart_count == 1UL &&
        duplicate_recovery_probe.recovery_failed == 0 &&
        duplicate_recovery_registry.recovery_events == 1UL)
    {
        printf("Step 17.5K: PASS - Duplicate recovery request was rejected\n");
    }
    else
    {
        printf("Step 17.5K: ERROR - Duplicate recovery changed task state\n");
    }
}
else
{
    printf("Step 17.5K: ERROR - Duplicate recovery probe registration failed\n");
}


printf("\n=== STEP 17.5L: MUTEX CLEANUP FAILURE RESERVATION TEST ===\n");

TaskControlBlock mutex_cleanup_failure_probe;
TaskControlBlock mutex_cleanup_failure_unrelated;
TaskRegistry mutex_cleanup_failure_registry;
KernelMutex mutex_cleanup_failure_mutex;

Scheduler_Init(&mutex_cleanup_failure_registry);

Task_Init(
    &mutex_cleanup_failure_probe,
    89,
    2,
    "MutexCleanupFailureProbe",
    CPU_Task
);

Task_Init(
    &mutex_cleanup_failure_unrelated,
    90,
    2,
    "MutexCleanupFailureUnrelated",
    CPU_Task
);

Kernel_Mutex_Init(&mutex_cleanup_failure_mutex);

if (Scheduler_AddTask(
        &mutex_cleanup_failure_registry,
        &mutex_cleanup_failure_probe) == 0)
{
    /*
     * Deliberately make the mutex waiter list omit the faulted task.
     * Cleanup should return -3 without removing or changing this entry.
     */
    mutex_cleanup_failure_probe.state = TASK_FAULTED;
    mutex_cleanup_failure_probe.fault_type = FAULT_RUNTIME;
    mutex_cleanup_failure_probe.waiting_mutex =
        &mutex_cleanup_failure_mutex;
    mutex_cleanup_failure_probe.mutex_waiting = 1;

    mutex_cleanup_failure_mutex.waiter_count = 1;
    mutex_cleanup_failure_mutex.waiters[0] =
        &mutex_cleanup_failure_unrelated;

    Scheduler_ProcessFaultRecovery(&mutex_cleanup_failure_registry);

    if (mutex_cleanup_failure_probe.state == TASK_FAULTED &&
        mutex_cleanup_failure_probe.recovery_pending == 0 &&
        mutex_cleanup_failure_probe.recovery_in_progress == 0 &&
        mutex_cleanup_failure_probe.recovery_failed == 1 &&
        mutex_cleanup_failure_probe.recovery_attempts == 1UL &&
        mutex_cleanup_failure_probe.restart_count == 1UL &&
        mutex_cleanup_failure_registry.recovery_events == 1UL &&
        mutex_cleanup_failure_registry.reschedule_pending == 1 &&
        mutex_cleanup_failure_registry.reschedule_reason ==
            RESCHEDULE_FAULT)
    {
        printf("Step 17.5L: PASS - Mutex cleanup failure released recovery reservation\n");
    }
    else
    {
        printf("Step 17.5L: ERROR - Mutex cleanup failure left incorrect recovery state\n");
    }
}
else
{
    printf("Step 17.5L: ERROR - Mutex cleanup failure probe registration failed\n");
}


printf("\n=== STEP 17.5M: INVALID MUTEX WAITER COUNT RECOVERY TEST ===\n");

TaskControlBlock invalid_count_recovery_probe;
TaskControlBlock invalid_count_recovery_unrelated;
TaskRegistry invalid_count_recovery_registry;
KernelMutex invalid_count_recovery_mutex;

Scheduler_Init(&invalid_count_recovery_registry);

Task_Init(
    &invalid_count_recovery_probe,
    91,
    2,
    "InvalidCountRecoveryProbe",
    CPU_Task
);

Task_Init(
    &invalid_count_recovery_unrelated,
    92,
    2,
    "InvalidCountRecoveryUnrelated",
    CPU_Task
);

Kernel_Mutex_Init(&invalid_count_recovery_mutex);

if (Scheduler_AddTask(
        &invalid_count_recovery_registry,
        &invalid_count_recovery_probe) == 0)
{
    /*
     * The count exceeds the allocated waiter-array capacity.
     * Cleanup must reject it before indexing or modifying the array.
     */
    invalid_count_recovery_probe.state = TASK_FAULTED;
    invalid_count_recovery_probe.fault_type = FAULT_RUNTIME;
    invalid_count_recovery_probe.waiting_mutex =
        &invalid_count_recovery_mutex;
    invalid_count_recovery_probe.mutex_waiting = 1;

    invalid_count_recovery_mutex.waiters[0] =
        &invalid_count_recovery_unrelated;
    invalid_count_recovery_mutex.waiter_count =
        MUTEX_MAX_WAITERS + 1;

    Scheduler_ProcessFaultRecovery(&invalid_count_recovery_registry);

    if (invalid_count_recovery_probe.state == TASK_FAULTED &&
        invalid_count_recovery_probe.recovery_pending == 0 &&
        invalid_count_recovery_probe.recovery_in_progress == 0 &&
        invalid_count_recovery_probe.recovery_failed == 1 &&
        invalid_count_recovery_probe.recovery_attempts == 1UL &&
        invalid_count_recovery_probe.restart_count == 1UL &&
        invalid_count_recovery_registry.recovery_events == 1UL &&
        invalid_count_recovery_registry.reschedule_pending == 1 &&
        invalid_count_recovery_registry.reschedule_reason ==
            RESCHEDULE_FAULT &&
        invalid_count_recovery_probe.waiting_mutex ==
            &invalid_count_recovery_mutex &&
        invalid_count_recovery_probe.mutex_waiting == 1 &&
        invalid_count_recovery_mutex.waiter_count ==
            MUTEX_MAX_WAITERS + 1 &&
        invalid_count_recovery_mutex.waiters[0] ==
            &invalid_count_recovery_unrelated)
    {
        printf("Step 17.5M: PASS - Invalid waiter count rejected and recovery reservation released\n");
    }
    else
    {
        printf("Step 17.5M: ERROR - Invalid waiter count recovery state was incorrect\n");
    }
}
else
{
    printf("Step 17.5M: ERROR - Invalid waiter count probe registration failed\n");
}

printf("\n=== STEP 17.6: INITIAL STACK FRAME VALIDATION TEST ===\n");

if (cpu_task.stack[TASK_STACK_SIZE - 8] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 7] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 6] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 5] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 4] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 3] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 2] ==
        (uint32_t)(uintptr_t)cpu_task.task_function &&
    cpu_task.stack[TASK_STACK_SIZE - 1] == 0x01000000UL)
{
    printf("Step 17.6: Initial Cortex-M stack frame is valid\n");
}
else
{
    printf("Step 17.6: ERROR - Initial stack frame is invalid\n");
}

printf("\n=== STEP 17.7: SAVED SP FRAME VALIDATION TEST ===\n");

if (cpu_task.context.sp ==
    (uintptr_t)&cpu_task.stack[TASK_STACK_SIZE - 16])
{
    printf("Step 17.7: Saved SP correctly points to stack frame\n");
}
else
{
    printf("Step 17.7: ERROR - Saved SP does not point to stack frame\n");
}

printf("\n=== STEP 17.8: ACTUAL STACK FRAME VALIDATION TEST ===\n");

uint32_t* stack_frame =
    &cpu_task.stack[TASK_STACK_SIZE - 8];

if (stack_frame[0] == 0 &&
    stack_frame[1] == 0 &&
    stack_frame[2] == 0 &&
    stack_frame[3] == 0 &&
    stack_frame[4] == 0 &&
    stack_frame[5] == 0 &&
    stack_frame[6] ==
        (uint32_t)(uintptr_t)cpu_task.task_function &&
    stack_frame[7] == 0x01000000UL)
{
    printf("Step 17.8: Actual task stack frame is valid\n");
}
else
{
    printf("Step 17.8: ERROR - Actual task stack frame is invalid\n");
}

printf("\n=== STEP 17.9: SOFTWARE CONTEXT VALIDATION TEST ===\n");

if (cpu_task.stack[TASK_STACK_SIZE - 16] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 15] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 14] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 13] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 12] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 11] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 10] == 0 &&
    cpu_task.stack[TASK_STACK_SIZE - 9] == 0)
{
    printf("Step 17.9: Software-saved registers initialized correctly\n");
}
else
{
    printf("Step 17.9: ERROR - Software context initialization failed\n");
}

printf("\n=== STEP 17.11: CONTEXT TYPE-WIDTH TEST ===\n");

if (sizeof(uint32_t) == 4 &&
    sizeof(cpu_task.stack[0]) == sizeof(uint32_t) &&
    sizeof(cpu_task.stack[0]) == 4 &&
    8 * sizeof(uint32_t) == 32)
{
    printf("Step 17.11: Cortex-M register widths are correct\n");
}
else
{
    printf("Step 17.11: ERROR - Context type-width mismatch\n");
}


printf("\n=== STEP 17.12: STACK ALIGNMENT TEST ===\n");

uintptr_t stack_base = (uintptr_t)&cpu_task.stack[0];
uintptr_t saved_sp = cpu_task.context.sp;
uintptr_t stack_end = (uintptr_t)&cpu_task.stack[TASK_STACK_SIZE];
uintptr_t frame_start =
    (uintptr_t)&cpu_task.stack[TASK_STACK_SIZE - 16];

if ((stack_base % 8U) == 0U &&
    (saved_sp % 8U) == 0U &&
    saved_sp == frame_start &&
    saved_sp + 16U * sizeof(uint32_t) <= stack_end)
{
    printf("Step 17.12: Stack alignment and frame boundaries are valid\n");
}
else
{
    printf("Step 17.12: ERROR - Stack alignment or frame boundary invalid\n");
}


mutex_a.owner = &cpu_task;
mutex_a.locked = 1;
mutex_b.owner = &mutex_task;
mutex_b.locked = 1;

mutex_a.waiter_count = 0;
mutex_b.waiter_count = 0;

cpu_task.waiting_mutex = NULL;
mutex_task.waiting_mutex = NULL;

cpu_task.mutex_waiting = 0;
mutex_task.mutex_waiting = 0;

cpu_task.state = TASK_READY;
mutex_task.state = TASK_READY;

Kernel_Mutex_Unlock(&mutex_a, &cpu_task);
Kernel_Mutex_Unlock(&mutex_b, &mutex_task);

 cpu_task.state = TASK_READY;

    mutex_task.state = TASK_BLOCKED;
    mutex_task.wakeup_tick = 2;

    sensor_task.state = TASK_BLOCKED;
    sensor_task.wakeup_tick = 3;

 Scheduler_AddTask(&registry, &sensor_task);
 Scheduler_AddTask(&registry, &communication_task);
 Scheduler_AddTask(&registry, &mutex_task);
 Scheduler_AddTask(&registry, &cpu_task);
 Scheduler_AddTask(&registry, &idle_task);

sensor_task.state = TASK_BLOCKED;
sensor_task.wakeup_tick = 3;

communication_task.state = TASK_BLOCKED;
communication_task.wakeup_tick = 50;

mutex_task.state = TASK_BLOCKED;
mutex_task.wakeup_tick = 2;

cpu_task.state = TASK_READY;
cpu_task.wakeup_tick = 0;

 registry.idle_task = &idle_task;

printf("Testing Kernel_Run...\n");
printf("Initial IPC Queue Count: %d\n", Kernel_IPC_GetCount());

 Kernel_Run(&registry);

printf("\n=== STEP 18: DISABLED-PROGRESS WATCHDOG TEST ===\n");

TaskControlBlock watchdog_probe;
TaskRegistry watchdog_probe_registry;

Scheduler_Init(&watchdog_probe_registry);

Task_Init(
    &watchdog_probe,
    99,
    1,
    "WatchdogDisabledProbe",
    Idle_Task
);

watchdog_probe.progress_enabled = 0;

if (Scheduler_AddTask(
        &watchdog_probe_registry,
        &watchdog_probe) == 0)
{
    watchdog_probe.state = TASK_RUNNING;
    watchdog_probe.progress_reported = 0;

    /* Simulate progress becoming stale. */
    watchdog_probe.last_progress_tick =
        Scheduler_GetTick() -
        (TASK_WATCHDOG_THRESHOLD + 1UL);

    watchdog_probe_registry.current_task = &watchdog_probe;

    Scheduler_CheckTaskWatchdog(&watchdog_probe_registry);

    if (watchdog_probe.state == TASK_RUNNING &&
        watchdog_probe.fault_count == 0)
    {
        printf("Step 18: Watchdog correctly ignored task "
               "with progress disabled\n");
    }
    else
    {
        printf("Step 18: ERROR - Disabled-progress task "
               "was incorrectly faulted\n");
    }
}
else
{
    printf("Step 18: ERROR - Watchdog test task "
           "registration failed\n");
}

 Kernel_Trace_Print();

printf("\n=== STEP 19: ENABLED-PROGRESS WATCHDOG TEST ===\n");

watchdog_probe.progress_enabled = 1;
watchdog_probe.state = TASK_RUNNING;
watchdog_probe.progress_reported = 0;
watchdog_probe.fault_type = FAULT_NONE;
watchdog_probe.fault_count = 0;

watchdog_probe.last_progress_tick =
    Scheduler_GetTick() -
    (TASK_WATCHDOG_THRESHOLD + 1UL);

Scheduler_CheckTaskWatchdog(&watchdog_probe_registry);

if (watchdog_probe.state == TASK_FAULTED &&
    watchdog_probe.fault_type == FAULT_WATCHDOG &&
    watchdog_probe.fault_count == 1)
{
    printf("Step 19: Watchdog correctly detected stalled "
           "task with progress enabled\n");
}
else
{
    printf("Step 19: ERROR - Watchdog failed to detect "
           "stalled enabled task\n");
}

printf("\n=== STEP 19A: HEARTBEAT VS PROGRESS TEST ===\n");

unsigned long progress_before_heartbeat =
    watchdog_probe.progress_count;

unsigned long tick_before_heartbeat =
    watchdog_probe.last_progress_tick;

Scheduler_TaskHeartbeat(&watchdog_probe);

if (watchdog_probe.progress_count == progress_before_heartbeat &&
    watchdog_probe.last_heartbeat_tick == Scheduler_GetTick() &&
    watchdog_probe.last_progress_tick == tick_before_heartbeat)
{
    printf("Step 19A: PASS - Heartbeat refreshes liveness without incrementing progress\n");
}
else
{
    printf("Step 19A: ERROR - Heartbeat/progress separation failed\n");
}

(void)tick_before_heartbeat;

printf("\n=== STEP 20: FRESH HEARTBEAT WATCHDOG TEST ===\n");

watchdog_probe.state = TASK_RUNNING;
watchdog_probe.progress_enabled = 1;
watchdog_probe.progress_reported = 0;
watchdog_probe.fault_type = FAULT_NONE;
watchdog_probe.fault_count = 0;

/* Simulate an old progress timestamp. */
watchdog_probe.last_progress_tick =
    Scheduler_GetTick() -
    (TASK_WATCHDOG_THRESHOLD + 1UL);

/* Report a fresh heartbeat before checking the watchdog. */
Scheduler_TaskHeartbeat(&watchdog_probe);

Scheduler_CheckTaskWatchdog(&watchdog_probe_registry);

if (watchdog_probe.state == TASK_FAULTED &&
    watchdog_probe.fault_type == FAULT_WATCHDOG &&
    watchdog_probe.fault_count == 1 &&
    watchdog_probe.last_heartbeat_tick == Scheduler_GetTick())
{
    printf("Step 20: PASS - Fresh heartbeat did not hide stalled progress\n");
}
else
{
    printf("Step 20: ERROR - Fresh heartbeat test failed\n");
}

printf("\n=== STEP 21: PROGRESS DEADLINE EXPIRY WATCHDOG TEST ===\n");

watchdog_probe.state = TASK_RUNNING;
watchdog_probe.progress_enabled = 1;
watchdog_probe.progress_reported = 0;
watchdog_probe.fault_type = FAULT_NONE;
watchdog_probe.fault_count = 0;

/* Establish a fresh heartbeat baseline. */
Scheduler_TaskHeartbeat(&watchdog_probe);

/* Simulate the heartbeat becoming stale. */
watchdog_probe.last_progress_tick =
    Scheduler_GetTick() -
    (TASK_WATCHDOG_THRESHOLD + 1UL);

watchdog_probe.progress_reported = 0;
Scheduler_CheckTaskWatchdog(&watchdog_probe_registry);

if (watchdog_probe.state == TASK_FAULTED &&
    watchdog_probe.fault_type == FAULT_WATCHDOG &&
    watchdog_probe.fault_count == 1)
{
    printf("Step 21: PASS - Expired progress deadline detected\n");
}
else
{
    printf("Step 21: ERROR - Expired progress deadline not detected\n");
}

printf("\n=== STEP 21A: REPEATED HEARTBEAT WITHOUT PROGRESS TEST ===\n");

/* Reset the probe after Step 21 faulted it. */
watchdog_probe.state = TASK_RUNNING;
watchdog_probe.progress_enabled = 1;
watchdog_probe.fault_type = FAULT_NONE;
watchdog_probe.fault_count = 0;
watchdog_probe.progress_count = 0;
watchdog_probe.progress_reported = 0;
watchdog_probe.execution_units = 0;
watchdog_probe.time_slice = 100UL;
watchdog_probe.watchdog_last_execution_units = 0;
watchdog_probe.last_progress_tick = Scheduler_GetTick();

unsigned long heartbeat_test_progress_start =
    watchdog_probe.progress_count;

/* Send heartbeats without reporting genuine task progress. */
for (int heartbeat_test_i = 0;
     heartbeat_test_i < TASK_WATCHDOG_THRESHOLD + 2;
     heartbeat_test_i++)
{
    Scheduler_Tick(&watchdog_probe_registry);
    Scheduler_TaskHeartbeat(&watchdog_probe);
    Scheduler_CheckTaskWatchdog(&watchdog_probe_registry);
}


if (watchdog_probe.progress_count ==
        heartbeat_test_progress_start &&
    watchdog_probe.state == TASK_FAULTED &&
    watchdog_probe.fault_type == FAULT_WATCHDOG &&
    watchdog_probe.fault_count >= 1)
{
    printf("Step 21A: PASS - Repeated heartbeats did not hide stalled progress\n");
}
else
{
    printf("Step 21A: ERROR - Repeated-heartbeat watchdog assertion failed\n");
}


printf("\n=== STEP 22: RECOVERY VERIFICATION TESTS ===\n");

/* Test A: Recovery succeeds when genuine progress is reported. */
TaskControlBlock recovery_verify_probe;
TaskRegistry recovery_verify_registry;

Scheduler_Init(&recovery_verify_registry);

Task_Init(
    &recovery_verify_probe,
    100,
    1,
    "RecoveryVerifySuccess",
    CPU_Task
);

if (Scheduler_AddTask(
        &recovery_verify_registry,
        &recovery_verify_probe) == 0)
{
    recovery_verify_probe.state = TASK_RUNNING;
    recovery_verify_probe.recovery_pending = 1;
    recovery_verify_probe.recovery_start_progress_count = 4UL;
    recovery_verify_probe.progress_count = 5UL;
    recovery_verify_probe.progress_reported = 1;
    recovery_verify_probe.successful_recoveries = 0UL;
    recovery_verify_probe.recovery_attempts = 1UL;
    recovery_verify_probe.recovery_failed = 0;
    recovery_verify_probe.fault_type = FAULT_WATCHDOG;

    Scheduler_VerifyRecovery(
        &recovery_verify_registry,
        &recovery_verify_probe
    );

    if (recovery_verify_probe.recovery_pending == 0 &&
        recovery_verify_probe.successful_recoveries == 1UL &&
        recovery_verify_probe.recovery_attempts == 0UL &&
        recovery_verify_probe.recovery_failed == 0 &&
        recovery_verify_probe.fault_type == FAULT_NONE &&
        recovery_verify_probe.state == TASK_RUNNING)
    {
        printf("Step 22A: PASS - Genuine progress verified recovery\n");
    }
    else
    {
        printf("Step 22A: ERROR - Successful recovery verification failed\n");
    }
}
else
{
    printf("Step 22A: ERROR - Success probe registration failed\n");
}

/* Test B: Record the current behavior when progress is absent. */
TaskControlBlock no_progress_probe;
TaskRegistry no_progress_registry;

Scheduler_Init(&no_progress_registry);

Task_Init(
    &no_progress_probe,
    101,
    1,
    "RecoveryNoProgress",
    CPU_Task
);

if (Scheduler_AddTask(
        &no_progress_registry,
        &no_progress_probe) == 0)
{
    no_progress_probe.state = TASK_RUNNING;
    no_progress_probe.recovery_pending = 1;
    no_progress_probe.recovery_start_progress_count = 3UL;
    no_progress_probe.progress_count = 3UL;
    no_progress_probe.progress_reported = 0;
    no_progress_probe.recovery_failed = 0;

        no_progress_probe.progress_enabled = 1;
    no_progress_probe.successful_recoveries = 0UL;

    /* Repeated heartbeats must not complete recovery. */
    for (int recovery_heartbeat_i = 0;
         recovery_heartbeat_i < 3;
         recovery_heartbeat_i++)
    {
        Scheduler_TaskHeartbeat(&no_progress_probe);

        Scheduler_VerifyRecovery(
            &no_progress_registry,
            &no_progress_probe
        );
    }

    Scheduler_VerifyRecovery(
        &no_progress_registry,
        &no_progress_probe
    );

            if (no_progress_probe.state == TASK_RUNNING &&
        no_progress_probe.recovery_pending == 1 &&
        no_progress_probe.successful_recoveries == 0UL &&
        no_progress_probe.progress_count ==
            no_progress_probe.recovery_start_progress_count &&
        no_progress_probe.recovery_failed == 0 &&
        no_progress_registry.reschedule_pending == 0)
    {
        printf("Step 22B: PASS - Recovery remains pending before progress\n");
    }
    else
    {
        printf("Step 22B: ERROR - Recovery failed before progress was allowed\n");
    }
}
else
{
    printf("Step 22B: ERROR - No-progress probe registration failed\n");
}

printf("\n=== STEP 22C: DELAYED RECOVERY PROGRESS TEST ===\n");

TaskControlBlock delayed_progress_probe;
TaskRegistry delayed_progress_registry;

Scheduler_Init(&delayed_progress_registry);

Task_Init(
    &delayed_progress_probe,
    102,
    1,
    "DelayedProgressProbe",
    CPU_Task
);

if (Scheduler_AddTask(
        &delayed_progress_registry,
        &delayed_progress_probe) == 0)
{
    delayed_progress_probe.state = TASK_RUNNING;
    delayed_progress_probe.recovery_pending = 1;
    delayed_progress_probe.recovery_start_progress_count = 0UL;
    delayed_progress_probe.progress_count = 0UL;
    delayed_progress_probe.progress_reported = 0;
    delayed_progress_probe.recovery_failed = 0;

    /* First dispatch: the task has not reported progress yet. */
    Scheduler_VerifyRecovery(
        &delayed_progress_registry,
        &delayed_progress_probe
    );

    if (delayed_progress_probe.state == TASK_RUNNING &&
        delayed_progress_probe.recovery_pending == 1 &&
        delayed_progress_probe.recovery_failed == 0)
    {
        printf("Step 22C: PASS - Recovery allowed another dispatch\n");

        /* Second dispatch: the task now reports genuine progress. */
        delayed_progress_probe.progress_count = 1UL;
        delayed_progress_probe.progress_reported = 1;

        Scheduler_VerifyRecovery(
            &delayed_progress_registry,
            &delayed_progress_probe
        );

        if (delayed_progress_probe.recovery_pending == 0 &&
            delayed_progress_probe.successful_recoveries == 1UL &&
            delayed_progress_probe.recovery_failed == 0 &&
            delayed_progress_probe.fault_type == FAULT_NONE)
        {
            printf("Step 22C: PASS - Delayed progress completed recovery\n");
        }
        else
        {
            printf("Step 22C: ERROR - Delayed progress did not complete recovery\n");
        }
    }
    else
    {
        printf("Step 22C: ERROR - Recovery failed before the second dispatch\n");
    }
}
else
{
    printf("Step 22C: ERROR - Delayed-progress probe registration failed\n");
}


printf("\n=== STEP 22E: RECOVERY AFTER WATCHDOG SCAN TEST ===\n");

TaskControlBlock watchdog_cleared_progress_probe;
TaskRegistry watchdog_cleared_progress_registry;

Scheduler_Init(&watchdog_cleared_progress_registry);

Task_Init(
    &watchdog_cleared_progress_probe,
    103,
    1,
    "RecoveryAfterWatchdogScan",
    CPU_Task
);

if (Scheduler_AddTask(
        &watchdog_cleared_progress_registry,
        &watchdog_cleared_progress_probe) == 0)
{
    watchdog_cleared_progress_probe.state = TASK_RUNNING;
    watchdog_cleared_progress_probe.progress_enabled = 1;
    watchdog_cleared_progress_probe.recovery_pending = 1;
    watchdog_cleared_progress_probe.recovery_start_progress_count = 9UL;
    watchdog_cleared_progress_probe.progress_count = 10UL;
    watchdog_cleared_progress_probe.progress_reported = 1;
    watchdog_cleared_progress_probe.last_progress_tick = Scheduler_GetTick();
    watchdog_cleared_progress_probe.successful_recoveries = 0UL;
    watchdog_cleared_progress_probe.recovery_attempts = 1UL;
    watchdog_cleared_progress_probe.recovery_failed = 0;
    watchdog_cleared_progress_probe.fault_type = FAULT_WATCHDOG;

    /* Simulate a registry-wide watchdog scan consuming the progress flag. */
    Scheduler_CheckTaskWatchdog(&watchdog_cleared_progress_registry);

    int watchdog_cleared_progress =
        watchdog_cleared_progress_probe.progress_reported == 0;

    /* Recovery must recognize the durable progress-count increase. */
    Scheduler_VerifyRecovery(
        &watchdog_cleared_progress_registry,
        &watchdog_cleared_progress_probe
    );

    if (watchdog_cleared_progress &&
        watchdog_cleared_progress_probe.recovery_pending == 0 &&
        watchdog_cleared_progress_probe.successful_recoveries == 1UL &&
        watchdog_cleared_progress_probe.recovery_attempts == 0UL &&
        watchdog_cleared_progress_probe.recovery_failed == 0 &&
        watchdog_cleared_progress_probe.fault_type == FAULT_NONE &&
        watchdog_cleared_progress_probe.state == TASK_RUNNING)
    {
        printf("Step 22E: PASS - Watchdog scan did not hide genuine recovery progress\n");
    }
    else
    {
        printf("Step 22E: ERROR - Watchdog scan hid genuine recovery progress\n");
    }
}
else
{
    printf("Step 22E: ERROR - Watchdog-scan probe registration failed\n");
}

printf("\n=== STEP 22D: RECOVERY AFTER YIELD TEST ===\n");

TaskControlBlock recovery_yield_probe;
TaskRegistry recovery_yield_registry;

Scheduler_Init(&recovery_yield_registry);

Task_Init(
    &recovery_yield_probe,
    105,
    1,
    "RecoveryYieldProbe",
    RecoveryYieldProbe_Task
);

if (Scheduler_AddTask(
        &recovery_yield_registry,
        &recovery_yield_probe) == 0)
{
    recovery_yield_probe.progress_enabled = 1;
    recovery_yield_probe.state = TASK_RUNNING;
    recovery_yield_probe.recovery_pending = 1;
    recovery_yield_probe.recovery_start_progress_count = 0UL;
    recovery_yield_probe.progress_count = 0UL;
    recovery_yield_probe.progress_reported = 0;
    recovery_yield_probe.recovery_failed = 0;
    recovery_yield_registry.current_task = &recovery_yield_probe;

    /* First dispatch reports progress and yields. */
    Scheduler_Dispatch(&recovery_yield_registry);
    Scheduler_VerifyRecovery(
        &recovery_yield_registry,
        &recovery_yield_probe
    );

    if (recovery_yield_probe.state == TASK_READY &&
        recovery_yield_probe.recovery_pending == 1 &&
        recovery_yield_probe.progress_count == 1UL)
    {
        printf("Step 22D: PASS - Yield deferred recovery verification\n");

        /* Resume the task and allow it to continue without yielding. */
        TaskControlBlock *selected =
            Scheduler_SelectNext(&recovery_yield_registry);

        if (selected == &recovery_yield_probe)
        {
            Scheduler_Dispatch(&recovery_yield_registry);
            Scheduler_VerifyRecovery(
                &recovery_yield_registry,
                &recovery_yield_probe
            );

            if (recovery_yield_probe.state == TASK_RUNNING &&
                recovery_yield_probe.recovery_pending == 0 &&
                recovery_yield_probe.successful_recoveries == 1UL &&
                recovery_yield_probe.progress_count == 2UL &&
                recovery_yield_probe.recovery_failed == 0 &&
                recovery_yield_probe.fault_type == FAULT_NONE)
            {
                printf("Step 22D: PASS - Recovery verified after task resumed\n");
            }
            else
            {
                printf("Step 22D: ERROR - Recovery failed after task resumed\n");
            }
        }
        else
        {
            printf("Step 22D: ERROR - Yielded task was not rescheduled\n");
        }
    }
    else
    {
        printf("Step 22D: ERROR - Yield state or progress was unexpected\n");
    }
}
else
{
    printf("Step 22D: ERROR - Recovery-yield probe registration failed\n");
}

printf("\n=== STEP 23: BOUNDED RECOVERY AND ISOLATION TEST ===\n");

TaskControlBlock isolation_probe;
TaskRegistry isolation_registry;

Scheduler_Init(&isolation_registry);

Task_Init(
    &isolation_probe,
    103,
    1,
    "IsolationProbe",
    CPU_Task
);

if (Scheduler_AddTask(&isolation_registry, &isolation_probe) == 0)
{
    /* First fault: recovery attempt 1. */
    isolation_probe.state = TASK_FAULTED;
    isolation_probe.fault_type = FAULT_WATCHDOG;

    Scheduler_ProcessFaultRecovery(&isolation_registry);

    if (isolation_probe.state == TASK_READY &&
        isolation_probe.recovery_attempts == 1UL &&
        isolation_probe.isolated == 0)
    {
        printf("Step 23A: PASS - First recovery attempt completed\n");
    }
    else
    {
        printf("Step 23A: ERROR - First recovery attempt failed\n");
    }

    /* Second fault: recovery attempt 2. */
    isolation_probe.state = TASK_FAULTED;
    isolation_probe.fault_type = FAULT_WATCHDOG;

    Scheduler_ProcessFaultRecovery(&isolation_registry);

    if (isolation_probe.state == TASK_READY &&
        isolation_probe.recovery_attempts == MAX_RECOVERY_ATTEMPTS &&
        isolation_probe.isolated == 0)
    {
        printf("Step 23B: PASS - Second recovery attempt completed\n");
    }
    else
    {
        printf("Step 23B: ERROR - Second recovery attempt failed\n");
    }

    /* Third fault: the exhausted task must be isolated. */
    isolation_probe.state = TASK_FAULTED;
    isolation_probe.fault_type = FAULT_WATCHDOG;

    Scheduler_ProcessFaultRecovery(&isolation_registry);

    if (isolation_probe.isolated == 1 &&
        isolation_probe.recovery_failed == 1 &&
        isolation_probe.state == TASK_FAULTED &&
        isolation_probe.recovery_pending == 0 &&
        isolation_probe.recovery_in_progress == 0 &&
        isolation_probe.recovery_attempts == MAX_RECOVERY_ATTEMPTS &&
        isolation_probe.restart_count == MAX_RECOVERY_ATTEMPTS &&
        isolation_probe.successful_recoveries == 0UL &&
        isolation_registry.recovery_events == MAX_RECOVERY_ATTEMPTS)
    {
        printf("Step 23C: PASS - Task isolated after recovery limit\n");
    }
    else
    {
        printf("Step 23C: ERROR - Task was not isolated at the limit\n");
    }

        /* Step 23D: An isolated task must not be restarted. */
    Scheduler_ProcessFaultRecovery(&isolation_registry);

    if (isolation_probe.isolated == 1 &&
        isolation_probe.recovery_failed == 1 &&
        isolation_probe.state == TASK_FAULTED &&
        isolation_probe.recovery_pending == 0 &&
        isolation_probe.recovery_in_progress == 0 &&
        isolation_probe.recovery_attempts == MAX_RECOVERY_ATTEMPTS &&
        isolation_probe.restart_count == MAX_RECOVERY_ATTEMPTS &&
        isolation_probe.successful_recoveries == 0UL &&
        isolation_registry.recovery_events == MAX_RECOVERY_ATTEMPTS)
    {
        printf("Step 23D: PASS - Isolated task remains isolated\n");
    }
    else
    {
        printf("Step 23D: ERROR - Isolated task was restarted\n");
    }
}
else
 {
    printf("Step 23: ERROR - Isolation probe registration failed\n");
 }

printf("\n=== STEP 24: WATCHDOG-RECOVERY INTEGRATION TEST ===\n");

TaskControlBlock recovery_stall_probe;
TaskRegistry recovery_stall_registry;

Scheduler_Init(&recovery_stall_registry);

Task_Init(
    &recovery_stall_probe,
    104,
    1,
    "RecoveryStallProbe",
    CPU_Task
);

if (Scheduler_AddTask(
        &recovery_stall_registry,
        &recovery_stall_probe) == 0)
{
    recovery_stall_probe.progress_enabled = 1;
    recovery_stall_probe.recovery_failure_injection_enabled = 1;

    /* Make the probe stall immediately and after every restart. */
    recovery_stall_probe.restart_count = 1;
    recovery_stall_probe.state = TASK_RUNNING;
    recovery_stall_probe.progress_reported = 0;
    recovery_stall_probe.execution_units = 1;
    recovery_stall_probe.watchdog_last_execution_units = 0;
    recovery_stall_probe.last_progress_tick =
        Scheduler_GetTick() - (TASK_WATCHDOG_THRESHOLD + 1UL);

    recovery_stall_registry.current_task = &recovery_stall_probe;

    for (unsigned long cycle = 0;
         cycle < MAX_RECOVERY_ATTEMPTS + 1UL;
         cycle++)
    {
        if (recovery_stall_probe.isolated)
        {
            break;
        }

        /* Simulate an expired progress deadline. */
        recovery_stall_probe.state = TASK_RUNNING;
        recovery_stall_probe.progress_reported = 0;
        recovery_stall_probe.last_progress_tick =
            Scheduler_GetTick() - (TASK_WATCHDOG_THRESHOLD + 1UL);
        recovery_stall_probe.execution_units =
            recovery_stall_probe.watchdog_last_execution_units + 1UL;

        Scheduler_CheckTaskWatchdog(&recovery_stall_registry);

        if (recovery_stall_probe.state != TASK_FAULTED)
        {
            printf("Step 24: ERROR - Watchdog did not detect stall\n");
            break;
        }

        Scheduler_ProcessFaultRecovery(&recovery_stall_registry);

        if (recovery_stall_probe.isolated)
        {
            break;
        }

        if (recovery_stall_probe.state != TASK_READY ||
            recovery_stall_probe.recovery_pending != 1)
        {
            printf("Step 24: ERROR - Recovery restart failed\n");
            break;
        }

        TaskControlBlock *selected =
            Scheduler_SelectNext(&recovery_stall_registry);

        if (selected != &recovery_stall_probe)
        {
            printf("Step 24: ERROR - Recovered task was not scheduled\n");
            break;
        }

        Scheduler_Dispatch(&recovery_stall_registry);
        Scheduler_VerifyRecovery(
            &recovery_stall_registry,
            &recovery_stall_probe
        );
    }

    if (recovery_stall_probe.isolated == 1 &&
        recovery_stall_probe.recovery_failed == 1 &&
        recovery_stall_probe.state == TASK_FAULTED &&
        recovery_stall_probe.recovery_attempts ==
            MAX_RECOVERY_ATTEMPTS &&
        recovery_stall_probe.fault_count >=
            MAX_RECOVERY_ATTEMPTS + 1UL)
    {
        printf("Step 24: PASS - Watchdog detected repeated stalls, "
               "recovery was retried, and the task was isolated\n");
    }
    else
    {
        printf("Step 24: ERROR - End-to-end recovery test failed\n");
    }
}
else
{
    printf("Step 24: ERROR - Recovery probe registration failed\n");
}

printf("\n=== STEP 25: RECOVERY HEARTBEAT WITHOUT PROGRESS WATCHDOG TEST ===\n");

TaskControlBlock recovery_heartbeat_probe;
TaskRegistry recovery_heartbeat_registry;

Scheduler_Init(&recovery_heartbeat_registry);

Task_Init(
    &recovery_heartbeat_probe,
    106,
    1,
    "RecoveryHeartbeatProbe",
    RecoveryHeartbeatProbe_Task
);

if (Scheduler_AddTask(
        &recovery_heartbeat_registry,
        &recovery_heartbeat_probe) == 0)
{
    recovery_heartbeat_probe.progress_enabled = 1;
    recovery_heartbeat_probe.state = TASK_FAULTED;
    recovery_heartbeat_probe.fault_type = FAULT_WATCHDOG;

    Scheduler_ProcessFaultRecovery(&recovery_heartbeat_registry);

    if (recovery_heartbeat_probe.state == TASK_READY &&
        recovery_heartbeat_probe.recovery_pending == 1 &&
        recovery_heartbeat_probe.recovery_attempts == 1UL &&
        recovery_heartbeat_probe.recovery_failed == 0)
    {
        int heartbeat_recovery_cycle_ok = 1;

        for (unsigned long cycle = 0;
             cycle < TASK_WATCHDOG_THRESHOLD + 1UL;
             cycle++)
        {
            Scheduler_Tick(&recovery_heartbeat_registry);

            TaskControlBlock* selected =
                Scheduler_SelectNext(&recovery_heartbeat_registry);

            if (selected != &recovery_heartbeat_probe)
            {
                printf("Step 25: ERROR - Recovered heartbeat task was not scheduled\n");
                heartbeat_recovery_cycle_ok = 0;
                break;
            }

            recovery_heartbeat_probe.execution_units++;

            Scheduler_Dispatch(&recovery_heartbeat_registry);

            Scheduler_VerifyRecovery(
                &recovery_heartbeat_registry,
                &recovery_heartbeat_probe
            );

            if (recovery_heartbeat_probe.recovery_pending != 1 ||
                recovery_heartbeat_probe.successful_recoveries != 0UL ||
                recovery_heartbeat_probe.progress_count !=
                    recovery_heartbeat_probe.recovery_start_progress_count)
            {
                printf("Step 25: ERROR - Heartbeat incorrectly completed recovery\n");
                heartbeat_recovery_cycle_ok = 0;
                break;
            }

            Scheduler_CheckTaskWatchdog(&recovery_heartbeat_registry);

            if (recovery_heartbeat_probe.state == TASK_FAULTED)
            {
                break;
            }
        }

        if (heartbeat_recovery_cycle_ok &&
            recovery_heartbeat_probe.state == TASK_FAULTED &&
            recovery_heartbeat_probe.fault_type == FAULT_WATCHDOG &&
            recovery_heartbeat_probe.fault_count == 1UL &&
            recovery_heartbeat_probe.recovery_pending == 1 &&
            recovery_heartbeat_probe.recovery_attempts == 1UL &&
            recovery_heartbeat_probe.successful_recoveries == 0UL &&
            recovery_heartbeat_probe.progress_count ==
                recovery_heartbeat_probe.recovery_start_progress_count &&
            recovery_heartbeat_probe.last_heartbeat_tick ==
                Scheduler_GetTick() &&
            Scheduler_GetTick() -
                recovery_heartbeat_probe.last_progress_tick >
                    TASK_WATCHDOG_THRESHOLD)
        {
            printf("Step 25: PASS - Watchdog detected stalled recovered task despite repeated heartbeats\n");
        }
        else
        {
            printf("Step 25: ERROR - Recovery heartbeat watchdog test failed\n");
        }
    }
    else
    {
        printf("Step 25: ERROR - Initial task recovery failed\n");
    }
}
else
{
    printf("Step 25: ERROR - Heartbeat probe registration failed\n");
}

printf("\n=== STEP 26: MULTI-TASK RECOVERY INDEPENDENCE TEST ===\n");

TaskControlBlock multi_recovery_task_a;
TaskControlBlock multi_recovery_task_b;
TaskRegistry multi_recovery_registry;

Scheduler_Init(&multi_recovery_registry);

Task_Init(
    &multi_recovery_task_a,
    107,
    2,
    "MultiRecoveryA",
    RecoveryHeartbeatProbe_Task
);

Task_Init(
    &multi_recovery_task_b,
    108,
    2,
    "MultiRecoveryB",
    RecoveryHeartbeatProbe_Task
);

int multi_recovery_setup_ok =
    Scheduler_AddTask(
        &multi_recovery_registry,
        &multi_recovery_task_a) == 0 &&
    Scheduler_AddTask(
        &multi_recovery_registry,
        &multi_recovery_task_b) == 0;

if (multi_recovery_setup_ok)
{
    /* Give each task a different progress baseline. */
    multi_recovery_task_a.progress_enabled = 1;
    multi_recovery_task_b.progress_enabled = 1;

    multi_recovery_task_a.progress_count = 7UL;
    multi_recovery_task_b.progress_count = 11UL;

    /* Fault both tasks before one recovery-processing pass. */
    multi_recovery_task_a.state = TASK_FAULTED;
    multi_recovery_task_a.fault_type = FAULT_WATCHDOG;

    multi_recovery_task_b.state = TASK_FAULTED;
    multi_recovery_task_b.fault_type = FAULT_RUNTIME;

    Scheduler_ProcessFaultRecovery(&multi_recovery_registry);

    if (multi_recovery_task_a.state == TASK_READY &&
        multi_recovery_task_b.state == TASK_READY &&
        multi_recovery_task_a.recovery_pending == 1 &&
        multi_recovery_task_b.recovery_pending == 1 &&
        multi_recovery_task_a.recovery_attempts == 1UL &&
        multi_recovery_task_b.recovery_attempts == 1UL &&
        multi_recovery_task_a.recovery_start_progress_count == 7UL &&
        multi_recovery_task_b.recovery_start_progress_count == 11UL &&
        multi_recovery_task_a.recovery_failed == 0 &&
        multi_recovery_task_b.recovery_failed == 0 &&
        multi_recovery_registry.recovery_events == 2UL)
    {
        printf("Step 26A: PASS - Both faulted tasks restarted independently\n");

        /* Simulate genuine progress from each recovered task. */
        multi_recovery_task_a.state = TASK_RUNNING;
        multi_recovery_task_b.state = TASK_RUNNING;

        multi_recovery_task_a.progress_count = 8UL;
        multi_recovery_task_b.progress_count = 13UL;

        multi_recovery_task_a.progress_reported = 1;
        multi_recovery_task_b.progress_reported = 1;

        Scheduler_VerifyRecovery(
            &multi_recovery_registry,
            &multi_recovery_task_a
        );

        Scheduler_VerifyRecovery(
            &multi_recovery_registry,
            &multi_recovery_task_b
        );

        if (multi_recovery_task_a.recovery_pending == 0 &&
            multi_recovery_task_b.recovery_pending == 0 &&
            multi_recovery_task_a.successful_recoveries == 1UL &&
            multi_recovery_task_b.successful_recoveries == 1UL &&
            multi_recovery_task_a.recovery_attempts == 0UL &&
            multi_recovery_task_b.recovery_attempts == 0UL &&
            multi_recovery_task_a.fault_type == FAULT_NONE &&
            multi_recovery_task_b.fault_type == FAULT_NONE &&
            multi_recovery_task_a.recovery_failed == 0 &&
            multi_recovery_task_b.recovery_failed == 0 &&
            multi_recovery_registry.recovery_events == 2UL)
        {
            printf("Step 26B: PASS - Both tasks independently verified after progress\n");
        }
        else
        {
            printf("Step 26B: ERROR - Independent recovery verification failed\n");
        }
    }
    else
    {
        printf("Step 26A: ERROR - Multi-task recovery processing failed\n");
    }
}
else
{
    printf("Step 26: ERROR - Multi-task recovery setup failed\n");
}

printf("\n=== STEP 27: MULTI-TASK WATCHDOG DETECTION TEST ===\n");

TaskControlBlock watchdog_multi_task_a;
TaskControlBlock watchdog_multi_task_b;
TaskRegistry watchdog_multi_registry;

Scheduler_Init(&watchdog_multi_registry);

Task_Init(
    &watchdog_multi_task_a,
    109,
    2,
    "WatchdogMultiA",
    RecoveryHeartbeatProbe_Task
);

Task_Init(
    &watchdog_multi_task_b,
    110,
    2,
    "WatchdogMultiB",
    RecoveryHeartbeatProbe_Task
);

int watchdog_multi_setup_ok =
    Scheduler_AddTask(
        &watchdog_multi_registry,
        &watchdog_multi_task_a) == 0 &&
    Scheduler_AddTask(
        &watchdog_multi_registry,
        &watchdog_multi_task_b) == 0;

if (watchdog_multi_setup_ok)
{
    unsigned long progress_baseline_tick = Scheduler_GetTick();

    watchdog_multi_task_a.progress_enabled = 1;
    watchdog_multi_task_b.progress_enabled = 1;

    watchdog_multi_task_a.state = TASK_RUNNING;
    watchdog_multi_task_b.state = TASK_READY;

    watchdog_multi_task_a.progress_reported = 0;
    watchdog_multi_task_b.progress_reported = 0;

    watchdog_multi_task_a.last_progress_tick = progress_baseline_tick;
    watchdog_multi_task_b.last_progress_tick = progress_baseline_tick;

    /* Both tasks have executed since their last watchdog baseline. */
    watchdog_multi_task_a.execution_units = 1UL;
    watchdog_multi_task_b.execution_units = 1UL;

    watchdog_multi_task_a.watchdog_last_execution_units = 0UL;
    watchdog_multi_task_b.watchdog_last_execution_units = 0UL;

    /* Keep both tasks sending fresh heartbeats without progress. */
    for (unsigned long tick = 0;
         tick < TASK_WATCHDOG_THRESHOLD + 1UL;
         tick++)
    {
        Scheduler_Tick(&watchdog_multi_registry);

        Scheduler_TaskHeartbeat(&watchdog_multi_task_a);
        Scheduler_TaskHeartbeat(&watchdog_multi_task_b);
    }

    Scheduler_CheckTaskWatchdog(&watchdog_multi_registry);

    if (watchdog_multi_task_a.state == TASK_FAULTED &&
        watchdog_multi_task_b.state == TASK_FAULTED &&
        watchdog_multi_task_a.fault_type == FAULT_WATCHDOG &&
        watchdog_multi_task_b.fault_type == FAULT_WATCHDOG &&
        watchdog_multi_task_a.fault_count == 1UL &&
        watchdog_multi_task_b.fault_count == 1UL &&
        watchdog_multi_task_a.last_heartbeat_tick == Scheduler_GetTick() &&
        watchdog_multi_task_b.last_heartbeat_tick == Scheduler_GetTick() &&
        Scheduler_GetTick() - watchdog_multi_task_a.last_progress_tick >
            TASK_WATCHDOG_THRESHOLD &&
        Scheduler_GetTick() - watchdog_multi_task_b.last_progress_tick >
            TASK_WATCHDOG_THRESHOLD &&
        watchdog_multi_registry.reschedule_pending == 1 &&
        watchdog_multi_registry.reschedule_reason == RESCHEDULE_FAULT)
    {
        printf("Step 27: PASS - One watchdog scan detected both stalled tasks despite fresh heartbeats\n");
    }
    else
    {
        printf("Step 27: ERROR - Multi-task watchdog detection failed\n");
    }
}
else
{
    printf("Step 27: ERROR - Multi-task watchdog setup failed\n");
}

printf("\n=== STEP 28: MIXED-OUTCOME MULTI-TASK RECOVERY TEST ===\n");

TaskControlBlock mixed_recovery_task;
TaskControlBlock mixed_isolation_task;
TaskRegistry mixed_recovery_registry;

Scheduler_Init(&mixed_recovery_registry);

Task_Init(
    &mixed_recovery_task,
    111,
    2,
    "MixedRecoveryTask",
    RecoveryHeartbeatProbe_Task
);

Task_Init(
    &mixed_isolation_task,
    112,
    2,
    "MixedIsolationTask",
    RecoveryHeartbeatProbe_Task
);

int mixed_recovery_setup_ok =
    Scheduler_AddTask(
        &mixed_recovery_registry,
        &mixed_recovery_task) == 0 &&
    Scheduler_AddTask(
        &mixed_recovery_registry,
        &mixed_isolation_task) == 0;

if (mixed_recovery_setup_ok)
{
    /* First task can recover normally. */
    mixed_recovery_task.state = TASK_FAULTED;
    mixed_recovery_task.fault_type = FAULT_WATCHDOG;

    /* Second task has already exhausted its recovery attempts. */
    mixed_isolation_task.state = TASK_FAULTED;
    mixed_isolation_task.fault_type = FAULT_RUNTIME;
    mixed_isolation_task.recovery_attempts = MAX_RECOVERY_ATTEMPTS;

    Scheduler_ProcessFaultRecovery(&mixed_recovery_registry);

    if (mixed_recovery_task.state == TASK_READY &&
        mixed_recovery_task.recovery_pending == 1 &&
        mixed_recovery_task.recovery_attempts == 1UL &&
        mixed_recovery_task.recovery_failed == 0 &&
        mixed_isolation_task.state == TASK_FAULTED &&
        mixed_isolation_task.isolated == 1 &&
        mixed_isolation_task.recovery_failed == 1 &&
        mixed_isolation_task.recovery_attempts == MAX_RECOVERY_ATTEMPTS &&
        mixed_recovery_registry.recovery_events == 1UL)
    {
        printf("Step 28A: PASS - One task restarted while the exhausted task was isolated\n");

        /* Only the recoverable task now demonstrates genuine progress. */
        mixed_recovery_task.state = TASK_RUNNING;
        mixed_recovery_task.progress_count =
            mixed_recovery_task.recovery_start_progress_count + 1UL;
        mixed_recovery_task.progress_reported = 1;

        Scheduler_VerifyRecovery(
            &mixed_recovery_registry,
            &mixed_recovery_task
        );

        if (mixed_recovery_task.state == TASK_RUNNING &&
            mixed_recovery_task.recovery_pending == 0 &&
            mixed_recovery_task.successful_recoveries == 1UL &&
            mixed_recovery_task.recovery_attempts == 0UL &&
            mixed_recovery_task.fault_type == FAULT_NONE &&
            mixed_recovery_task.recovery_failed == 0 &&
            mixed_isolation_task.state == TASK_FAULTED &&
            mixed_isolation_task.isolated == 1 &&
            mixed_isolation_task.recovery_failed == 1 &&
            mixed_isolation_task.recovery_attempts == MAX_RECOVERY_ATTEMPTS &&
            mixed_isolation_task.successful_recoveries == 0UL &&
            mixed_recovery_registry.recovery_events == 1UL)
        {
            printf("Step 28B: PASS - Successful recovery did not affect the isolated task\n");
        }
        else
        {
            printf("Step 28B: ERROR - Mixed recovery outcomes interfered\n");
        }
    }
    else
    {
        printf("Step 28A: ERROR - Independent recovery and isolation failed\n");
    }
}
else
{
    printf("Step 28: ERROR - Mixed recovery setup failed\n");
}

 printf("Total Context Switches: %lu\n",
       Scheduler_GetContextSwitchCount(&registry));
 printf("Recovery Events: %lu\n",
       Scheduler_GetRecoveryEventCount(&registry));
 printf("Kernel Run Scheduler Activity: %.2f%%\n",
       Scheduler_GetLoad(&registry));
  printf("Task Function Elapsed: %llu ns\n",
       (unsigned long long)registry.task_function_elapsed_ns);
 printf("Idle Function Elapsed: %llu ns\n",
       (unsigned long long)registry.idle_function_elapsed_ns);
 printf("Kernel Process CPU Time: %llu ns\n",
       (unsigned long long)registry.kernel_cpu_ns);
printf("Kernel Wall Time: %llu ns\n",
       (unsigned long long)registry.kernel_wall_ns);

  return 0;
}
