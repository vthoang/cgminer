#include <pthread.h>
#include <stdint.h>

struct threadmsg {
	void *data;
	long msgtype;
	long qlength;
};

struct threadqueue {
	long length;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct msglist *first,*last;
	struct msglist *msgpool;
	long msgpool_length;
};

struct msglist {
    struct threadmsg msg;
    struct msglist *next;
};


void* memdup(const void* addr, size_t size);
void stuff_int32(unsigned char *dst, uint32_t x);
void stuff_reverse(unsigned char *dst, unsigned char *src, uint32_t len);
uint64_t bound(uint64_t value, uint64_t lower_bound, uint64_t upper_bound);
uint32_t bmcrc(unsigned char *ptr, uint32_t len);

int thread_queue_init(struct threadqueue *queue);
int thread_queue_add(struct threadqueue *queue, void *data, long msgtype);
int thread_queue_get(struct threadqueue *queue, const struct timespec *timeout, struct threadmsg *msg);
long thread_queue_length( struct threadqueue *queue );
int thread_queue_cleanup(struct threadqueue *queue, int freedata);
