#ifndef MUTEX_H
#define MUTEX_H

#include "task.h"

#define MUTEX_MAX_WAITERS 10

typedef struct KernelMutex
{
    int locked;
    TaskControlBlock* owner;
    KernelMutex* next_owned;

    TaskControlBlock* waiters[MUTEX_MAX_WAITERS];
    int waiter_count;

} KernelMutex;

void Kernel_Mutex_Init(KernelMutex* mutex);

int Kernel_Mutex_Lock(KernelMutex* mutex,
                      TaskControlBlock* task);

int Kernel_Mutex_Unlock(KernelMutex* mutex,
                        TaskControlBlock* task);

int Kernel_Mutex_IsLocked(const KernelMutex* mutex);

TaskControlBlock* Kernel_Mutex_GetOwner(
    const KernelMutex* mutex);

int Kernel_Mutex_IsOwner(
    const KernelMutex* mutex,
    const TaskControlBlock* task);

void Kernel_Mutex_ApplyPriorityInheritance(
    KernelMutex* mutex,
    TaskControlBlock* waiter);

void Kernel_Mutex_RecalculatePriority(
    KernelMutex* mutex);

int Kernel_Mutex_AbortWait(
    KernelMutex* mutex,
    TaskControlBlock* task);

int Kernel_Mutex_CleanupTask(TaskControlBlock* task);

int Kernel_Mutex_ValidateState(
    const KernelMutex* mutex);

int Kernel_Mutex_RemoveWaiter(
    KernelMutex* mutex,
    TaskControlBlock* task);

#endif
