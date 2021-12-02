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

void *conn_thread(void*);
void backend_newfd(int fd) {
	printf("Got connected on fd %d\n", fd);
	int idx = fd_idx;
	fd_idx += 1;
	connected_fds[idx].fd = fd;
	connected_fds[idx].q = queue_init(10);
	connected_fds[idx].open = true;
	pthread_t pid;
	pthread_create(&pid, NULL, &conn_thread, &connected_fds[idx]);
}

struct packet {
	int length;
	int src_fd;
	void *data;
};

void *conn_thread(void *_fd_info) {
	struct fd_info *fd_info = _fd_info;
	int retval, sent_len;
	char buf[BUFLEN];
	struct packet packet;
	packet.length = sizeof(buf);
	do {
		retval = recv(fd_info->fd, buf, BUFLEN, MSG_WAITALL);
		if (retval >= 0) {
			printf("%d: Got packet %s\n", fd_info->fd, buf);
			for (int i = 0; i < fd_idx; i++) {
				if (!connected_fds[i].open)
					continue;
				packet.src_fd = fd_info->fd;
				packet.data = buf;

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
	fd_info->open = false;
	return NULL;
}
