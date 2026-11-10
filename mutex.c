#include <stdio.h>
#include "mutex.h"
#include "scheduler.h"
#include "trace.h"
#include "kernel_critical.h"

#ifdef MICROKERNEL_ARM
#define printf(...) ((void)0)
#endif

static int Kernel_Mutex_ValidateStateInternal(
    const KernelMutex* mutex);

static int Kernel_Mutex_IsOwnerInternal(
    const KernelMutex* mutex,
    const TaskControlBlock* task);

static void Kernel_Mutex_RecalculatePriorityInternal(
    KernelMutex* mutex);

static void Kernel_Mutex_InitInternal(
    KernelMutex* mutex);

static int Kernel_Mutex_CleanupTaskInternal(
    TaskControlBlock* task);

static int Kernel_Mutex_LockInternal(
    KernelMutex* mutex,
    TaskControlBlock* task);

static int Kernel_Mutex_UnlockInternal(
    KernelMutex* mutex,
    TaskControlBlock* task);

static void Kernel_Mutex_RecalculateTaskPriority(
    TaskControlBlock* task,
    TaskControlBlock* visited[MAX_TASKS],
    int depth);

static void Kernel_Mutex_InitInternal(KernelMutex* mutex)
{
    if (mutex == NULL)
    {
        return;
    }

    mutex->locked = 0;
    mutex->owner = NULL;
    mutex->next_owned = NULL;
    mutex->waiter_count = 0;

    for (int i = 0; i < MUTEX_MAX_WAITERS; i++)
    {
        mutex->waiters[i] = NULL;
    }
}

void Kernel_Mutex_Init(KernelMutex* mutex)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    Kernel_Mutex_InitInternal(mutex);

    Kernel_CriticalExit(state);
}

static int Kernel_Mutex_CanAddOwned(
    TaskControlBlock* task,
    KernelMutex* mutex)
{
    if (task == NULL || mutex == NULL)
    {
        return -1;
    }

    KernelMutex* slow = task->owned_mutexes;
    KernelMutex* fast = task->owned_mutexes;

    while (fast != NULL && fast->next_owned != NULL)
    {
        slow = slow->next_owned;
        fast = fast->next_owned->next_owned;

        if (slow == fast)
        {
            return -2;
        }
    }

    for (KernelMutex* current = task->owned_mutexes;
         current != NULL;
         current = current->next_owned)
    {
        if (current == mutex)
        {
            return -3;
        }
    }

    return 0;
}

static int Kernel_Mutex_AddOwned(
    TaskControlBlock* task,
    KernelMutex* mutex)
{
    int validation_result =
        Kernel_Mutex_CanAddOwned(task, mutex);

    if (validation_result != 0)
    {
        return validation_result;
    }

    mutex->next_owned = task->owned_mutexes;
    task->owned_mutexes = mutex;

    return 0;
}

static int Kernel_Mutex_RemoveOwned(
    TaskControlBlock* task,
    KernelMutex* mutex)
{
    if (task == NULL || mutex == NULL)
    {
        return -1;
    }

    KernelMutex* slow = task->owned_mutexes;
    KernelMutex* fast = task->owned_mutexes;

    while (fast != NULL && fast->next_owned != NULL)
    {
        slow = slow->next_owned;
        fast = fast->next_owned->next_owned;

        if (slow == fast)
        {
            return -3;
        }
    }

    KernelMutex** link = &task->owned_mutexes;

    while (*link != NULL)
    {
        if (*link == mutex)
        {
            *link = mutex->next_owned;
            mutex->next_owned = NULL;
            return 0;
        }

        link = &(*link)->next_owned;
    }

    return -2;
}

static int Kernel_Mutex_CleanupTaskInternal(TaskControlBlock* task)
{
    if (task == NULL)
    {
        return -1;
    }

    /*
     * Remove the task from its mutex wait queue without
     * unblocking it. Recovery owns the state transition.
     */
    KernelMutex* waiting_mutex = task->waiting_mutex;

    if (waiting_mutex != NULL)
    {
        if (waiting_mutex->waiter_count < 0 ||
            waiting_mutex->waiter_count > MUTEX_MAX_WAITERS)
        {
            return -2;
        }

        int found = -1;

        for (int i = 0; i < waiting_mutex->waiter_count; i++)
        {
            if (waiting_mutex->waiters[i] == task)
            {
                found = i;
                break;
            }
        }

        if (found < 0)
        {
            return -3;
        }

        for (int i = found;
             i < waiting_mutex->waiter_count - 1;
             i++)
        {
            waiting_mutex->waiters[i] =
                waiting_mutex->waiters[i + 1];
        }

        waiting_mutex->waiters[waiting_mutex->waiter_count - 1] =
            NULL;
        waiting_mutex->waiter_count--;

        task->waiting_mutex = NULL;
        task->mutex_waiting = 0;

        Kernel_Mutex_RecalculatePriorityInternal(waiting_mutex);
    }
    else
    {
        task->mutex_waiting = 0;
    }

    /*
     * Release every mutex owned by the faulted task.
     * Normal unlock logic preserves waiter priority ordering
     * and transfers ownership when a waiter exists.
     */
    while (task->owned_mutexes != NULL)
    {
        KernelMutex* owned = task->owned_mutexes;

        if (!owned->locked || owned->owner != task)
        {
            return -4;
        }

        if (Kernel_Mutex_UnlockInternal(owned, task) != 0)
        {
            return -5;
        }
    }

    task->waiting_mutex = NULL;
    task->mutex_waiting = 0;
    task->mutex_hold_active = 0;
    task->mutex_hold_phase = 0;
    task->priority = task->base_priority;

    /* Do not change task->state here. */

    return 0;
}

int Kernel_Mutex_CleanupTask(TaskControlBlock* task)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_CleanupTaskInternal(task);

    Kernel_CriticalExit(state);

    return result;
}

static int Kernel_Mutex_WouldDeadlock(
    KernelMutex* mutex,
    TaskControlBlock* task)
{
    if (mutex == NULL || task == NULL)
    {
        return 0;
    }

    TaskControlBlock* owner = mutex->owner;

    /*
     * Follow the dependency chain:
     *
     * task
     *   -> mutex it wants
     *   -> current owner
     *   -> mutex that owner wants
     *   -> next owner
     *
     * If we reach the original task, a circular
     * wait exists.
     */
    for (int depth = 0;
         depth < MUTEX_MAX_WAITERS;
         depth++)
    {
        if (owner == NULL)
        {
            return 0;
        }

        if (owner == task)
        {
            return 1;
        }

        if (owner->waiting_mutex == NULL)
        {
            return 0;
        }

        owner = owner->waiting_mutex->owner;
    }

    return 0;
}

static int Kernel_Mutex_LockInternal(KernelMutex* mutex,
                                     TaskControlBlock* task)
{
    if (mutex == NULL || task == NULL)
    {
        return -1;
    }

    int validation_result =
    Kernel_Mutex_ValidateStateInternal(mutex);

if (validation_result != 0)
{
    return -3;
}

    /*
     * Mutex is free.
     */
    if (!mutex->locked)
    {
        int add_result = Kernel_Mutex_AddOwned(task, mutex);

        if (add_result != 0)
        {
            printf("Mutex: ownership insertion failed for %s "
                   "(error %d)\n",
                   task->name,
                   add_result);
            return -2;
        }

        mutex->locked = 1;
        mutex->owner = task;
        task->waiting_mutex = NULL;

        printf("Mutex: acquired by %s\n",
               task->name);

        Kernel_Trace(TRACE_MUTEX_LOCK, task);
        return 0;
    }

    /*
     * Recursive locking by the current owner is not
     * supported by this mutex.
     */
    if (mutex->owner == task)
    {
        printf("Mutex: recursive lock rejected -> %s\n",
               task->name);

        return -2;
    }

    if (task->mutex_waiting || task->waiting_mutex != NULL)
{
    printf("Mutex: task already waiting -> %s\n",
           task->name);
    return -5;
}

    /*
 * Detect circular wait before blocking.
 */
if (Kernel_Mutex_WouldDeadlock(mutex, task))
{
    printf("Mutex: deadlock detected -> %s\n",
           task->name);

    return -4;
}

    /*
     * Do not insert the same task twice.
     */
    for (int i = 0; i < mutex->waiter_count; i++)
    {
        if (mutex->waiters[i] == task)
        {
            return 1;
        }
    }

    if (mutex->waiter_count >= MUTEX_MAX_WAITERS)
    {
        printf("Mutex: waiter queue full -> %s\n",
               task->name);

        return -3;
    }

    /*
     * Add task to the waiting list.
     */
    mutex->waiters[mutex->waiter_count] = task;
    mutex->waiter_count++;

    Scheduler_Block(task);
    task->mutex_waiting = 1;
    task->waiting_mutex = mutex;
    Kernel_Mutex_RecalculatePriorityInternal(mutex);
    Kernel_Trace(TRACE_MUTEX_WAIT, task);

    printf("Mutex: BUSY -> owner %s | blocked %s\n",
           mutex->owner->name,
           task->name);

    return 1;
}

static int Kernel_Mutex_UnlockInternal(KernelMutex* mutex,
                                       TaskControlBlock* task)
{
    if (mutex == NULL || task == NULL)
    {
        return -1;
    }

    if (!mutex->locked)
    {
        printf("Mutex: unlock rejected - mutex already free\n");
        return -2;
    }

    /*
     * Only the owner can unlock the mutex.
     */
    if (!Kernel_Mutex_IsOwnerInternal(mutex, task))
    {
        printf("Mutex: unlock rejected - %s is not owner\n",
               task->name);

        return -3;
    }

    int validation_result = Kernel_Mutex_ValidateStateInternal(mutex);

if (validation_result != 0)
{
    printf("Mutex: unlock rejected - invalid state (error %d)\n",
           validation_result);
    return -4;
}

    /*
     * No waiting tasks.
     */

     if (mutex->waiter_count == 0)
{

    int remove_result = Kernel_Mutex_RemoveOwned(task, mutex);

if (remove_result != 0)
{
    printf("Mutex: ownership removal failed for %s (error %d)\n",
           task->name,
           remove_result);
    return -4;
}

mutex->locked = 0;
    mutex->owner = NULL;

    TaskControlBlock* visited[MAX_TASKS];

    Kernel_Mutex_RecalculateTaskPriority(
        task,
        visited,
        0);

    printf("Mutex: released by %s\n", task->name);


    Kernel_Trace(TRACE_MUTEX_UNLOCK, task);

    return 0;
}

    /*
     * Select highest-priority waiter.
     * Equal priorities are resolved by queue order.
     */
    int selected_index = 0;

    for (int i = 1; i < mutex->waiter_count; i++)
    {
        if (mutex->waiters[i]->priority >
            mutex->waiters[selected_index]->priority)
        {
            selected_index = i;
        }
    }

    TaskControlBlock* next_owner =
        mutex->waiters[selected_index];

     int add_validation =
     Kernel_Mutex_CanAddOwned(next_owner, mutex);

    if (add_validation != 0)
    {
        printf("Mutex: ownership transfer rejected for %s "
               "(error %d)\n",
               next_owner->name,
               add_validation);
        return -5;
    }

    /*
     * Remove selected waiter from the queue.
     */
    for (int i = selected_index;
         i < mutex->waiter_count - 1;
         i++)
    {
        mutex->waiters[i] = mutex->waiters[i + 1];
    }

    mutex->waiters[mutex->waiter_count - 1] = NULL;
    mutex->waiter_count--;

    /*
     * Direct ownership transfer.
     *
     * This avoids a race where another task could acquire
     * the mutex between release and waiter wake-up.
     */
        int remove_result = Kernel_Mutex_RemoveOwned(task, mutex);

    if (remove_result != 0)
    {
        for (int i = mutex->waiter_count;
             i > selected_index;
             i--)
        {
            mutex->waiters[i] = mutex->waiters[i - 1];
        }

        mutex->waiters[selected_index] = next_owner;
        mutex->waiter_count++;

        printf("Mutex: ownership transfer aborted for %s (error %d)\n",
               task->name,
               remove_result);

        return -4;
    }

    /*
     * Add the mutex to the new owner's ownership list
     * before publishing the ownership transfer.
     */
    int add_result = Kernel_Mutex_AddOwned(next_owner, mutex);

    if (add_result != 0)
    {
        int restore_result = Kernel_Mutex_AddOwned(task, mutex);

        for (int i = mutex->waiter_count;
             i > selected_index;
             i--)
        {
            mutex->waiters[i] = mutex->waiters[i - 1];
        }

        mutex->waiters[selected_index] = next_owner;
        mutex->waiter_count++;

        if (restore_result != 0)
        {
            printf("Mutex: ownership rollback failed for %s "
                   "(error %d)\n",
                   task->name,
                   restore_result);
            return -6;
        }

        printf("Mutex: ownership transfer aborted for %s "
               "(error %d)\n",
               next_owner->name,
               add_result);

        return -5;
    }

    TaskControlBlock* visited[MAX_TASKS];

    Kernel_Mutex_RecalculateTaskPriority(
        task,
        visited,
        0);

    mutex->owner = next_owner;
    mutex->locked = 1;

    Kernel_Trace(TRACE_MUTEX_UNLOCK, task);

    next_owner->mutex_waiting = 0;
    next_owner->waiting_mutex = NULL;
    Kernel_Mutex_RecalculatePriorityInternal(mutex);

    Scheduler_Unblock(next_owner);

    printf("Mutex: released by %s\n",
           task->name);

    printf("Mutex: ownership transferred -> %s\n",
           next_owner->name);

    return 0;
}

int Kernel_Mutex_Lock(KernelMutex* mutex,
                      TaskControlBlock* task)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_LockInternal(mutex, task);

    Kernel_CriticalExit(state);

    return result;
}

int Kernel_Mutex_Unlock(KernelMutex* mutex,
                        TaskControlBlock* task)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_UnlockInternal(mutex, task);

    Kernel_CriticalExit(state);

    return result;
}

static int Kernel_Mutex_IsLockedInternal(const KernelMutex* mutex)
{
    if (mutex == NULL)
    {
        return 0;
    }

    return mutex->locked;
}

static TaskControlBlock* Kernel_Mutex_GetOwnerInternal(
    const KernelMutex* mutex)
{
    if (mutex == NULL)
    {
        return NULL;
    }

    return mutex->owner;
}

static int Kernel_Mutex_IsOwnerInternal(
    const KernelMutex* mutex,
    const TaskControlBlock* task)
{
    if (mutex == NULL || task == NULL)
    {
        return 0;
    }

    return mutex->locked &&
           mutex->owner == task;
}

int Kernel_Mutex_IsLocked(const KernelMutex* mutex)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_IsLockedInternal(mutex);

    Kernel_CriticalExit(state);

    return result;
}

TaskControlBlock* Kernel_Mutex_GetOwner(
    const KernelMutex* mutex)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    TaskControlBlock* result =
        Kernel_Mutex_GetOwnerInternal(mutex);

    Kernel_CriticalExit(state);

    return result;
}

int Kernel_Mutex_IsOwner(
    const KernelMutex* mutex,
    const TaskControlBlock* task)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_IsOwnerInternal(mutex, task);

    Kernel_CriticalExit(state);

    return result;
}

static int Kernel_Mutex_AbortWaitInternal(
    KernelMutex* mutex,
    TaskControlBlock* task)
{
    if (mutex == NULL || task == NULL)
    {
        return -1;
    }

    int found = -1;

    /*
     * The task may already be present in the
     * wait queue if it was blocked before recovery.
     */
    for (int i = 0; i < mutex->waiter_count; i++)
    {
        if (mutex->waiters[i] == task)
        {
            found = i;
            break;
        }
    }

        /*
     * Do not abort a wait belonging to another mutex.
     */
    if (task->waiting_mutex != NULL &&
        task->waiting_mutex != mutex)
    {
        return -3;
    }

    /*
     * The task is not queued on this mutex.
     * Allow an already-runnable task to complete
     * an idempotent recovery request.
     */
    if (found < 0)
    {
        if (task->mutex_waiting ||
            task->waiting_mutex != NULL)
        {
            return -4;
        }

        if (task->state == TASK_BLOCKED ||
            task->state == TASK_WAITING)
        {
            return -5;
        }

        return 0;
    }

    /*
     * The queue entry and task waiting state must agree.
     */
    if (!task->mutex_waiting ||
        task->waiting_mutex != mutex)
    {
        return -6;
    }

    /*
     * If the task is already a waiter, remove it.
     */
    if (found >= 0)
    {
        for (int i = found;
             i < mutex->waiter_count - 1;
             i++)
        {
            mutex->waiters[i] = mutex->waiters[i + 1];
        }

        mutex->waiters[mutex->waiter_count - 1] = NULL;
        mutex->waiter_count--;
    }

    /*
     * Cancel the task's pending mutex dependency.
     */
    task->mutex_waiting = 0;
    task->waiting_mutex = NULL;

    TaskControlBlock* visited[MAX_TASKS];

    Kernel_Mutex_RecalculateTaskPriority(
    task,
    visited,
    0);

    /*
     * Make the task runnable again.
     */
    if (task->state == TASK_BLOCKED ||
        task->state == TASK_WAITING)
    {
        Scheduler_Unblock(task);
    }
    else
    {
        task->state = TASK_READY;
    }

    /*
     * Recalculate priority inheritance for
     * the mutex owner and remaining waiters.
     */
    Kernel_Mutex_RecalculatePriorityInternal(mutex);

    printf("Mutex: deadlock recovery -> %s\n",
           task->name);

    return 0;
}

int Kernel_Mutex_AbortWait(
    KernelMutex* mutex,
    TaskControlBlock* task)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_AbortWaitInternal(mutex, task);

    Kernel_CriticalExit(state);

    return result;
}

static int Kernel_Mutex_ValidateStateInternal(
    const KernelMutex* mutex)
{
    if (mutex == NULL)
    {
        return -1;
    }

    /*
     * An unlocked mutex must not have an owner.
     */
    if (!mutex->locked && mutex->owner != NULL)
    {
        return -2;
    }

    /*
     * A locked mutex must have an owner.
     */
    if (mutex->locked && mutex->owner == NULL)
    {
        return -3;
    }

    /*
     * Waiter count must remain within bounds.
     */
    if (mutex->waiter_count < 0 ||
        mutex->waiter_count > MUTEX_MAX_WAITERS)
    {
        return -4;
    }

    /*
     * Every active waiter must be a valid task
     * and must indicate that it is waiting for
     * this mutex.
     */
    for (int i = 0; i < mutex->waiter_count; i++)
    {
        TaskControlBlock* waiter = mutex->waiters[i];

        if (waiter == NULL)
        {
            return -5;
        }

        if (!waiter->mutex_waiting)
        {
            return -6;
        }

        if (waiter->waiting_mutex != mutex)
        {
            return -7;
        }

                for (int j = i + 1; j < mutex->waiter_count; j++)
        {
            if (mutex->waiters[j] == waiter)
            {
                return -8;
            }
        }

    }


    return 0;
}

int Kernel_Mutex_ValidateState(
    const KernelMutex* mutex)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_ValidateStateInternal(mutex);

    Kernel_CriticalExit(state);

    return result;
}

static int Kernel_Mutex_RemoveWaiterInternal(
    KernelMutex* mutex,
    TaskControlBlock* task)
{
    if (mutex == NULL || task == NULL)
    {
        return -1;
    }

    int found = -1;

    for (int i = 0; i < mutex->waiter_count; i++)
    {
        if (mutex->waiters[i] == task)
        {
            found = i;
            break;
        }
    }

    if (found == -1)
    {
        return -2;
    }

    /*
     * Remove the waiter while preserving
     * the order of the remaining waiters.
     */
    for (int i = found;
         i < mutex->waiter_count - 1;
         i++)
    {
        mutex->waiters[i] = mutex->waiters[i + 1];
    }

    mutex->waiters[mutex->waiter_count - 1] = NULL;
    mutex->waiter_count--;

    task->mutex_waiting = 0;
    task->waiting_mutex = NULL;

    TaskControlBlock* visited[MAX_TASKS];

    Kernel_Mutex_RecalculateTaskPriority(
        task,
        visited,
        0);

    if (task->state == TASK_BLOCKED ||
        task->state == TASK_WAITING)
    {
        Scheduler_Unblock(task);
    }

    Kernel_Mutex_RecalculatePriorityInternal(mutex);

    return 0;
}

int Kernel_Mutex_RemoveWaiter(
    KernelMutex* mutex,
    TaskControlBlock* task)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    int result = Kernel_Mutex_RemoveWaiterInternal(mutex, task);

    Kernel_CriticalExit(state);

    return result;
}

static void Kernel_Mutex_RecalculateTaskPriority(
    TaskControlBlock* task,
    TaskControlBlock* visited[MAX_TASKS],
    int depth)
{
    if (task == NULL || depth >= MAX_TASKS)
    {
        return;
    }

    /* Prevent repeated traversal of dependency chains. */
    for (int i = 0; i < depth; i++)
    {
        if (visited[i] == task)
        {
            return;
        }
    }

    visited[depth] = task;

    /*
     * Calculate effective priority across every mutex
     * currently owned by this task.
     */
    int highest_priority = task->base_priority;

        /* Reject a corrupted owned-mutex list instead of looping forever. */
    KernelMutex* slow = task->owned_mutexes;
    KernelMutex* fast = task->owned_mutexes;

    while (fast != NULL && fast->next_owned != NULL)
    {
        slow = slow->next_owned;
        fast = fast->next_owned->next_owned;

        if (slow == fast)
        {
            return;
        }
    }

    for (KernelMutex* owned = task->owned_mutexes;
         owned != NULL;
         owned = owned->next_owned)
    {
        if (!owned->locked || owned->owner != task)
        {
            continue;
        }

        for (int i = 0;
             i < owned->waiter_count &&
             i < MUTEX_MAX_WAITERS;
             i++)
        {
            TaskControlBlock* waiter = owned->waiters[i];

            if (waiter != NULL &&
                waiter->priority > highest_priority)
            {
                highest_priority = waiter->priority;
            }
        }
    }

    if (task->priority != highest_priority)
    {
        int old_priority = task->priority;
        task->priority = highest_priority;

        if (highest_priority > old_priority)
        {
            printf("Mutex: Priority inheritance -> %s "
                   "(%d -> %d)\n",
                   task->name,
                   old_priority,
                   highest_priority);

            Kernel_Trace(TRACE_PRIORITY_INHERIT, task);
        }
        else
        {
            printf("Mutex: Priority recalculated -> %s "
                   "(%d -> %d)\n",
                   task->name,
                   old_priority,
                   highest_priority);

            Kernel_Trace(TRACE_PRIORITY_RESTORE, task);
        }
    }

    /*
     * Propagate priority changes through the mutex
     * dependency chain to the next owner.
     */
    if (task->waiting_mutex != NULL &&
        task->waiting_mutex->owner != NULL)
    {
        Kernel_Mutex_RecalculateTaskPriority(
            task->waiting_mutex->owner,
            visited,
            depth + 1);
    }
}

static void Kernel_Mutex_RecalculatePriorityInternal(
    KernelMutex* mutex)
{
    if (mutex == NULL || mutex->owner == NULL)
    {
        return;
    }

    TaskControlBlock* visited[MAX_TASKS];

    Kernel_Mutex_RecalculateTaskPriority(
        mutex->owner,
        visited,
        0);
}

void Kernel_Mutex_RecalculatePriority(KernelMutex* mutex)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    Kernel_Mutex_RecalculatePriorityInternal(mutex);

    Kernel_CriticalExit(state);
}

static void Kernel_Mutex_ApplyPriorityInheritanceInternal(
    KernelMutex* mutex,
    TaskControlBlock* waiter)
{
    if (mutex == NULL ||
        waiter == NULL)
    {
        return;
    }

    Kernel_Mutex_RecalculatePriorityInternal(mutex);
}

void Kernel_Mutex_ApplyPriorityInheritance(
    KernelMutex* mutex,
    TaskControlBlock* waiter)
{
    KernelCriticalState state = Kernel_CriticalEnter();

    Kernel_Mutex_ApplyPriorityInheritanceInternal(mutex, waiter);

    Kernel_CriticalExit(state);
}
