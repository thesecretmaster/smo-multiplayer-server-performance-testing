#include "backend.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sysinfo.h>
#include "../queues/queue.h"
#include "../packet_meta.h"

/* This backend takes a different approch to the other two,
 * as soon as we start we launch one thread for each core in
 * the machine. However, we never spawn any other threads.
 *
 * These threads use the epoll interface to wait for new data on
 * ANY of the file descriptors that are connected. Whenever data
 * becomes readable, one of the threads will pick it up and then
 * read it, and send that data out to all the sockets
 */

struct fd_info {
	int fd;
	bool open;
	struct queue *q;
};

static struct fd_info connected_fds[1024];
static int fd_idx = 0;
static int epoll_fd;

void *threadpool_thread(void*);
void backend_setup(void) {
	for (int i = 0; i < sizeof(connected_fds) / sizeof(connected_fds[0]); i++) {
		connected_fds[i].fd = -1;
		connected_fds[i].q = NULL;
		connected_fds[i].open = false;
	}

	epoll_fd = epoll_create(1024);

	pthread_t pid;
	for (int i = 0; i < get_nprocs(); i++)
		pthread_create(&pid, NULL, &threadpool_thread, NULL);
}

void backend_newfd(int fd) {
	printf("Got connected on fd %d\n", fd);
	int idx = fd_idx;
	fd_idx += 1;
	connected_fds[idx].fd = fd;
	connected_fds[idx].q = queue_init(10);
	connected_fds[idx].open = true;

	struct epoll_event ep_event;
	ep_event.data.fd = fd;
	ep_event.events = EPOLLIN | EPOLLONESHOT;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ep_event);
}

struct packet {
	int length;
	int src_fd;
	void *data;
};

void *threadpool_thread(void *_v) {
	int retval, sent_len;
	char buf[BUFLEN];
	struct packet packet;
	packet.length = sizeof(buf);
	struct epoll_event ep_ev;
	printf("Thread starting\n");
	while (epoll_wait(epoll_fd, &ep_ev, 1, -1) >= 1) {
		retval = recv(ep_ev.data.fd, buf, BUFLEN, MSG_WAITALL);
		if (retval >= 0) {
			printf("%d: Got packet %s\n", ep_ev.data.fd, buf);
			for (int i = 0; i < fd_idx; i++) {
				if (!connected_fds[i].open)
					continue;
				packet.src_fd = ep_ev.data.fd;
				packet.data = buf;

				sent_len = 0;
				while (sent_len < packet.length) {
					retval = send(connected_fds[i].fd, packet.data + sent_len, packet.length - sent_len, MSG_NOSIGNAL);
					if (retval < 0)
						break;
					sent_len += retval;
				}
			}
			printf("%d: Finished sending out packet %s\n", ep_ev.data.fd, buf);
			ep_ev.events = EPOLLIN | EPOLLONESHOT;
			epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ep_ev.data.fd, &ep_ev);
		} else {
			for (int i = 0; i < fd_idx; i++)
				if (connected_fds[i].fd == ep_ev.data.fd)
					connected_fds[i].open = false;
			printf("%d removed from epoll list\n", ep_ev.data.fd);
		}
	}
	printf("Thread exiting\n");
	return NULL;
}
