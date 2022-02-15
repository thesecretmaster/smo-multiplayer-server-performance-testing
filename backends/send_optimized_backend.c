#include "backend.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/timerfd.h>
#include <errno.h>
#include "../packet_meta.h"

/* This backend is the most optimal of them all, in that it resolves the major scaling
 * issue with all the other backends: With the other backends, the number of `send`/`recv`
 * calls per second is 20*|clients| + 20*(|client|^2). This is because we recieve 20 times
 * per second and we send that data to every other client 20 times a second, giving us that
 * O(n^2) complexity with respect to socket operations.
 *
 * This backend takes an alternative approach to specifically the `send` calls that lowers
 * the complexity to roughly O(n). This trick is that we maintain a set of buffers that hold
 * the latest packet from each client. Then, once every 3/60 of a second, we use a different
 * thread to read in all those buffers, construct one mega-packet with data from all the
 * sockets, and send that packet all at once. This isn't a silver bullet, the complexity looks
 * more like 20*|clients| + 20*|client|*(|client|/C) where C depends on the OS TCP send buffer
 * (larger buffer = bigger C)
 *
 * I would normally try to summerize the code in a little more detail, but this code is quite
 * complex. The TL;DR is that we take the `epoll` approach from `threadpool_backend` and
 * expand upon it.
 */

#ifdef DEBUG
#define debug_print(...) \
            do { if (DEBUG) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define debug_print(fmt, ...)
#endif

#define BUF_CNT 2

struct lastread_buffer {
	int version;
	bool write_in_progress;
	int reader_count;
	char buffer[BUFLEN];
};

// Keep track of the latest data we have from a client
// The *_in_progress flags could probably get merged but like whatever
struct client_lastread {
	int write_progress;
	int version_ctr;
	struct lastread_buffer buffers[BUF_CNT];
};

struct send_progress {
	int send_fd;
	int read_fd_idx;
	int next_byte_idx;
	int data_length;
	char *data;
};

// Epoll management stuff
enum event_type {
	EPOLL_EV_SEND_START,
	EPOLL_EV_READABLE,
	EPOLL_EV_SEND_CONTINUE,
};

// We need read state and send state on wake, but for SEND_TIMER we just need the FD to send on
union event_data {
	volatile struct client_lastread *read_state;
	struct send_progress *send_state;
	int send_start_fd;
};

struct epoll_event_data {
	int fd;
	enum event_type type;
	union event_data data;
};

// Keep track of a connection and it's fd
struct fd_info {
	int fd;
	bool readready;
	bool open;
	volatile struct client_lastread *lastread_data;
};

// All connected fds and an idx to indicate how far into the list we've gotten
// We don't handle overflow of this array because this is just a test example
static volatile struct fd_info connected_fds[1024];
static volatile int fd_idx = 0;
static volatile int client_count = 0;
// Global storage for the epoll struct. We could also pass this into the threads
// but I'm lazy.
static int epoll_fd;

void *threadpool_thread(void*);
// Called at server startup
void backend_setup(void) {
	// Initialize the connected fd list
	for (int i = 0; i < sizeof(connected_fds) / sizeof(connected_fds[0]); i++) {
		connected_fds[i].fd = -1;
		connected_fds[i].open = false;
		connected_fds[i].readready = false;
		connected_fds[i].lastread_data = NULL;
	}

	// Setup the epoll instance, size hint of 1024 because that's the connecte fd size
	// However, that number is ignored so it doesn't really matter :P
	epoll_fd = epoll_create(1024);

	// Spawn all of the threads for the threadpool, one for each CPU core
	pthread_t pid;
	for (int i = 0; i < get_nprocs(); i++)
		pthread_create(&pid, NULL, &threadpool_thread, NULL);
}

static struct client_lastread *lastread_init() {
	struct client_lastread *d = malloc(sizeof(struct client_lastread));
	d->write_progress = 0;
	d->version_ctr = 0;
	for (int i = 0; i < BUF_CNT; i++) {
		d->buffers[i].version = 0;
		d->buffers[i].reader_count = 0;
		d->buffers[i].write_in_progress = false;
		memset(d->buffers[i].buffer, 0x0, sizeof(d->buffers[i].buffer));
	}
	return d;
}

// Called every time a new connection is accepted
void backend_newfd(int fd) {
	// Add the connection to the fd list
	debug_print("Got connected on fd %d\n", fd);
	int idx = fd_idx;
	fd_idx += 1;
	connected_fds[idx].fd = fd;
	connected_fds[idx].open = true;
	connected_fds[idx].lastread_data = lastread_init();


	struct epoll_event ep_event;
	struct epoll_event_data *data;
	// Add the fd to the epoll descriptor
	// We use EPOLLONESHOT for thread safe handling, so that each packet
	// will only wake up one thread
	data = malloc(sizeof(struct epoll_event_data));
	data->fd = fd;
	data->type = EPOLL_EV_READABLE;
	data->data.read_state = connected_fds[idx].lastread_data;
	ep_event.data.ptr = data;
	ep_event.events = EPOLLIN | EPOLLONESHOT;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ep_event);

	// Start the send timer! We try to send once very 1/20 of a second
	int timer = timerfd_create(CLOCK_REALTIME, 0x0);
	const struct timespec send_rate = {
		.tv_sec = 0,
		.tv_nsec = 50000000
	};
	const struct itimerspec ts = {
		.it_interval = send_rate,
		.it_value = send_rate
	};
	timerfd_settime(timer, 0x0, &ts, NULL);
	data = malloc(sizeof(struct epoll_event_data));
	data->fd = timer;
	data->type = EPOLL_EV_SEND_START;
	data->data.send_start_fd = fd;
	ep_event.data.ptr = data;
	ep_event.events = EPOLLIN | EPOLLONESHOT;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer, &ep_event);
	__atomic_fetch_add(&client_count, 1, __ATOMIC_SEQ_CST);
	debug_print("Got conn timer: %d, sock: %d\n", timer, fd);
}

// Our packets are just 512 bytes of garbage (literally uninitialized memory)
// src_fd is the place that the source connection for a packet
struct packet {
	int length;
	int src_fd;
	void *data;
};

static void close_sock(int fd, bool is_sock) {
	// If we couldn't read, we mark it as closed. We need to search for the correct
	// element because we don't know the index
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	close(fd);
	for (int i = 0; i < fd_idx; i++)
		if (connected_fds[i].fd == fd)
			connected_fds[i].open = false;
	debug_print("%d removed from epoll list\n", fd);
}

void *threadpool_thread(void *_v) {
	struct epoll_event ep_ev;
	struct epoll_event_data *ep_tracking_data_in;
	debug_print("Thread starting\n");
	// Wait for any FD in the epoll descriptor to become ready for reading
sock_close:
	while (epoll_wait(epoll_fd, &ep_ev, 1, -1) >= 1) {
		ep_tracking_data_in = ep_ev.data.ptr;
		switch (ep_tracking_data_in->type) {
			case EPOLL_EV_SEND_START : {
				// Read the timer lol
				unsigned long expire_cnt;
				read(ep_tracking_data_in->fd, &expire_cnt, 8);
				int visited_len = client_count;
				int visited[visited_len];
				memset(visited, 0, sizeof(visited));
				int visited_idx = 0;
				int client_idx = 0;
				int spins = 0;
				int maxspins = client_count * 4;
				char *send_buf = malloc(sizeof(char) * (visited_len * BUFLEN));
				while (visited_idx < visited_len) {
					spins += 1;
					// Failsafe in case we're just getting absolutely fucked, we can't stall too long
					if (spins >= maxspins) {
						break;
					}
					if (!connected_fds[client_idx].open) {
						client_idx = (client_idx + 1) % fd_idx;
						continue;
					}
					bool is_visited = false;
					for (int i = 0; i < visited_idx; i++) {
						if (client_idx == visited[i]) {
							is_visited = true;
							break;
						}
					}
					if (is_visited) {
						client_idx = (client_idx + 1) % fd_idx;
						continue;
					}
					spins = 0;
					int high_vers_idx;
					int high_vers = -1;
					for (int i = 0; i < BUF_CNT; i++) {
						if (connected_fds[client_idx].lastread_data->buffers[i].version > high_vers) {
							high_vers_idx = i;
							high_vers = connected_fds[client_idx].lastread_data->buffers[i].version;
						}
					}
					__atomic_fetch_add(&connected_fds[client_idx].lastread_data->buffers[high_vers_idx].reader_count, 1, __ATOMIC_SEQ_CST);
					if (__atomic_load_n(&connected_fds[client_idx].lastread_data->buffers[high_vers_idx].write_in_progress, __ATOMIC_SEQ_CST)) {
						__atomic_fetch_sub(&connected_fds[client_idx].lastread_data->buffers[high_vers_idx].reader_count, 1, __ATOMIC_SEQ_CST);
					} else {
						memcpy(send_buf + (BUFLEN * visited_idx), (char*)connected_fds[client_idx].lastread_data->buffers[high_vers_idx].buffer, BUFLEN);
						__atomic_fetch_sub(&connected_fds[client_idx].lastread_data->buffers[high_vers_idx].reader_count, 1, __ATOMIC_SEQ_CST);
						visited[visited_idx += 1] = client_idx;
					}
					client_idx = (client_idx + 1) % fd_idx;
				}
				debug_print("Sending %d events on %d\n", visited_idx, ep_tracking_data_in->fd);
				int sent_bytes = send(ep_tracking_data_in->data.send_start_fd, send_buf, BUFLEN * visited_idx, MSG_NOSIGNAL);
				if (sent_bytes == -1) {
					debug_print("Can't send on %d, closing\n", ep_tracking_data_in->data.send_start_fd);
					close_sock(ep_tracking_data_in->data.send_start_fd, true);
					close_sock(ep_tracking_data_in->fd, false);
					goto sock_close;
				} else {
					// Partial send time B-)
					struct send_progress *ss = malloc(sizeof(struct send_progress));
					ss->send_fd = dup(ep_tracking_data_in->data.send_start_fd); // Dup so we can reg it to epoll multiple times
					ss->data = send_buf;
					ss->next_byte_idx = sent_bytes;
					ss->data_length = BUFLEN * visited_idx;
					struct epoll_event ep_part_send_event;
					struct epoll_event_data *part_send_data = malloc(sizeof(struct epoll_event_data));
					part_send_data->fd = ss->send_fd;
					debug_print("Duped %d to %d\n", ep_tracking_data_in->data.send_start_fd, part_send_data->fd);
					part_send_data->type = EPOLL_EV_SEND_CONTINUE;
					part_send_data->data.send_state = ss;
					ep_part_send_event.data.ptr = part_send_data;
					ep_part_send_event.events = EPOLLOUT | EPOLLONESHOT;
					epoll_ctl(epoll_fd, EPOLL_CTL_ADD, part_send_data->fd, &ep_part_send_event);
				}
				ep_ev.events = EPOLLIN | EPOLLONESHOT;
				epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ep_tracking_data_in->fd, &ep_ev);
				break;
			}
			case EPOLL_EV_SEND_CONTINUE : {
				struct send_progress *ss = ep_tracking_data_in->data.send_state;
				debug_print("GOT SEND CONT %d\n", ss->send_fd);
				int sent_bytes = send(ss->send_fd, ss->data + ss->next_byte_idx, ss->data_length - ss->next_byte_idx, MSG_NOSIGNAL);
				if (sent_bytes == -1) {
					debug_print("Can't send on %d, closing\n", ss->send_fd);
					close_sock(ss->send_fd, true);
					goto sock_close;
				} else {
					ss->next_byte_idx += sent_bytes;
					if (ss->next_byte_idx == ss->data_length) {
						debug_print("Send complete on %d, closing\n", ss->send_fd);
						close_sock(ss->send_fd, false);
						free(ss->data);
						free(ss);
						goto sock_close;
					} else {
						ep_ev.events = EPOLLOUT | EPOLLONESHOT;
						epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ss->send_fd, &ep_ev);
					}
				}
				break;
			}
			case EPOLL_EV_READABLE : {
				// HANDLE THE PARTIAL READ CASE (I think this actually should work but idk)
				volatile struct client_lastread *lr = ep_tracking_data_in->data.read_state;
				int low_vers_idx = -1;
				int rcv_hotbuf_len = 0;
				char rcv_hotbuf[BUFLEN];
				for (int i = 0; i < BUF_CNT; i++)
					if (lr->buffers[i].version < low_vers_idx || low_vers_idx == -1)
						low_vers_idx = i;
				__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, true, __ATOMIC_SEQ_CST);
				while (__atomic_load_n(&lr->buffers[low_vers_idx].reader_count, __ATOMIC_SEQ_CST) > 0) {
					if (rcv_hotbuf_len < BUFLEN) {
						int rcv_rv = recv(ep_tracking_data_in->fd, rcv_hotbuf + rcv_hotbuf_len, BUFLEN - rcv_hotbuf_len, 0x0);
						if (rcv_rv == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
							debug_print("Bad sock close\n");
							close_sock(ep_tracking_data_in->fd, true);
							goto sock_close;
						} else if (rcv_rv == 0) {
							debug_print("Good sock close\n");
							close_sock(ep_tracking_data_in->fd, true);
							goto sock_close;
						} else {
							rcv_hotbuf_len += rcv_rv;
						}
					}
				}
				int rcv_rv;
				if (rcv_hotbuf_len == 0) {
					rcv_rv = recv(ep_tracking_data_in->fd, (char*)lr->buffers[low_vers_idx].buffer + lr->write_progress, BUFLEN - lr->write_progress, 0x0);
					if (rcv_rv == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
						debug_print("Bad sock close\n");
						close_sock(ep_tracking_data_in->fd, true);
						goto sock_close;
					} else if (rcv_rv == 0) {
						debug_print("Good sock close\n");
						close_sock(ep_tracking_data_in->fd, true);
						goto sock_close;
					}
				} else {
					memcpy((char*)lr->buffers[low_vers_idx].buffer, rcv_hotbuf, rcv_hotbuf_len);
					rcv_rv = rcv_hotbuf_len;
				}

				if (rcv_rv != BUFLEN) {
					lr->write_progress += rcv_rv;
					debug_print("Recv progress %d on %d\n", lr->write_progress, ep_tracking_data_in->fd);
				} else {
					__atomic_store_n(&lr->write_progress, 0, __ATOMIC_RELAXED);
					__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, false, __ATOMIC_SEQ_CST);
					int next_vers = __atomic_add_fetch(&lr->version_ctr, 1, __ATOMIC_RELAXED);
					__atomic_store_n(&lr->buffers[low_vers_idx].version, next_vers, __ATOMIC_SEQ_CST);
					debug_print("Recv complete on %d\n", ep_tracking_data_in->fd);
				}
				ep_ev.events = EPOLLIN | EPOLLONESHOT;
				epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ep_tracking_data_in->fd, &ep_ev);
				break;
			}
		}
	}
	debug_print("Thread exiting\n");
	return NULL;
}
