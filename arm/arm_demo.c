#include "task.h"
#include "scheduler.h"
#include "kernel_ipc.h"
#include "mutex.h"
#include "memory_pool.h"
#include "memory_pool_critical.h"
#include <stdint.h>
#include <stddef.h>
#include "port_layout.h"
#include "trace.h"

#define SCB_ICSR   (*(volatile uint32_t *)0xE000ED04UL)
#define SCB_SHPR3  (*(volatile uint32_t *)0xE000ED20UL)
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010UL)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014UL)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018UL)

#define ICSR_PENDSVSET (1UL << 28)
#define SYSTICK_ENABLE (1UL << 0)
#define SYSTICK_TICKINT (1UL << 1)
#define SYSTICK_CLKSOURCE (1UL << 2)

typedef void (*ArmTaskEntry)(TaskControlBlock *);

_Static_assert(
    offsetof(TaskControlBlock, context) +
    offsetof(TaskContext, sp) == offsetof(TaskControlBlock, context),
    "Unexpected TCB context layout");

_Static_assert(sizeof(uint32_t) == 4,
               "Cortex-M registers must be 32 bits");
_Static_assert(sizeof(uintptr_t) == sizeof(uint32_t),
               "Cortex-M saved SP must be one 32-bit word");

static TaskControlBlock task_a;
static TaskControlBlock task_b;
static TaskRegistry arm_scheduler;
static TaskControlBlock arm_recovery_owner;
static TaskControlBlock arm_recovery_waiter;
static KernelMutex arm_recovery_owned_mutex;
static KernelMutex arm_recovery_waiting_mutex;

#define ARM_POOL_BLOCK_SIZE 32U
#define ARM_POOL_BLOCK_COUNT 4U

static KernelMemoryPool arm_memory_pool;
static union
{
    max_align_t alignment;
    unsigned char bytes[ARM_POOL_BLOCK_SIZE * ARM_POOL_BLOCK_COUNT];
} arm_memory_pool_storage;

volatile uint32_t arm_memory_pool_test_checks;
volatile uint32_t arm_memory_pool_test_failures;

TaskControlBlock * volatile g_current_tcb;
uintptr_t * volatile g_current_saved_sp;

volatile uint32_t task_a_runs;
volatile uint32_t task_b_runs;
volatile uint32_t arm_context_switches;
volatile uint32_t arm_tick_count;
volatile uint32_t arm_systick_reschedule_requests;
volatile uint32_t arm_systick_no_reschedule_ticks;
volatile uint32_t arm_systick_slice_progress_checks;
volatile uint32_t arm_systick_slice_expiry_checks;
volatile uint32_t arm_systick_slice_timing_failures;
volatile uint32_t arm_register_test_checks;
volatile uint32_t arm_register_test_failures;
volatile uint32_t arm_register_test_last_switches;
volatile uint32_t arm_stack_high_water_test_checks;
volatile uint32_t arm_stack_high_water_test_failures;
volatile uint32_t arm_stack_warning_test_checks;
volatile uint32_t arm_stack_warning_test_failures;
volatile uint32_t arm_stack_runtime_warning_events;
volatile int32_t arm_stack_runtime_warning_task_id;
volatile uint32_t arm_stack_runtime_high_water_usage;
volatile uint32_t arm_context_restart_test_checks;
volatile uint32_t arm_context_restart_test_failures;
volatile uint32_t arm_scheduler_recovery_test_checks;
volatile uint32_t arm_scheduler_recovery_test_failures;

extern void ARM_StartFirstTask(void);
extern void ARM_RegisterPreservationTest(TaskControlBlock *task);
extern const uint8_t __text_start__;
extern const uint8_t __text_end__;

void TaskExit_Handler(void)
{
    /* A demo task must not return. */
    __asm volatile ("cpsid i" ::: "memory");

    for (;;)
    {
        __asm volatile ("nop");
    }
}

static void ARM_TaskInit(
    TaskControlBlock *task,
    int id,
    const char *name,
    ArmTaskEntry entry)
{
    Task_Init(task, id, 1, name, entry);

    /*
     * Pass the TCB pointer as the first argument (R0)
     * when the task starts for the first time.
     */
    task->initial_stack_argument = (uintptr_t)task;
    task->initial_stack_return_handler = (uintptr_t)TaskExit_Handler;

    task->stack[TASK_STACK_SIZE - 8] =
        (uint32_t)task->initial_stack_argument;

    task->stack[TASK_STACK_SIZE - 3] =
        (uint32_t)task->initial_stack_return_handler;

}

static void ARM_WatchdogService(TaskControlBlock *reporting_task)
{
    Scheduler_VerifyRecovery(&arm_scheduler, reporting_task);
    Scheduler_CheckTaskWatchdog(&arm_scheduler);
    Scheduler_ProcessFaultRecovery(&arm_scheduler);
}

void ARM_TaskAProgress(void)
{
    Scheduler_TaskProgress(&task_a);
    ARM_WatchdogService(&task_a);
}

static void TaskA_Entry(TaskControlBlock *task)
{
    ARM_RegisterPreservationTest(task);

    for (;;)
    {
        task_a_runs++;
    }
}


static __attribute__((noinline)) void ARM_StackUsageExercise(void)
{
    /* Keep a real local stack frame active across a task switch. */
    volatile uint32_t stack_probe[176];

    for (uint32_t i = 0; i < 176U; ++i)
    {
        stack_probe[i] = i ^ 0xA5A5A5A5UL;
    }

    uint32_t starting_switches = arm_context_switches;

    while (arm_context_switches == starting_switches)
    {
        stack_probe[0]++;
    }
}


static void TaskB_Entry(TaskControlBlock *task)
{
    for (;;)
    {
        task_b_runs++;
        ARM_StackUsageExercise();
        Scheduler_TaskProgress(task);
        ARM_WatchdogService(task);
    }
}

static int ARM_ValidateSavedContext(const TaskControlBlock *task)
{
    if (Task_ValidateContext(task) != 0)
    {
        return -1;
    }

    /*
     * Saved frame layout:
     * R4-R11 at words 0-7,
     * R0-R3, R12, LR, PC, xPSR at words 8-15.
     */
    const uint32_t *frame = (const uint32_t *)task->context.sp;

    const uintptr_t saved_pc =
        (uintptr_t)frame[14] & ~(uintptr_t)1U;
    const uint32_t saved_xpsr = frame[15];

    const uintptr_t text_start = (uintptr_t)&__text_start__;
    const uintptr_t text_end = (uintptr_t)&__text_end__;

    if (saved_pc < text_start || saved_pc >= text_end)
    {
        return -2;
    }

    /* A valid Cortex-M exception frame must indicate Thumb state. */
    if ((saved_xpsr & (1UL << 24)) == 0U)
    {
        return -3;
    }

    /* The saved task context must have originated in Thread mode. */
    if ((saved_xpsr & 0x1FFUL) != 0U)
    {
        return -4;
    }

    return 0;
}

uintptr_t *ARM_ScheduleNext(void)
{
    TaskControlBlock *previous = g_current_tcb;
    Task_UpdateStackHighWater(previous);

    /* Record each task's stack warning only once. */
    if (previous != NULL && !previous->stack_warning_reported)
    {
        int warning_status = Task_IsStackUsageWarning(previous);
        if (warning_status == 1)
        {
            previous->stack_warning_reported = 1;
            arm_stack_runtime_warning_events++;
            arm_stack_runtime_warning_task_id = previous->id;
        }
    }
    /* Track Task B's deepest measured stack usage. */
    if (previous == &task_b)
    {
        uint32_t usage =
            (uint32_t)Task_GetStackHighWaterUsage(previous);
        if (usage > arm_stack_runtime_high_water_usage)
        {
            arm_stack_runtime_high_water_usage = usage;
        }
    }
    TaskControlBlock *next;

        if (previous != NULL && previous->state == TASK_RUNNING)
    {
        if (!previous->is_idle)
        {
            previous->execution_units++;
        }

        previous->state = TASK_READY;
    }

    arm_scheduler.current_task = previous;
    arm_scheduler.reschedule_pending = 1;
    arm_scheduler.reschedule_reason = RESCHEDULE_TIMESLICE;
    arm_scheduler.time_slice_expired = 1;

    next = Scheduler_SelectNext(&arm_scheduler);

    /*
     * Never restore a task whose saved context is invalid.
     * Isolate invalid tasks and look for another runnable task.
     */
    for (int attempts = 0;
         next != NULL &&
         ARM_ValidateSavedContext(next) != 0 &&
         attempts < arm_scheduler.task_count;
         attempts++)
    {
        next->state = TASK_FAULTED;
        next->fault_type = FAULT_RUNTIME;
        next->fault_count++;
        next->recovery_pending = 0;
        next->recovery_failed = 1;
        next->isolated = 1;
        Kernel_Trace(TRACE_TASK_FAULT, next);
        Kernel_Trace(TRACE_TASK_ISOLATED, next);

        arm_scheduler.current_task = previous;
        arm_scheduler.reschedule_pending = 1;
        arm_scheduler.reschedule_reason = RESCHEDULE_FAULT;

        next = Scheduler_SelectNext(&arm_scheduler);
    }

    /*
     * No valid task remains. Do not return an invalid context
     * to PendSV_Handler.
     */
    if (next == NULL || ARM_ValidateSavedContext(next) != 0)
    {
        for (;;)
        {
            __asm volatile ("nop");
        }
    }

    arm_scheduler.reschedule_pending = 0;
    arm_scheduler.reschedule_reason = RESCHEDULE_NONE;
    arm_scheduler.time_slice_expired = 0;

    if (next != previous)
    {
        arm_context_switches++;
    }

    g_current_tcb = next;
    g_current_saved_sp = &next->context.sp;

    return &next->context.sp;
}

void SysTick_Handler(void)
{
    TaskControlBlock *tick_task = arm_scheduler.current_task;
    unsigned long slice_before = 0UL;
    int was_running =
        tick_task != NULL && tick_task->state == TASK_RUNNING;

    if (was_running)
    {
        slice_before = tick_task->time_slice;
    }

    arm_tick_count++;
    Scheduler_Tick(&arm_scheduler);

    if (was_running)
    {
        if (slice_before > 1UL &&
            arm_scheduler.current_task == tick_task &&
            tick_task->state == TASK_RUNNING &&
            tick_task->time_slice == slice_before - 1UL &&
            !arm_scheduler.reschedule_pending &&
            !arm_scheduler.time_slice_expired)
        {
            arm_systick_slice_progress_checks++;
        }
        else if (slice_before == 1UL &&
                 arm_scheduler.current_task == tick_task &&
                 tick_task->state == TASK_READY &&
                 tick_task->time_slice == 0UL &&
                 arm_scheduler.reschedule_pending &&
                 arm_scheduler.time_slice_expired)
        {
            arm_systick_slice_expiry_checks++;
        }
        else
        {
            arm_systick_slice_timing_failures++;
        }
    }

    if (arm_scheduler.reschedule_pending ||
        arm_scheduler.time_slice_expired)
    {
        arm_systick_reschedule_requests++;
        SCB_ICSR = ICSR_PENDSVSET;
    }
    else
    {
        arm_systick_no_reschedule_ticks++;
    }
}

static uint32_t ARM_ReadPrimask(void)
{
    uint32_t value;
    __asm volatile ("mrs %0, primask" : "=r" (value) : : "memory");
    return value;
}

static int ARM_MemoryPool_Check(int passed)
{
    if (passed)
    {
        arm_memory_pool_test_checks++;
        return 1;
    }

    arm_memory_pool_test_failures++;
    return 0;
}

static int ARM_RunMemoryPoolSmokeTest(void)
{
    void *first;
    void *second;
    void *reused;
    uint32_t before;
    uint32_t after;

    if (!ARM_MemoryPool_Check(
            Kernel_MemoryPool_Init(
                &arm_memory_pool,
                arm_memory_pool_storage.bytes,
                ARM_POOL_BLOCK_SIZE,
                ARM_POOL_BLOCK_COUNT) == 0))
    {
        return -1;
    }

    if (!ARM_MemoryPool_Check(
            Kernel_MemoryPool_SetCriticalSection(
                &arm_memory_pool,
                ARM_MemoryPool_EnterCritical,
                ARM_MemoryPool_ExitCritical) == 0))
    {
        return -1;
    }

    if (!ARM_MemoryPool_Check(
            Kernel_MemoryPool_GetFreeCount(&arm_memory_pool) ==
            ARM_POOL_BLOCK_COUNT))
    {
        return -1;
    }

    before = ARM_ReadPrimask();
    first = Kernel_MemoryPool_Alloc(&arm_memory_pool);
    after = ARM_ReadPrimask();
    if (!ARM_MemoryPool_Check(before == after) ||
        !ARM_MemoryPool_Check(first != NULL))
    {
        return -1;
    }

    second = Kernel_MemoryPool_Alloc(&arm_memory_pool);
    if (!ARM_MemoryPool_Check(second != NULL && second != first))
    {
        return -1;
    }

    if (!ARM_MemoryPool_Check(
            Kernel_MemoryPool_GetFreeCount(&arm_memory_pool) ==
            ARM_POOL_BLOCK_COUNT - 2U))
    {
        return -1;
    }

    before = ARM_ReadPrimask();
    int free_result = Kernel_MemoryPool_Free(&arm_memory_pool, first);
    after = ARM_ReadPrimask();
    if (!ARM_MemoryPool_Check(before == after) ||
        !ARM_MemoryPool_Check(free_result == 0))
    {
        return -1;
    }

    reused = Kernel_MemoryPool_Alloc(&arm_memory_pool);
    if (!ARM_MemoryPool_Check(reused == first))
    {
        return -1;
    }

    if (!ARM_MemoryPool_Check(
            Kernel_MemoryPool_Free(&arm_memory_pool, reused) == 0) ||
        !ARM_MemoryPool_Check(
            Kernel_MemoryPool_Free(&arm_memory_pool, second) == 0) ||
        !ARM_MemoryPool_Check(
            Kernel_MemoryPool_GetFreeCount(&arm_memory_pool) ==
            ARM_POOL_BLOCK_COUNT))
    {
        return -1;
    }

    return 0;
}

int main(void)
{
    Kernel_Trace_Init();

    if (ARM_RunMemoryPoolSmokeTest() != 0)
    {
        for (;;)
        {
            __asm volatile ("nop");
        }
    }
        ARM_TaskInit(&task_a, 1, "task_a", TaskA_Entry);
    ARM_TaskInit(&task_b, 2, "task_b", TaskB_Entry);

    /* Verify initial stack high-water estimates on ARM. */
    arm_stack_high_water_test_checks = 0;
    arm_stack_high_water_test_failures = 0;

    if (Task_GetStackHighWaterUsage(&task_a) == 64U)
    {
        arm_stack_high_water_test_checks++;
    }
    else
    {
        arm_stack_high_water_test_failures++;
    }

    if (Task_GetStackHighWaterUsage(&task_b) == 64U)
    {
        arm_stack_high_water_test_checks++;
    }
    else
    {
        arm_stack_high_water_test_failures++;
    }

    /* Verify stack-warning behavior on ARM. */
    arm_stack_runtime_warning_events = 0;
    arm_stack_runtime_warning_task_id = -1;

    arm_stack_warning_test_checks = 0;
    arm_stack_warning_test_failures = 0;

    uintptr_t warning_original_sp = task_b.context.sp;
    uintptr_t warning_original_watermark = task_b.stack_low_watermark;

    task_b.context.sp =
        (uintptr_t)&task_b.stack[TASK_STACK_SIZE - 188];
    Task_UpdateStackHighWater(&task_b);
    int warning_below_threshold = Task_IsStackUsageWarning(&task_b);

    task_b.context.sp =
        (uintptr_t)&task_b.stack[TASK_STACK_SIZE - 190];
    Task_UpdateStackHighWater(&task_b);
    int warning_above_threshold = Task_IsStackUsageWarning(&task_b);

    int warning_invalid_input = Task_IsStackUsageWarning(NULL);

    /* Restore test state before scheduler startup. */
    task_b.context.sp = warning_original_sp;
    task_b.stack_low_watermark = warning_original_watermark;

    if (warning_below_threshold == 0)
    {
        arm_stack_warning_test_checks++;
    }
    else
    {
        arm_stack_warning_test_failures++;
    }

    if (warning_above_threshold == 1)
    {
        arm_stack_warning_test_checks++;
    }
    else
    {
        arm_stack_warning_test_failures++;
    }

    if (warning_invalid_input == -1)
    {
        arm_stack_warning_test_checks++;
    }
    else
    {
        arm_stack_warning_test_failures++;
    }

    /* Verify task-context restart before scheduler startup. */
    arm_context_restart_test_checks = 0;
    arm_context_restart_test_failures = 0;

    task_b.restart_count = 7UL;
    task_b.successful_recoveries = 3UL;
    task_b.stack_warning_reported = 1;
    task_b.stack_low_watermark =
        (uintptr_t)&task_b.stack[TASK_STACK_SIZE - 32];
    task_b.stack[0] = 0U;
    task_b.stack[20] = 0xDEADBEEFUL;

    int restart_result = Task_RestartContext(&task_b);
    if (restart_result == 0 &&
        Task_ValidateContext(&task_b) == 0 &&
        task_b.context.sp ==
            (uintptr_t)&task_b.stack[TASK_STACK_SIZE - 16] &&
        task_b.stack_low_watermark == task_b.context.sp &&
        task_b.stack_warning_reported == 0 &&
        task_b.stack[20] != 0xDEADBEEFUL &&
        task_b.stack[TASK_STACK_SIZE - 8] ==
            (uint32_t)(uintptr_t)&task_b &&
        task_b.stack[TASK_STACK_SIZE - 3] ==
            (uint32_t)(uintptr_t)TaskExit_Handler &&
        task_b.stack[TASK_STACK_SIZE - 2] ==
            (uint32_t)(uintptr_t)TaskB_Entry &&
        task_b.stack[TASK_STACK_SIZE - 1] == 0x01000000UL &&
        task_b.id == 2 &&
        task_b.priority == 1 &&
        task_b.task_function == TaskB_Entry &&
        task_b.initial_stack_argument == (uintptr_t)&task_b &&
        task_b.initial_stack_return_handler ==
            (uintptr_t)TaskExit_Handler &&
        task_b.restart_count == 7UL &&
        task_b.successful_recoveries == 3UL)
    {
        arm_context_restart_test_checks++;
    }
    else
    {
        arm_context_restart_test_failures++;
    }

    if (Task_RestartContext(NULL) == -1)
    {
        arm_context_restart_test_checks++;
    }
    else
    {
        arm_context_restart_test_failures++;
    }

    /* Reset smoke-test metadata before testing scheduler recovery. */
    ARM_TaskInit(&task_b, 2, "task_b", TaskB_Entry);

    /* Exercise scheduler-integrated recovery before runtime startup. */
    arm_scheduler_recovery_test_checks = 0;
    arm_scheduler_recovery_test_failures = 0;

    TaskRegistry recovery_test_registry;
    Scheduler_Init(&recovery_test_registry);

    if (Scheduler_AddTask(&recovery_test_registry, &task_b) == 0)
    {
        uint32_t expected_fill = task_b.stack[20];

        ARM_TaskInit(&arm_recovery_owner, 90, "RecoveryOwner", TaskB_Entry);
        ARM_TaskInit(&arm_recovery_waiter, 91, "RecoveryWaiter", TaskB_Entry);

        Kernel_Mutex_Init(&arm_recovery_owned_mutex);
        Kernel_Mutex_Init(&arm_recovery_waiting_mutex);
        Kernel_IPC_Init();

        int ipc_waiting_before = Kernel_IPC_GetWaitingCount();

        int resource_setup_ok =
            Kernel_Mutex_Lock(&arm_recovery_owned_mutex, &task_b) == 0 &&
            Kernel_Mutex_Lock(&arm_recovery_owned_mutex,
                              &arm_recovery_waiter) == 1 &&
            Kernel_Mutex_Lock(&arm_recovery_waiting_mutex,
                              &arm_recovery_owner) == 0 &&
            Kernel_Mutex_Lock(&arm_recovery_waiting_mutex, &task_b) == 1;

        if (resource_setup_ok)
        {
            task_b.state = TASK_RUNNING;
            Kernel_IPC_RegisterReceiver(&task_b);
            resource_setup_ok =
                Kernel_IPC_GetWaitingCount() == ipc_waiting_before + 1;
        }

        if (resource_setup_ok)
        {
            Kernel_IPC_Wait(&task_b);
            task_b.wake_reason = WAKE_TIMEOUT;
            task_b.ipc_timeout_tick = Scheduler_GetTick() + 99;
        }

        task_b.state = TASK_FAULTED;
        task_b.fault_type = FAULT_RUNTIME;
        task_b.stack_warning_reported = 1;
        task_b.stack_low_watermark =
            (uintptr_t)&task_b.stack[TASK_STACK_SIZE - 32];
        task_b.stack[0] = 0U;
        task_b.stack[20] = 0xDEADBEEFUL;
        task_b.stack[TASK_STACK_SIZE - 2] = 0U;

        Scheduler_RecoverFaultedTask(
            &recovery_test_registry, &task_b);

        if (resource_setup_ok &&
            task_b.state == TASK_READY &&
            task_b.recovery_pending == 1 &&
            task_b.recovery_failed == 0 &&
            task_b.recovery_attempts == 1UL &&
            task_b.restart_count == 1UL &&
            recovery_test_registry.recovery_events == 1UL &&
            Task_ValidateContext(&task_b) == 0 &&
            task_b.context.sp ==
                (uintptr_t)&task_b.stack[TASK_STACK_SIZE - 16] &&
            task_b.stack_low_watermark == task_b.context.sp &&
            task_b.stack_warning_reported == 0 &&
            task_b.stack[0] != 0U &&
            task_b.stack[20] == expected_fill &&
            task_b.stack[TASK_STACK_SIZE - 8] ==
                (uint32_t)(uintptr_t)&task_b &&
            task_b.stack[TASK_STACK_SIZE - 3] ==
                (uint32_t)(uintptr_t)TaskExit_Handler &&
            task_b.stack[TASK_STACK_SIZE - 2] ==
                (uint32_t)(uintptr_t)TaskB_Entry &&
            task_b.stack[TASK_STACK_SIZE - 1] == 0x01000000UL &&
            task_b.owned_mutexes == NULL &&
            task_b.waiting_mutex == NULL &&
            task_b.mutex_waiting == 0 &&
            task_b.mutex_hold_active == 0 &&
            task_b.wake_reason == WAKE_NONE &&
            task_b.ipc_timeout_tick == 0 &&
            Kernel_IPC_GetWaitingCount() == ipc_waiting_before &&
            arm_recovery_owned_mutex.owner == &arm_recovery_waiter &&
            arm_recovery_waiter.owned_mutexes == &arm_recovery_owned_mutex &&
            arm_recovery_waiting_mutex.waiter_count == 0 &&
            arm_recovery_waiting_mutex.owner == &arm_recovery_owner &&
            arm_recovery_owner.priority == arm_recovery_owner.base_priority &&
            arm_recovery_waiter.state == TASK_READY &&
            Kernel_Mutex_ValidateState(&arm_recovery_owned_mutex) == 0 &&
            Kernel_Mutex_ValidateState(&arm_recovery_waiting_mutex) == 0)
        {
            arm_scheduler_recovery_test_checks++;
        }
        else
        {
            arm_scheduler_recovery_test_failures++;
        }

                /* Verify recovery after genuine forward progress. */
        task_b.state = TASK_RUNNING;
        task_b.progress_count =
            task_b.recovery_start_progress_count + 1UL;
        task_b.progress_reported = 1;

        Scheduler_VerifyRecovery(
            &recovery_test_registry,
            &task_b
        );

        if (task_b.state == TASK_RUNNING &&
            task_b.recovery_pending == 0 &&
            task_b.successful_recoveries == 1UL &&
            task_b.recovery_failed == 0 &&
            task_b.fault_type == FAULT_NONE)
        {
            arm_scheduler_recovery_test_checks++;
        }
        else
        {
            arm_scheduler_recovery_test_failures++;
        }

        /* Invalid entry function must never produce a READY task. */
        task_b.task_function = NULL;
        task_b.state = TASK_FAULTED;
        task_b.recovery_pending = 0;
        task_b.recovery_failed = 0;
        recovery_test_registry.reschedule_pending = 0;
        recovery_test_registry.reschedule_reason = RESCHEDULE_NONE;

        Scheduler_RecoverFaultedTask(
            &recovery_test_registry, &task_b);

        if (task_b.state == TASK_FAULTED &&
            task_b.recovery_pending == 0 &&
            task_b.recovery_failed == 1 &&
            recovery_test_registry.reschedule_pending == 1 &&
            recovery_test_registry.reschedule_reason == RESCHEDULE_FAULT)
        {
            arm_scheduler_recovery_test_checks++;
        }
        else
        {
            arm_scheduler_recovery_test_failures++;
        }

                /* Maximum recovery attempts must isolate the task. */
        task_b.recovery_attempts = MAX_RECOVERY_ATTEMPTS;
        task_b.state = TASK_FAULTED;
        task_b.isolated = 0;

        Scheduler_RecoverFaultedTask(
            &recovery_test_registry, &task_b);

        if (task_b.isolated == 1 &&
            task_b.recovery_failed == 1 &&
            task_b.state == TASK_FAULTED &&
            task_b.recovery_attempts == MAX_RECOVERY_ATTEMPTS)
        {
            arm_scheduler_recovery_test_checks++;
        }
        else
        {
            arm_scheduler_recovery_test_failures++;
        }

    }

    /* Restore a clean Task B for the real scheduler run. */
    ARM_TaskInit(&task_b, 2, "task_b", TaskB_Entry);
    Scheduler_Init(&arm_scheduler);

    if (Scheduler_AddTask(&arm_scheduler, &task_a) != 0 ||
        Scheduler_AddTask(&arm_scheduler, &task_b) != 0)
    {
        for (;;)
        {
            __asm volatile ("nop");
        }
    }

    arm_scheduler.current_task = &task_a;
arm_scheduler.last_selected_index = 0;
task_a.state = TASK_RUNNING;

g_current_tcb = &task_a;
g_current_saved_sp = &task_a.context.sp;

    /*
     * STM32L476RG reset-clock assumption: MSI at 4 MHz.
     * 40,000 clock cycles gives a nominal 10 ms tick.
     */
    SCB_SHPR3 = 0xF0F00000UL; /* SysTick and PendSV share the lowest priority */
    SYST_RVR = 40000UL - 1UL;
    SYST_CVR = 0UL;
    SYST_CSR = SYSTICK_CLKSOURCE |
                SYSTICK_TICKINT |
                SYSTICK_ENABLE;

    ARM_StartFirstTask();

    for (;;)
    {
        /* The first task should never return here. */
    }
}
