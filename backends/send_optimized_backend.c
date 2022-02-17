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
#include <fcntl.h>
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
#define debug_print(...) printf(__VA_ARGS__);
#else
#define debug_print(fmt, ...)
#endif

#define BUF_CNT 2
#define MAX_CLIENT_COUNT 1024

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

struct send_start {
	int fd;
	volatile bool send_in_progress;
	int last_seen_list[MAX_CLIENT_COUNT];
};

struct send_progress {
	int original_fd; // For closing
	volatile bool *send_in_progress; // For signaling send is done
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
	struct send_start *send_init;
};

struct room;
struct epoll_event_data {
	int fd;
	volatile struct room *room;
	enum event_type type;
	union event_data data;
};

enum EP_EV_RETVAL {
	EP_EV_REARM,
	EP_EV_DONT_REARM
};

// Keep track of a connection and it's fd
struct fd_info {
	int fd;
	bool open;
	volatile struct client_lastread *lastread_data;
};

struct room {
	int client_count;
	int client_idx;
	struct fd_info clients[MAX_CLIENT_COUNT];
};

#define MAX_ROOM_COUNT 1024
static volatile struct room * volatile roomlist[MAX_ROOM_COUNT];
// Global storage for the epoll struct. We could also pass this into the threads
// but I'm lazy.
static int epoll_fd;

void *threadpool_thread(void*);
// Called at server startup
void backend_setup(void) {
	for (int i = 0; i < MAX_ROOM_COUNT; i++) {
		roomlist[i] = NULL;
	}

	// Setup the epoll instance, size hint of 1024 because that's the connecte fd size
	// However, that number is ignored so it doesn't really matter :P
	epoll_fd = epoll_create(1024);

	// Spawn all of the threads for the threadpool, one for each CPU core
	pthread_t pid;
	for (int i = 0; i < get_nprocs(); i++)
		pthread_create(&pid, NULL, &threadpool_thread, NULL);
}

static struct room *room_init() {
	struct room *room = malloc(sizeof(struct room));
	room->client_count = 0;
	room->client_idx = 0;
	for (int i = 0; i < sizeof(room->clients) / sizeof(room->clients[0]); i++) {
		room->clients[i].fd = -1;
		room->clients[i].open = false;
		room->clients[i].lastread_data = NULL;
	}
	return room;
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
	// Authenticate this new fd
	char auth_buf[4];
	auth_buf[sizeof(auth_buf) - 1] = '\0';
	recv(fd, auth_buf, sizeof(auth_buf) - 1, MSG_WAITALL);
	long room_idx = strtol(auth_buf, NULL, 10);
	if (room_idx >= sizeof(roomlist)) {
		// Can't have a room index past the end of the list
		return;
	}
	volatile struct room *room;
	if (__atomic_load_n(&roomlist[room_idx], __ATOMIC_SEQ_CST) != NULL) {
		room = roomlist[room_idx];
	} else {
		volatile struct room *old_room = NULL;
		room = room_init();
		if (!__atomic_compare_exchange_n(&roomlist[room_idx], &old_room, room, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
			free((struct room*)room);
			room = old_room;
		}
	}
	printf("Authed to room %ld (%p)\n", room_idx, room);
	// Auth is complete! Hock up the fd!
	int idx = __atomic_fetch_add(&room->client_idx, 1, __ATOMIC_SEQ_CST);
	if (idx >= MAX_CLIENT_COUNT) {
		close(fd);
		printf("Could not add client, already too many clients\n");
		return;
	}
	debug_print("Sucessfully connected on fd %d\n", fd);
	int sndbuf_size = 1024 * (4 * 2);
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	room->clients[idx].fd = fd;
	room->clients[idx].lastread_data = lastread_init();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	room->clients[idx].open = true;


	struct epoll_event ep_event;
	struct epoll_event_data *data;
	// Add the fd to the epoll descriptor
	// We use EPOLLONESHOT for thread safe handling, so that each packet
	// will only wake up one thread
	data = malloc(sizeof(struct epoll_event_data));
	data->fd = fd;
	data->room = room;
	data->type = EPOLL_EV_READABLE;
	data->data.read_state = room->clients[idx].lastread_data;
	ep_event.data.ptr = data;
	ep_event.events = EPOLLIN | EPOLLONESHOT;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ep_event);

	// Start the send timer! We try to send once very 1/20 of a second
	int timer = timerfd_create(CLOCK_REALTIME, 0x0);
	const struct timespec send_rate = {
		.tv_sec = 0,
		.tv_nsec = 30000000 + (rand() % 100000)
	};
	const struct itimerspec ts = {
		.it_interval = send_rate,
		.it_value = send_rate
	};
	timerfd_settime(timer, 0x0, &ts, NULL);
	struct send_start *si = malloc(sizeof(struct send_start));
	si->fd = fd;
	si->send_in_progress = false;
	memset(si->last_seen_list, 0, sizeof(si->last_seen_list));
	data = malloc(sizeof(struct epoll_event_data));
	data->fd = timer;
	data->room = room;
	data->type = EPOLL_EV_SEND_START;
	data->data.send_init = si;
	ep_event.data.ptr = data;
	ep_event.events = EPOLLIN | EPOLLONESHOT;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer, &ep_event);
	debug_print("Got conn timer: %d, sock: %d\n", timer, fd);
	__atomic_fetch_add(&room->client_count, 1, __ATOMIC_SEQ_CST);
}

// Our packets are just 512 bytes of garbage (literally uninitialized memory)
// src_fd is the place that the source connection for a packet
struct packet {
	int length;
	int src_fd;
	void *data;
};

static void close_sock(int fd, volatile struct room *room, bool is_sock) {
	// If we couldn't read, we mark it as closed. We need to search for the correct
	// element because we don't know the index
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	if (close(fd) != 0) {
		printf("Close error %d\n", errno);
	}
	if (is_sock)
		for (int i = 0; i < room->client_idx; i++)
			if (room->clients[i].fd == fd)
				room->clients[i].open = false;
	debug_print("%d removed from epoll list\n", fd);
}

static enum EP_EV_RETVAL start_send(int send_fd, volatile struct room *room, struct send_start *si) {
	// If there's already a partial send pending, we're gonna let it finish before
	// we send it more data
	unsigned long expire_cnt;
	read(send_fd, &expire_cnt, 8);
	if (__atomic_load_n(&si->send_in_progress, __ATOMIC_SEQ_CST)) {
		return EP_EV_REARM;
	}
	int visited_len = room->client_count;
	int visited[visited_len];
	memset(visited, 0, sizeof(visited));
	int visited_idx = 0;
	int client_idx = 0;
	int spins = 0;
	int maxspins = room->client_idx * 4;
	char *send_buf = malloc(sizeof(char) * (visited_len * BUFLEN));
	while (visited_idx < visited_len) {
		// Failsafe in case we're just getting absolutely fucked, we can't stall too long
		spins += 1;
		if (spins >= maxspins) {
			break;
		}
		// Skip closed sockets
		if (!room->clients[client_idx].open) {
			client_idx = (client_idx + 1) % room->client_idx;
			continue;
		}
		// Skip sockets that have already been read ("visited")
		// Could go from O(n) to O(1) with bitmap
		bool is_visited = false;
		for (int i = 0; i < visited_idx; i++) {
			if (client_idx == visited[i]) {
				is_visited = true;
				break;
			}
		}
		if (is_visited) {
			client_idx = (client_idx + 1) % room->client_idx;
			continue;
		}
		// Ok, we haven't seen it before!
		// Get the highest version so we can read from it
		int high_vers_idx;
		int high_vers = -1;
		for (int i = 0; i < BUF_CNT; i++) {
			if (room->clients[client_idx].lastread_data->buffers[i].version > high_vers) {
				high_vers_idx = i;
				high_vers = room->clients[client_idx].lastread_data->buffers[i].version;
			}
		}
		// If we've seen this version before, don't send it again
		if (si->last_seen_list[client_idx] >= high_vers) {
			continue;
		}
		// This is actual real new data! We can reset the spin counter for this one, because it's ok to spin on failure.
		spins = 0;
		// Attempt to read data
		__atomic_fetch_add(&room->clients[client_idx].lastread_data->buffers[high_vers_idx].reader_count, 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&room->clients[client_idx].lastread_data->buffers[high_vers_idx].write_in_progress, __ATOMIC_SEQ_CST)) {
			// Fail. Don't add to visited so that we try again.
			__atomic_fetch_sub(&room->clients[client_idx].lastread_data->buffers[high_vers_idx].reader_count, 1, __ATOMIC_SEQ_CST);
		} else {
			memcpy(send_buf + (BUFLEN * visited_idx), (char*)room->clients[client_idx].lastread_data->buffers[high_vers_idx].buffer, BUFLEN);
			__atomic_fetch_sub(&room->clients[client_idx].lastread_data->buffers[high_vers_idx].reader_count, 1, __ATOMIC_SEQ_CST);
			// Set visited to ensure we don't copy into the buffer again
			visited[visited_idx += 1] = client_idx;
			si->last_seen_list[client_idx] = high_vers;
		}
		client_idx = (client_idx + 1) % room->client_idx;
	}
	debug_print("Sending %d events on %d\n", visited_idx, send_fd);
	int sent_bytes = send(si->fd, send_buf, BUFLEN * visited_idx, MSG_NOSIGNAL);
	if (sent_bytes == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
		printf("Can't send on %d, closing (errno %d)\n", si->fd, errno);
		close_sock(si->fd, room, true);
		close_sock(send_fd, room, false);
		return EP_EV_DONT_REARM;
	} else if (sent_bytes != BUFLEN * visited_idx) {
		// Partial send time B-)
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			sent_bytes = 0;
		__atomic_store_n(&si->send_in_progress, true, __ATOMIC_SEQ_CST);
		struct send_progress *ss = malloc(sizeof(struct send_progress));
		ss->original_fd = si->fd;
		ss->send_fd = dup(si->fd); // Dup so we can reg it to epoll multiple times
		if (ss->send_fd < 0) {
			printf("Error duping %d (%d)\n", si->fd, errno);
		}
		ss->data = send_buf;
		ss->send_in_progress = &si->send_in_progress;
		ss->next_byte_idx = sent_bytes;
		ss->data_length = BUFLEN * visited_idx;
		printf("Partial send, %d / %d bytes sent (dupfd: %d)\n", sent_bytes, BUFLEN * visited_idx, ss->send_fd);
		debug_print("Duped %d to %d\n", si->fd, ss->send_fd);
		struct epoll_event ep_part_send_event;
		struct epoll_event_data *part_send_data = malloc(sizeof(struct epoll_event_data));
		part_send_data->fd = ss->send_fd;
		part_send_data->type = EPOLL_EV_SEND_CONTINUE;
		part_send_data->data.send_state = ss;
		ep_part_send_event.data.ptr = part_send_data;
		ep_part_send_event.events = EPOLLOUT | EPOLLONESHOT;
		if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, part_send_data->fd, &ep_part_send_event) != 0) {
			printf("Error adding partial send to epoll %d\n", errno);
		}
	} else {
		free(send_buf);
	}
	return EP_EV_REARM;
}

static enum EP_EV_RETVAL send_continue(volatile struct room *room, struct send_progress *ss) {
	debug_print("GOT SEND CONT %d of %d bytes\n", ss->send_fd, ss->data_length - ss->next_byte_idx);
	int sent_bytes = send(ss->send_fd, ss->data + ss->next_byte_idx, ss->data_length - ss->next_byte_idx, MSG_NOSIGNAL);
	if (sent_bytes == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
		__atomic_store_n(ss->send_in_progress, true, __ATOMIC_SEQ_CST);
		printf("Can't send on %d, closing\n", ss->send_fd);
		close_sock(ss->send_fd, room, false);
		close_sock(ss->original_fd, room, true);
		free(ss->data);
		free(ss);
		return EP_EV_DONT_REARM;
	} else {
		if (sent_bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			sent_bytes = 0;
		ss->next_byte_idx += sent_bytes;
		if (ss->next_byte_idx == ss->data_length) {
			__atomic_store_n(ss->send_in_progress, true, __ATOMIC_SEQ_CST);
			printf("Send complete on %d, closing\n", ss->send_fd);
			close_sock(ss->send_fd, room, false);
			free(ss->data);
			free(ss);
			return EP_EV_DONT_REARM;
		} else {
			printf("%d FD | %d - %d bytes left to send, re-adding (sb %d, errno %d)\n", ss->send_fd, ss->data_length, ss->next_byte_idx, sent_bytes, errno);
			return EP_EV_REARM;
		}
	}
}

static enum EP_EV_RETVAL recv_idk(int recv_fd, volatile struct room *room, volatile struct client_lastread *lr) {
	int low_vers_idx = -1;
	// Find lowest vers buf
	for (int i = 0; i < BUF_CNT; i++)
		if (lr->buffers[i].version < low_vers_idx || low_vers_idx == -1)
			low_vers_idx = i;

	// Mark write in progress pre-emptively
	__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, true, __ATOMIC_SEQ_CST);

	// Wait for reader count to drop to 0 (and try to recv into hotbuf in the meantime)
	int rcv_hotbuf_len = 0;
	int rcv_hotbuf_cap = BUFLEN - lr->write_progress;
	char rcv_hotbuf[BUFLEN];
	while (__atomic_load_n(&lr->buffers[low_vers_idx].reader_count, __ATOMIC_SEQ_CST) > 0) {
		if (rcv_hotbuf_len < rcv_hotbuf_cap) {
			int rcv_rv = recv(recv_fd, rcv_hotbuf + rcv_hotbuf_len, rcv_hotbuf_cap - rcv_hotbuf_len, 0x0);
			if (rcv_rv == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
				printf("Bad sock close\n");
				__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, false, __ATOMIC_SEQ_CST);
				close_sock(recv_fd, room, true);
				return EP_EV_DONT_REARM;
			} else if (rcv_rv == 0) {
				printf("Good sock close %d\n", recv_fd);
				__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, false, __ATOMIC_SEQ_CST);
				close_sock(recv_fd, room, true);
				return EP_EV_DONT_REARM;
			} else {
				rcv_hotbuf_len += rcv_rv;
			}
		}
	}
	// Readers at 0 now!
	// Either append the hotbuf stuff or give one more try and a recv
	if (rcv_hotbuf_len == 0) {
		int rcv_rv = recv(recv_fd, (char*)lr->buffers[low_vers_idx].buffer + lr->write_progress, BUFLEN - lr->write_progress, 0x0);
		if (rcv_rv == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
			printf("Bad sock close\n");
			close_sock(recv_fd, room, true);
			__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, false, __ATOMIC_SEQ_CST);
			return EP_EV_DONT_REARM;
		} else if (rcv_rv == 0) {
			printf("Good sock close 2 %d (%d)\n", recv_fd, BUFLEN - lr->write_progress);
			__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, false, __ATOMIC_SEQ_CST);
			close_sock(recv_fd, room, true);
			return EP_EV_DONT_REARM;
		} else {
			lr->write_progress += rcv_rv;
		}
	} else {
		memcpy((char*)lr->buffers[low_vers_idx].buffer + lr->write_progress, rcv_hotbuf, rcv_hotbuf_len);
		lr->write_progress += rcv_hotbuf_len;
	}

	if (lr->write_progress != BUFLEN) {
		debug_print("Recv progress %d on %d\n", lr->write_progress, recv_fd);
	} else {
		__atomic_store_n(&lr->write_progress, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&lr->buffers[low_vers_idx].write_in_progress, false, __ATOMIC_SEQ_CST);
		int next_vers = __atomic_add_fetch(&lr->version_ctr, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&lr->buffers[low_vers_idx].version, next_vers, __ATOMIC_SEQ_CST);
		debug_print("Recv complete on %d\n", recv_fd);
	}
	return EP_EV_REARM;
}

void *threadpool_thread(void *_v) {
	struct epoll_event ep_ev;
	struct epoll_event_data *ev_data;
	debug_print("Thread starting\n");
	// Wait for any FD in the epoll descriptor to become ready for reading
	while (epoll_wait(epoll_fd, &ep_ev, 1, -1) >= 1) {
		ev_data = ep_ev.data.ptr;
		switch (ev_data->type) {
			case EPOLL_EV_SEND_START : {
				switch (start_send(ev_data->fd, ev_data->room, ev_data->data.send_init)) {
					case EP_EV_REARM:
						ep_ev.events = EPOLLIN | EPOLLONESHOT;
						if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ev_data->fd, &ep_ev) != 0) {
							printf("Error adding timer fd back to epoll %d\n", errno);
						}
						break;
					case EP_EV_DONT_REARM: break;
				}
				break;
			}
			case EPOLL_EV_SEND_CONTINUE : {
				switch (send_continue(ev_data->room, ev_data->data.send_state)) {
					case EP_EV_REARM:
						ep_ev.events = EPOLLOUT | EPOLLONESHOT;
						if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ev_data->fd, &ep_ev) != 0) {
							printf("Error adding partial send back to epoll %d (%d)\n", errno, ev_data->fd);
						}
						break;
					case EP_EV_DONT_REARM:
						free(ev_data);
						break;
				}
				break;
			}
			case EPOLL_EV_READABLE : {
				switch (recv_idk(ev_data->fd, ev_data->room, ev_data->data.read_state)) {
					case EP_EV_REARM:
						ep_ev.events = EPOLLIN | EPOLLONESHOT;
						if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ev_data->fd, &ep_ev) != 0) {
							printf("Error adding recv back to epoll %d\n", errno);
						}
					case EP_EV_DONT_REARM: break;
				}
				break;
			}
		}
	}
	debug_print("Thread exiting\n");
	return NULL;
}
