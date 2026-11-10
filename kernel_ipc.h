#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include "ipc.h"
#include "task.h"
#define IPC_MAX_WAITERS 4
#define IPC_DEFAULT_TIMEOUT_TICKS 3

void Kernel_IPC_Init(void);

int Kernel_IPC_Send(const char* message);

int Kernel_IPC_Receive(char* buffer, size_t buffer_size);

int Kernel_IPC_GetCount(void);

void Kernel_IPC_Wait(TaskControlBlock* task);

void Kernel_IPC_Wake(TaskControlBlock* task);

void Kernel_IPC_ClearWakeReason(TaskControlBlock* task);

int Kernel_IPC_IsEmpty(void);

void Kernel_IPC_RegisterReceiver(TaskControlBlock* task);

void Kernel_IPC_RemoveReceiver(TaskControlBlock* task);

void Kernel_IPC_CleanupTask(TaskControlBlock* task);

int Kernel_IPC_GetWaitingCount(void);

TaskControlBlock* Kernel_IPC_GetWaitingTask(void);

unsigned long Kernel_IPC_GetDefaultTimeout(void);

unsigned long Kernel_IPC_GetTaskTimeout(TaskControlBlock* task);

#endif
