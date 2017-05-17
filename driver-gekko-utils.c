#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include "driver-gekko-utils.h"

#define MSGPOOL_SIZE 256

void* memdup(const void* addr, size_t size) {
	void* ret = malloc(size);
	if (addr != NULL)
		memcpy(ret, addr, size);
	return ret;
}

uint64_t bound(uint64_t value, uint64_t lower_bound, uint64_t upper_bound)
{
	if (value < lower_bound)
		return lower_bound;
	if (value > upper_bound)
		return upper_bound;
	return value;
}

void stuff_reverse(unsigned char *dst, unsigned char *src, uint32_t len)
{
	uint32_t i;
	for (i = 0; i < len; i++) {
		dst[i] = src[len - i - 1];
	}
}

void stuff_int32(unsigned char *dst, uint32_t x)
{
	dst[0] = (x >>  0) & 0xff;
	dst[1] = (x >>  8) & 0xff;
	dst[2] = (x >> 16) & 0xff;
	dst[3] = (x >> 24) & 0xff;
}

uint32_t bmcrc(unsigned char *ptr, uint32_t len)
{
	unsigned char c[5] = {1, 1, 1, 1, 1};
	uint32_t i, c1, ptr_idx = 0;

	for (i = 0; i < len; i++) {
		c1 = c[1];
		c[1] = c[0];
		c[0] = c[4] ^ ((ptr[ptr_idx] & (0x80 >> (i % 8))) ? 1 : 0);
		c[4] = c[3];
		c[3] = c[2];
		c[2] = c1 ^ c[0];

		if (((i + 1) % 8) == 0)
			ptr_idx++;
	}
	return (c[4] * 0x10) | (c[3] * 0x08) | (c[2] * 0x04) | (c[1] * 0x02) | (c[0] * 0x01);
}

static inline struct msglist *get_msglist(struct threadqueue *queue)
{
    struct msglist *tmp;

    if(queue->msgpool != NULL) {
        tmp = queue->msgpool;
        queue->msgpool = tmp->next;
        queue->msgpool_length--;
    } else {
        tmp = malloc(sizeof *tmp);
    }

    return tmp;
}

static inline void release_msglist(struct threadqueue *queue, struct msglist *node)
{

    if(queue->msgpool_length > ( queue->length/8 + MSGPOOL_SIZE)) {
        free(node);
    } else {
        node->msg.data = NULL;
        node->msg.msgtype = 0;
        node->next = queue->msgpool;
        queue->msgpool = node;
        queue->msgpool_length++;
    }
    if(queue->msgpool_length > (queue->length/4 + MSGPOOL_SIZE*10)) {
        struct msglist *tmp = queue->msgpool;
        queue->msgpool = tmp->next;
        free(tmp);
        queue->msgpool_length--;
    }
}

int thread_queue_init(struct threadqueue *queue)
{
    int ret = 0;
    if (queue == NULL) {
        return EINVAL;
    }
    memset(queue, 0, sizeof(struct threadqueue));
    ret = pthread_cond_init(&queue->cond, NULL);
    if (ret != 0) {
        return ret;
    }

    ret = pthread_mutex_init(&queue->mutex, NULL);
    if (ret != 0) {
        pthread_cond_destroy(&queue->cond);
        return ret;
    }

    return 0;

}

int thread_queue_add(struct threadqueue *queue, void *data, long msgtype)
{
    struct msglist *newmsg;
    pthread_mutex_lock(&queue->mutex);
    newmsg = get_msglist(queue);
    if (newmsg == NULL) {
        pthread_mutex_unlock(&queue->mutex);
        return ENOMEM;
    }
    newmsg->msg.data = data;
    newmsg->msg.msgtype = msgtype;

    newmsg->next = NULL;
    if (queue->last == NULL) {
        queue->last = newmsg;
        queue->first = newmsg;
    } else {
        queue->last->next = newmsg;
        queue->last = newmsg;
    }

        if(queue->length == 0)
                pthread_cond_broadcast(&queue->cond);
    queue->length++;
    pthread_mutex_unlock(&queue->mutex);

    return 0;

}

int thread_queue_get(struct threadqueue *queue, const struct timespec *timeout, struct threadmsg *msg)
{
    struct msglist *firstrec;
    int ret = 0;
    struct timespec abstimeout;

    if (queue == NULL || msg == NULL) {
        return EINVAL;
    }
    if (timeout) {
        struct timeval now;

        gettimeofday(&now, NULL);
        abstimeout.tv_sec = now.tv_sec + timeout->tv_sec;
        abstimeout.tv_nsec = (now.tv_usec * 1000) + timeout->tv_nsec;
        if (abstimeout.tv_nsec >= 1000000000) {
            abstimeout.tv_sec++;
            abstimeout.tv_nsec -= 1000000000;
        }
    }

    pthread_mutex_lock(&queue->mutex);

    while (queue->first == NULL && ret != ETIMEDOUT) {
        if (timeout) {
            ret = pthread_cond_timedwait(&queue->cond, &queue->mutex, &abstimeout);
        } else {
            pthread_cond_wait(&queue->cond, &queue->mutex);

        }
    }
    if (ret == ETIMEDOUT) {
        pthread_mutex_unlock(&queue->mutex);
        return ret;
    }

    firstrec = queue->first;
    queue->first = queue->first->next;
    queue->length--;

    if (queue->first == NULL) {
        queue->last = NULL;
        queue->length = 0;
    }


    msg->data = firstrec->msg.data;
    msg->msgtype = firstrec->msg.msgtype;
        msg->qlength = queue->length;

    release_msglist(queue,firstrec);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

int thread_queue_cleanup(struct threadqueue *queue, int freedata)
{
    struct msglist *rec;
    struct msglist *next;
    struct msglist *recs[2];
    int ret,i;
    if (queue == NULL) {
        return EINVAL;
    }

    pthread_mutex_lock(&queue->mutex);
    recs[0] = queue->first;
    recs[1] = queue->msgpool;
    for(i = 0; i < 2 ; i++) {
        rec = recs[i];
        while (rec) {
            next = rec->next;
            if (freedata) {
                free(rec->msg.data);
            }
            free(rec);
            rec = next;
        }
    }

    pthread_mutex_unlock(&queue->mutex);
    ret = pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);

    return ret;

}

long thread_queue_length(struct threadqueue *queue)
{
    long counter;
    pthread_mutex_lock(&queue->mutex);
    counter = queue->length;
    pthread_mutex_unlock(&queue->mutex);
    return counter;
}