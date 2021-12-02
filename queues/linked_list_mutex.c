#include <pthread.h>
#define MUTEX
#include "linked_list_seq_core.c"

/* The same thing as linked_list.c, but wrapped in a mutex to ensure
 * thread safety. This ensures that only one thread can touch the queue
 * data structure at a time, preventing the random segfaults.
 *
 * This is the first "correct" implimentation, however this will bring
 * the CPU to 100% by busy-looping on an empty queue.
 */

bool nq(struct queue *q, void *data) {
	bool rv;
	pthread_mutex_lock(&q->mutex);
	rv = nq_seq(q, data);
	pthread_mutex_unlock(&q->mutex);
	return rv;
}
bool dq(struct queue *q, void **data) {
	bool rv;
	pthread_mutex_lock(&q->mutex);
	rv = dq_seq(q, data);
	pthread_mutex_unlock(&q->mutex);
	return rv;
}
