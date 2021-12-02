#include <pthread.h>
#define MUTEX
#define CONDVAR
#include "linked_list_seq_core.c"

/* This implimentation uses both a mutex for safety and a condition variable
 * to ensure better CPU usage. If we fail to dequeue an element from the list,
 * instead of returning false and spinning on an empty queue, we wait on a condition
 * variable. This condition variable is signaled after every insert, so the next
 * time an insert is completed, the dequeue will be signalled and wake up.
 *
 * This is the best implimentation we can get with pthread primatives.
 */

bool nq(struct queue *q, void *data) {
	bool rv;
	pthread_mutex_lock(&q->mutex);
	rv = nq_seq(q, data);
	pthread_mutex_unlock(&q->mutex);
	pthread_cond_signal(&q->condvar);
	return rv;
}

bool dq(struct queue *q, void **data) {
	bool rv;
	do {
		pthread_mutex_lock(&q->mutex);
		rv = dq_seq(q, data);
		if (!rv) {
			pthread_cond_wait(&q->condvar, &q->mutex);
		}
		pthread_mutex_unlock(&q->mutex);
	} while (!rv);
	return true;
}
