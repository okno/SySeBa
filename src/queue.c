#include "syseba_internal.h"

int syseba_queue_init(syseba_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
    if (syseba_mutex_init(&queue->mutex) != 0) {
        return -1;
    }
    if (syseba_cond_init(&queue->available) != 0) {
        syseba_mutex_destroy(&queue->mutex);
        return -1;
    }
    if (syseba_cond_init(&queue->completed) != 0) {
        syseba_cond_destroy(&queue->available);
        syseba_mutex_destroy(&queue->mutex);
        return -1;
    }
    return 0;
}

void syseba_queue_close(syseba_queue_t *queue)
{
    syseba_mutex_lock(&queue->mutex);
    queue->closed = true;
    syseba_cond_broadcast(&queue->available);
    syseba_cond_broadcast(&queue->completed);
    syseba_mutex_unlock(&queue->mutex);
}

void syseba_queue_destroy(syseba_queue_t *queue, void (*free_fn)(void *))
{
    syseba_queue_node_t *node;
    syseba_queue_node_t *next;

    syseba_queue_close(queue);
    syseba_mutex_lock(&queue->mutex);
    node = queue->head;
    while (node != NULL) {
        next = node->next;
        if (free_fn != NULL) {
            free_fn(node->data);
        }
        free(node);
        node = next;
    }
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    queue->unfinished = 0;
    syseba_mutex_unlock(&queue->mutex);
    syseba_cond_destroy(&queue->completed);
    syseba_cond_destroy(&queue->available);
    syseba_mutex_destroy(&queue->mutex);
}

int syseba_queue_push(syseba_queue_t *queue, void *data)
{
    syseba_queue_node_t *node = (syseba_queue_node_t *)calloc(1, sizeof(*node));
    if (node == NULL) {
        return -1;
    }
    node->data = data;

    syseba_mutex_lock(&queue->mutex);
    if (queue->closed) {
        syseba_mutex_unlock(&queue->mutex);
        free(node);
        return -1;
    }
    if (queue->tail == NULL) {
        queue->head = node;
    } else {
        queue->tail->next = node;
    }
    queue->tail = node;
    queue->size++;
    queue->unfinished++;
    syseba_cond_signal(&queue->available);
    syseba_mutex_unlock(&queue->mutex);
    return 0;
}

void *syseba_queue_pop(syseba_queue_t *queue, unsigned timeout_ms)
{
    syseba_queue_node_t *node;
    void *data;

    syseba_mutex_lock(&queue->mutex);
    while (queue->head == NULL && !queue->closed) {
        int result = timeout_ms == 0
                         ? syseba_cond_wait(&queue->available, &queue->mutex)
                         : syseba_cond_timedwait(&queue->available,
                                                &queue->mutex,
                                                timeout_ms);
        if (result != 0) {
            syseba_mutex_unlock(&queue->mutex);
            return NULL;
        }
    }
    if (queue->head == NULL) {
        syseba_mutex_unlock(&queue->mutex);
        return NULL;
    }

    node = queue->head;
    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    queue->size--;
    data = node->data;
    free(node);
    syseba_mutex_unlock(&queue->mutex);
    return data;
}

void syseba_queue_task_done(syseba_queue_t *queue)
{
    syseba_mutex_lock(&queue->mutex);
    if (queue->unfinished > 0) {
        queue->unfinished--;
    }
    if (queue->unfinished == 0) {
        syseba_cond_broadcast(&queue->completed);
    }
    syseba_mutex_unlock(&queue->mutex);
}

bool syseba_queue_wait_empty(syseba_queue_t *queue, unsigned timeout_ms)
{
    uint64_t deadline = syseba_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    bool empty;

    syseba_mutex_lock(&queue->mutex);
    while (queue->unfinished > 0 && !queue->closed) {
        uint64_t now = syseba_monotonic_ns();
        unsigned remaining;
        if (now >= deadline) {
            break;
        }
        remaining = (unsigned)((deadline - now) / 1000000ULL);
        if (remaining == 0) {
            remaining = 1;
        }
        if (syseba_cond_timedwait(&queue->completed, &queue->mutex, remaining) < 0) {
            break;
        }
    }
    empty = queue->unfinished == 0;
    syseba_mutex_unlock(&queue->mutex);
    return empty;
}

size_t syseba_queue_size(syseba_queue_t *queue)
{
    size_t size;
    syseba_mutex_lock(&queue->mutex);
    size = queue->size;
    syseba_mutex_unlock(&queue->mutex);
    return size;
}

bool syseba_queue_is_closed(syseba_queue_t *queue)
{
    bool closed;
    syseba_mutex_lock(&queue->mutex);
    closed = queue->closed;
    syseba_mutex_unlock(&queue->mutex);
    return closed;
}
