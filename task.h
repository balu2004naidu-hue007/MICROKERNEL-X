#include <stdint.h>
#include <stddef.h>
#ifndef TASK_H
#define TASK_H

#define DEFAULT_TIME_SLICE 2
#define TASK_STACK_SIZE 256

typedef enum
{
 TASK_READY,
 TASK_RUNNING,
 TASK_BLOCKED,
 TASK_WAITING,
 TASK_FAULTED
} TaskState;

typedef enum
{
    WAKE_NONE,
    WAKE_IPC,
    WAKE_TIMEOUT
} TaskWakeReason;

typedef enum
{
    FAULT_NONE,
    FAULT_WATCHDOG,
    FAULT_RUNTIME
} TaskFaultType;

typedef enum
{
    FAULT_INJECT_NONE,
    FAULT_INJECT_WATCHDOG
} FaultInjectionType;

typedef struct TaskControlBlock TaskControlBlock;
typedef struct KernelMutex KernelMutex;

typedef struct
{
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;

} TaskSoftwareContext;


typedef struct
{
    uintptr_t sp;
    uintptr_t lr;
    uintptr_t pc;
    uint32_t xpsr;
} TaskContext;


typedef struct
{
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t xpsr;


} TaskInitialStackFrame;

_Static_assert(
    sizeof(TaskSoftwareContext) == 8 * sizeof(uint32_t),
    "Invalid software context size");

_Static_assert(
    sizeof(TaskInitialStackFrame) == 8 * sizeof(uint32_t),
    "Invalid initial stack frame size");

_Static_assert(
    offsetof(TaskInitialStackFrame, pc) == 6 * sizeof(uint32_t),
    "Invalid initial frame PC offset");

_Static_assert(
    offsetof(TaskInitialStackFrame, xpsr) == 7 * sizeof(uint32_t),
    "Invalid initial frame xPSR offset");

_Static_assert(
    offsetof(TaskContext, sp) == 0,
    "Saved stack pointer must be the first context field");

struct TaskControlBlock
{
    int id;
    TaskContext context;
    _Alignas(8) uint32_t stack[TASK_STACK_SIZE];
    uintptr_t stack_low_watermark;
    int priority;
    int base_priority;
    const char* name;
    TaskState state;
    TaskWakeReason wake_reason;
    int is_idle;
    unsigned long run_count;
    unsigned long restart_count;
    int recovery_pending;

    TaskFaultType fault_type;
    unsigned long fault_count;
    unsigned long recovery_attempts;
    unsigned long successful_recoveries;
    int recovery_failed;
    int isolated;
    FaultInjectionType fault_injection;
    int fault_injection_enabled;
    int recovery_failure_injection_enabled;
    int yield_requested;
    unsigned long execution_units;
    unsigned long watchdog_last_execution_units;
    unsigned long progress_count;
    int mutex_hold_active;
    int mutex_hold_phase;
    int mutex_waiting;
    KernelMutex* waiting_mutex;
    KernelMutex* owned_mutexes;
    unsigned long recovery_start_progress_count;
    unsigned long last_progress_tick;
    unsigned long last_heartbeat_tick;
    unsigned long last_progress_count;
    int progress_enabled;
    int progress_reported;
    unsigned long time_slice;
    unsigned long wakeup_tick;
    unsigned long ipc_timeout_tick;
    void (*task_function)(TaskControlBlock*);
    int stack_warning_reported;
    uintptr_t initial_stack_argument;
    uintptr_t initial_stack_return_handler;
    int recovery_in_progress;
};

void Task_Init(TaskControlBlock* task,
               int id,
               int priority,
               const char* name,
               void (*task_function)(TaskControlBlock*));
int Task_RestartContext(TaskControlBlock* task);
int Task_ValidateContext(const TaskControlBlock* task);
size_t Task_GetStackHighWaterUsage(const TaskControlBlock* task);
void Task_UpdateStackHighWater(TaskControlBlock* task);
/* Returns 1 for warning, 0 below threshold, -1 for invalid input. */
int Task_IsStackUsageWarning(const TaskControlBlock* task);

#endif
