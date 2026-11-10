#include <stdio.h>
#include <string.h>
#include "ipc.h"

void IPC_Init(IPC_MessageQueue* queue)
{
    if (queue == NULL)
    {
        return;
    }

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    for (int i = 0; i < IPC_QUEUE_SIZE; i++)
    {
        queue->messages[i].data[0] = '\0';
    }
}

int IPC_Send(IPC_MessageQueue* queue, const char* message)
{
    if (queue == NULL || message == NULL)
    {
        return -1;
    }

    if (IPC_IsFull(queue))
    {
        return -1;
    }

    strncpy(queue->messages[queue->tail].data,
            message,
            IPC_MESSAGE_SIZE - 1);

    queue->messages[queue->tail].data[IPC_MESSAGE_SIZE - 1] = '\0';

    queue->tail++;

    if (queue->tail >= IPC_QUEUE_SIZE)
    {
        queue->tail = 0;
    }

    queue->count++;

    return 0;
}

int IPC_Receive(
    IPC_MessageQueue* queue,
    char* buffer,
    size_t buffer_size)
{
    if (queue == NULL || buffer == NULL)
    {
        return -1;
    }

    if (IPC_IsEmpty(queue))
    {
        return -1;
    }

    size_t message_length =
        strlen(queue->messages[queue->head].data) + 1;

    if (buffer_size < message_length)
    {
        return -2;
    }

    memcpy(buffer,
           queue->messages[queue->head].data,
           message_length);

    queue->messages[queue->head].data[0] = '\0';

    queue->head++;

    if (queue->head >= IPC_QUEUE_SIZE)
    {
        queue->head = 0;
    }

    queue->count--;

    return 0;
}

int IPC_IsEmpty(IPC_MessageQueue* queue)
{
    if (queue == NULL)
    {
        return 1;
    }

    return queue->count == 0;
}

int IPC_IsFull(IPC_MessageQueue* queue)
{
    if (queue == NULL)
    {
        return 0;
    }

    return queue->count >= IPC_QUEUE_SIZE;
}

int IPC_GetCount(IPC_MessageQueue* queue)
{
    if (queue == NULL)
    {
        return 0;
    }

    return queue->count;
}
