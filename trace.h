#ifndef TRACE_H
#define TRACE_H

#include "task.h"

#define TRACE_BUFFER_SIZE 64

typedef enum
{
    TRACE_TASK_SCHEDULED,
    TRACE_CONTEXT_SWITCH,
    TRACE_TASK_PROGRESS,
    TRACE_TASK_FAULT,
    TRACE_RECOVERY_START,
    TRACE_RECOVERY_SUCCESS,
    TRACE_RECOVERY_FAILED,
    TRACE_TASK_ISOLATED,
    TRACE_IPC_SEND,
    TRACE_IPC_RECEIVE,
    TRACE_MUTEX_LOCK,
    TRACE_MUTEX_WAIT,
    TRACE_MUTEX_UNLOCK,
    TRACE_PRIORITY_INHERIT,
    TRACE_PRIORITY_RESTORE
} TraceEventType;

typedef struct
{
    unsigned long tick;
    TraceEventType type;
    const char* task_name;
} TraceEvent;

void Kernel_Trace_Init(void);

void Kernel_Trace(TraceEventType type,
                  TaskControlBlock* task);

void Kernel_Trace_Print(void);

/* Copy the newest retained events into destination in chronological order. */
unsigned int Kernel_Trace_Copy(TraceEvent* destination,
                               unsigned int capacity);

const char* Kernel_Trace_GetEventName(TraceEventType type);

#endif
