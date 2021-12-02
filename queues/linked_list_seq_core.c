#include "queue.h"
#include <stdlib.h>

struct ll_node {
	void *data;
	struct ll_node *next;
	struct ll_node *prev;
};

struct queue {
	struct ll_node *head;
	struct ll_node *tail;
#ifdef MUTEX
	pthread_mutex_t mutex;
#endif
#ifdef CONDVAR
	pthread_cond_t condvar;
#endif
};

struct queue *queue_init(int _qlen) {
	struct queue *q = malloc(sizeof(struct queue));
	q->head = NULL;
	q->tail = NULL;
#ifdef MUTEX
	pthread_mutex_init(&q->mutex, NULL);
#endif
#ifdef CONDVAR
	pthread_cond_init(&q->condvar, NULL);
#endif
	return q;
};

void queue_deinit(struct queue *q) {
	struct ll_node *p = q->head;
	struct ll_node *n;
	if (p != NULL) {
		while (p->next != NULL) {
			n = p->next;
			free(p);
			p = n;
		}
	}
	free(q);
}

bool nq_seq(struct queue *q, void *data) {
	struct ll_node *node = malloc(sizeof(struct ll_node));
	node->data = data;

	if (q->head == NULL && q->tail == NULL) {
		node->next = NULL;
		node->prev = NULL;
		q->head = node;
		q->tail = node;
	} else {
		node->next = q->head;
		node->prev = NULL;
		q->head->prev = node;
		q->head = node;
	}
	return true;
}

bool dq_seq(struct queue *q, void **data) {
	if (q->head == NULL && q->tail == NULL) {
		return false;
	}

	struct ll_node *tail = q->tail;

	if (tail->prev == NULL) {
		q->tail = NULL;
		q->head = NULL;
	} else  {
		tail->prev->next = NULL;
		q->tail = tail->prev;
	}


	*data = tail->data;

	free(tail);
	return true;
}
