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

// Keep track of a connection info and it's fd
struct fd_info {
	int fd;
	bool open;
	struct queue *q;
};

// All connected fds and an idx to indicate how far into the list we've gotten
// We don't handle overflow of this array because this is just a test example
static struct fd_info connected_fds[1024];
static int fd_idx = 0;

// Called at server startup
void backend_setup(void) {
	// Initialize the connected fd list
	for (int i = 0; i < sizeof(connected_fds) / sizeof(connected_fds[0]); i++) {
		connected_fds[i].fd = -1;
		connected_fds[i].q = NULL;
		connected_fds[i].open = false;
	}
}

void *sender_thread(void*);
void *reciever_thread(void*);
// Called every time a new connection is accepted
void backend_newfd(int fd) {
	// Add the connection to the fd list
	printf("Got connected on fd %d\n", fd);
	int idx = fd_idx;
	fd_idx += 1;
	connected_fds[idx].fd = fd;
	connected_fds[idx].q = queue_init(10);
	connected_fds[idx].open = true;
	// Spawn the sender and reciever threads
	pthread_t pid;
	pthread_create(&pid, NULL, &sender_thread, &connected_fds[idx]);
	pthread_create(&pid, NULL, &reciever_thread, &connected_fds[idx]);
}

// Our packets are just 512 bytes of garbage (literally uninitialized memory)
// src_fd is the place that the source connection for a packet
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
		// Dequeue a backet from this threads queue
		if (dq(fd_info->q, (void**)&packet)) {
			printf("(%d -> %d) Dequeued packet %s\n", packet->src_fd, fd_info->fd, packet->data);
			// Send out the packet, handling partial sends
			sent_len = 0;
			while (sent_len < packet->length) {
				retval = send(fd_info->fd, packet->data + sent_len, packet->length - sent_len, MSG_NOSIGNAL);
				if (retval < 0)
					break;
				sent_len += retval;
			}
			printf("(%d -> %d) Sent packet %s\n", packet->src_fd, fd_info->fd, packet->data);
			// These are allocated in the reciever thread, we don't need them after
			// they're popped off the queue, so we can free them
			free(packet->data);
			free(packet);
		}
	} while (retval >= 0);
	// If we can no longer send on that fd, we can mark the fd as empty so threads
	// stop pushing to their queue
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
		// Block until there's a full packet ready to be read
		retval = recv(fd_info->fd, buf, BUFLEN, MSG_WAITALL);
		if (retval >= 0) {
			// Send the packet out to every fd (including this fd)
			for (int i = 0; i < fd_idx; i++) {
				// Skip if the fd is closed
				if (!connected_fds[i].open)
					continue;
				// Set up the packet structure with the data
				packet = malloc(sizeof(struct packet));
				packet->length = sizeof(buf);
				packet->src_fd = fd_info->fd;
				data_buf = malloc(sizeof(buf));
				memcpy(data_buf, buf, sizeof(buf));
				packet->data = data_buf;
				// Enqueue the packet we've built
				nq(connected_fds[i].q, packet);
				printf("(%d -> %d) Enqueued packet %s\n", fd_info->fd, connected_fds[i].fd, buf);
			}
		}
	} while (retval >= 0);
	// If we can no longer recv on that fd, we can mark the fd as empty so threads
	// stop pushing to their queue
	fd_info->open = false;
	return NULL;
}
