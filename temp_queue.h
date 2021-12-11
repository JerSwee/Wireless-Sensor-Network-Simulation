#include <stdio.h>
#include <stdlib.h>

typedef struct
{
    int x;
    int y;
} tuple;

typedef struct
{
    int rank;
    int temp;
} adj_node;

struct infraredQueue
{
    int front, rear, size;
    unsigned capacity;
    // int *array;
    struct Temp_entry *array;
};

struct Temp_entry
{
    time_t time;
    int temp;
    tuple coord;
};

struct infraredQueue *buildQueue(unsigned capacity)
{
    struct infraredQueue *queue = (struct infraredQueue *)malloc(
        sizeof(struct infraredQueue));
    queue->capacity = capacity;
    queue->front = queue->size = 0;

    queue->rear = capacity - 1;
    queue->array = malloc(queue->capacity * sizeof(struct Temp_entry));

    return queue;
}

int isFull(struct infraredQueue *queue)
{

    if (queue->size == queue->capacity)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

int isEmpty(struct infraredQueue *queue)
{
    if (queue->size == 0)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

void dequeue(struct infraredQueue *queue)
{

    struct Temp_entry item = queue->array[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    queue->size = queue->size - 1;
}

void enqueue(struct infraredQueue *queue, struct Temp_entry item, FILE *f)
{
    if (isFull(queue) == 1)
    {
        dequeue(queue);
    }
    queue->rear = (queue->rear + 1) % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
    fprintf(f, "(%d, %d) enqueued to queue with temp %d\n", item.coord.x, item.coord.y, item.temp);
}

// get front of queue
struct Temp_entry front(struct infraredQueue *queue)
{

    return queue->array[queue->front];
}

struct Temp_entry rear(struct infraredQueue *queue)
{

    return queue->array[queue->rear];
}