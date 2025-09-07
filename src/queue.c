#include <stdlib.h>
#include "queue.h"

dns_request_t queue[QUEUE_SIZE];
int q_head = 0, q_tail = 0;
pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;

// 加入任务队列
void enqueue_request(dns_request_t *req) {
    pthread_mutex_lock(&q_mutex);
    queue[q_tail] = *req;
    q_tail = (q_tail + 1) % QUEUE_SIZE;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_mutex);
}

// 从任务队列取出
int dequeue_request(dns_request_t *req) {
    pthread_mutex_lock(&q_mutex);
    while (q_head == q_tail)
        pthread_cond_wait(&q_cond, &q_mutex);
    *req = queue[q_head];
    q_head = (q_head + 1) % QUEUE_SIZE;
    pthread_mutex_unlock(&q_mutex);
    return 1;
}
