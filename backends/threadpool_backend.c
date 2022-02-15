#include "backend.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sysinfo.h>
#include <unistd.h>
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
// Global storage for the epoll struct. We could also pass this into the threads
// but I'm lazy.
static int epoll_fd;

void *threadpool_thread(void*);
// Called at server startup
void backend_setup(void) {
	// Initialize the connected fd list
	for (int i = 0; i < sizeof(connected_fds) / sizeof(connected_fds[0]); i++) {
		connected_fds[i].fd = -1;
		connected_fds[i].q = NULL;
		connected_fds[i].open = false;
	}

	// Setup the epoll instance, size hint of 1024 because that's the connecte fd size
	// However, that number is ignored so it doesn't really matter :P
	epoll_fd = epoll_create(1024);

	// Spawn all of the threads for the threadpool, one for each CPU core
	pthread_t pid;
	for (int i = 0; i < /*get_nprocs()*/ 4; i++)
		pthread_create(&pid, NULL, &threadpool_thread, NULL);
}

// Called every time a new connection is accepted
void backend_newfd(int fd) {
	// Add the connection to the fd list
	printf("Got connected on fd %d\n", fd);
	int idx = fd_idx;
	fd_idx += 1;
	connected_fds[idx].fd = fd;
	connected_fds[idx].q = queue_init(10);
	connected_fds[idx].open = true;

	// Add the fd to the epoll descriptor
	// We use EPOLLONESHOT for thread safe handling, so that each packet
	// will only wake up one thread
	struct epoll_event ep_event;
	ep_event.data.fd = fd;
	ep_event.events = EPOLLIN | EPOLLONESHOT;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ep_event);
}

// Our packets are just 512 bytes of garbage (literally uninitialized memory)
// src_fd is the place that the source connection for a packet
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
	// Wait for any FD in the epoll descriptor to become ready for reading
	while (epoll_wait(epoll_fd, &ep_ev, 1, -1) >= 1) {
		// Read the packet from the descriptor
		retval = recv(ep_ev.data.fd, buf, BUFLEN, MSG_WAITALL);
		if (retval > 0) {
			printf("%d: Got packet %s\n", ep_ev.data.fd, buf);
			// Send the packet out to every socket that is currently connected
			for (int i = 0; i < fd_idx; i++) {
				// Skip if the connection is closed
				if (!connected_fds[i].open)
					continue;
				packet.src_fd = ep_ev.data.fd;
				packet.data = buf;

				// Handle partial threads
				sent_len = 0;
				while (sent_len < packet.length) {
					retval = send(connected_fds[i].fd, packet.data + sent_len, packet.length - sent_len, MSG_NOSIGNAL);
					if (retval < 0)
						break;
					sent_len += retval;
				}
			}
			printf("%d: Finished sending out packet %s\n", ep_ev.data.fd, buf);
			// Readd the fd to the epoll descriptor because EPOLLONESHOT requires it
			ep_ev.events = EPOLLIN | EPOLLONESHOT;
			epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ep_ev.data.fd, &ep_ev);
		} else {
			// If we couldn't read, we mark it as closed. We need to search for the correct
			// element because we don't know the index
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ep_ev.data.fd, NULL);
			close(ep_ev.data.fd);
			for (int i = 0; i < fd_idx; i++)
				if (connected_fds[i].fd == ep_ev.data.fd)
					connected_fds[i].open = false;
			printf("%d removed from epoll list\n", ep_ev.data.fd);
		}
	}
	printf("Thread exiting\n");
	return NULL;
}
