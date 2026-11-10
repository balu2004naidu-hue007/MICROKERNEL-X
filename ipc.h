#ifndef IPC_H
#define IPC_H

#include <stddef.h>

#define IPC_QUEUE_SIZE 8
#define IPC_MESSAGE_SIZE 32

typedef struct
{
    char data[IPC_MESSAGE_SIZE];
} IPC_Message;

typedef struct
{
    IPC_Message messages[IPC_QUEUE_SIZE];

    int head;
    int tail;
    int count;

} IPC_MessageQueue;

void IPC_Init(IPC_MessageQueue* queue);

int IPC_Send(IPC_MessageQueue* queue, const char* message);

#include <stddef.h>

int IPC_Receive(
    IPC_MessageQueue* queue,
    char* buffer,
    size_t buffer_size);

int IPC_IsEmpty(IPC_MessageQueue* queue);

int IPC_IsFull(IPC_MessageQueue* queue);

int IPC_GetCount(IPC_MessageQueue* queue);

#endif
