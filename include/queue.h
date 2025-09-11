#ifndef QUEUE_H
#define QUEUE_H
#include <netinet/in.h>  // for sockaddr_in
#include <pthread.h>     // for pthread_cond_t, pthread_mutex_t
#include <stdint.h>      // for uint8_t
#include <sys/socket.h>  // for size_t, socklen_t

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
void dequeue_request(dns_request_t *req);
#endif
