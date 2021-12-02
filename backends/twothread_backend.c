#include "backend.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include "../queues/queue.h"
#include "../packet_meta.h"

/* With this backend, on every new connection we spawn two threads,
 * the sender thread and the reciver thread.
 *
 * The reciever thread reads data from the socket and then pushes it
 * into the send queues for every other thread.
 * The send thread reads the send queue and send the data out.
 */

struct fd_info {
	int fd;
	bool open;
	struct queue *q;
};

static struct fd_info connected_fds[1024];
static int fd_idx = 0;

void backend_setup(void) {
	for (int i = 0; i < sizeof(connected_fds) / sizeof(connected_fds[0]); i++) {
		connected_fds[i].fd = -1;
		connected_fds[i].q = NULL;
		connected_fds[i].open = false;
	}
}

void *sender_thread(void*);
void *reciever_thread(void*);
void backend_newfd(int fd) {
	printf("Got connected on fd %d\n", fd);
	int idx = fd_idx;
	fd_idx += 1;
	connected_fds[idx].fd = fd;
	connected_fds[idx].q = queue_init(10);
	connected_fds[idx].open = true;
	pthread_t pid;
	pthread_create(&pid, NULL, &sender_thread, &connected_fds[idx]);
	pthread_create(&pid, NULL, &reciever_thread, &connected_fds[idx]);
}

struct packet {
	int length;
	int src_fd;
	void *data;
};

void *sender_thread(void *_fd_info) {
	struct fd_info *fd_info = _fd_info;
	struct packet *packet;
	int retval, sent_len;
	do {
		retval = 0;
		if (dq(fd_info->q, (void**)&packet)) {
			printf("(%d -> %d) Dequeued packet %s\n", packet->src_fd, fd_info->fd, packet->data);
			sent_len = 0;
			while (sent_len < packet->length) {
				retval = send(fd_info->fd, packet->data + sent_len, packet->length - sent_len, MSG_NOSIGNAL);
				if (retval < 0)
					break;
				sent_len += retval;
			}
			printf("(%d -> %d) Sent packet %s\n", packet->src_fd, fd_info->fd, packet->data);
			free(packet->data);
			free(packet);
		}
	} while (retval >= 0);
	fd_info->open = false;
	return NULL;
}

void *reciever_thread(void *_fd_info) {
	struct fd_info *fd_info = _fd_info;
	int retval;
	struct packet *packet;
	char buf[BUFLEN];
	char *data_buf;
	do {
		retval = recv(fd_info->fd, buf, BUFLEN, MSG_WAITALL);
		if (retval >= 0) {
			for (int i = 0; i < fd_idx; i++) {
				if (!connected_fds[i].open)
					continue;
				packet = malloc(sizeof(struct packet));
				packet->length = sizeof(buf);
				packet->src_fd = fd_info->fd;
				data_buf = malloc(sizeof(buf));
				memcpy(data_buf, buf, sizeof(buf));
				packet->data = data_buf;
				nq(connected_fds[i].q, packet);
				printf("(%d -> %d) Enqueued packet %s\n", fd_info->fd, connected_fds[i].fd, buf);
			}
		}
	} while (retval >= 0);
	fd_info->open = false;
	return NULL;
}
