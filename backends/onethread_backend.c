#include "backend.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include "../queues/queue.h"
#include "../packet_meta.h"

/* With this backend, on every new connection we spawn only one thread.
 *
 * The thread first waits for new data to be avaiable, and as soon as it's
 * available it sends it out to all of the other sockets. This is slightly
 * more optimal than twothread_backend because there is no context switching
 * or queuing involved, and it involves half as many threads.
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

void *conn_thread(void*);
// Called every time a new connection is accepted
void backend_newfd(int fd) {
	// Add the connection to the fd list
	printf("Got connected on fd %d\n", fd);
	int idx = fd_idx;
	fd_idx += 1;
	connected_fds[idx].fd = fd;
	connected_fds[idx].q = queue_init(10);
	connected_fds[idx].open = true;
	// Spawn the connection handling thread
	pthread_t pid;
	pthread_create(&pid, NULL, &conn_thread, &connected_fds[idx]);
}

// Our packets are just 512 bytes of garbage (literally uninitialized memory)
// src_fd is the place that the source connection for a packet
struct packet {
	int length;
	int src_fd;
	void *data;
};

void *conn_thread(void *_fd_info) {
	struct fd_info *fd_info = _fd_info;
	int retval, sent_len;
	char buf[BUFLEN];
	// Initialize the parts of the packet that won't change
	struct packet packet;
	packet.length = sizeof(buf);
	do {
		// Wait for a new full packet to arrive. We don't need a sender thread or a queue this way
		// because we'll never need to send out a packet before we've recieved it, so we can
		// just recieve and immediately send
		retval = recv(fd_info->fd, buf, BUFLEN, MSG_WAITALL);
		if (retval >= 0) {
			printf("%d: Got packet %s\n", fd_info->fd, buf);
			// We built most of the packet structure at the start of the function, but
			// we need to add the rest
			packet.src_fd = fd_info->fd;
			packet.data = buf;
			// Send the packet out to every fd (including this fd)
			for (int i = 0; i < fd_idx; i++) {
				// Skip if the fd is closed
				if (!connected_fds[i].open)
					continue;

				// Send out the packet, handling partial sends
				sent_len = 0;
				while (sent_len < packet.length) {
					retval = send(connected_fds[i].fd, packet.data + sent_len, packet.length - sent_len, MSG_NOSIGNAL);
					if (retval < 0)
						break;
					sent_len += retval;
				}
			}
			printf("%d: Finished sending out packet %s\n", fd_info->fd, buf);
		}
	} while (retval >= 0);
	// If we can no longer send/recv on that fd, we can mark the fd as empty so threads
	// stop pushing to their queue
	fd_info->open = false;
	return NULL;
}
