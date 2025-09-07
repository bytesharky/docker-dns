#ifndef QUEUE_H
#define QUEUE_H
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define BUF_SIZE 4096
#define QUEUE_SIZE 1024

typedef struct {
    uint8_t data[BUF_SIZE];
    size_t len;
    struct sockaddr_in client_addr;
    socklen_t client_len;
} dns_request_t;

extern dns_request_t queue[];
extern int q_head, q_tail;
extern pthread_mutex_t q_mutex;
extern pthread_cond_t q_cond;

void enqueue_request(dns_request_t *req);
int dequeue_request(dns_request_t *req);
#endif
