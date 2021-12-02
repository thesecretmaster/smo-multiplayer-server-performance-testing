#include <stdbool.h>

struct queue;

struct queue *queue_init(int qlen);
void queue_deinit(struct queue *q);
bool nq(struct queue *q, void *data);
bool dq(struct queue *q, void **data);
