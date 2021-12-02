#include "linked_list_seq_core.c"

/* An unsafe queue, just a doubly linked list.
 * If you use this, you'll see random segfaults
 * as the implimentation does not do anything to
 * protect against them
 */

bool nq(struct queue *q, void *data) {
	return nq_seq(q, data);
}
bool dq(struct queue *q, void **data) {
	return dq_seq(q, data);
}
