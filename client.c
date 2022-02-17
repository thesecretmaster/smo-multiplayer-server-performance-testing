#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "packet_meta.h"
#include <stdbool.h>
#include <unistd.h>
#include "parse_args.h"
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/time.h>

// We want to simulate a 60fps game submitting a packet every 3 frames
// This is in microseconds because that's what `usleep` takes
#define WAIT_TIME ((1000000 /* microseconds in 1 second */ / 60 /* fps */) * 3 /* frames between sends */)

static int thread_count;
static int *fds;

void *client_thread(void *_args) {
	struct server_args  *args = _args;
	int sockfd;
	struct sockaddr_in servaddr;
	int port = getport(args);
	char *host = gethost(args);

	// socket create and varification
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		printf("socket creation failed...\n");
		return NULL;
	}

	// assign IP, PORT
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(host);
	servaddr.sin_port = htons(port);

	// connect the client socket to server socket
	if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) != 0) {
		printf("connection with the server failed...\n");
		return NULL;
	}

	printf("Connected to %s on port %d\n", host, port);
	char auth_buf[4] = "1";
	int sent_count = 0;
	while (sent_count < 4) {
		int rv = send(sockfd, auth_buf, 4, 0);
		if (rv <= 0) {
			printf("Could not start socket\n");
			close(sockfd);
			return NULL;
		} else {
			sent_count += rv;
		}
	}
	printf("Sent auth packet (%s)\n", auth_buf);

	char buf[BUFLEN];
	int sent_len, retval;
	int i = 0;
	struct timeval last_send_tv, current_tv;
	char recv_buf[BUFLEN];
	bool saw_my_packet;
	int seen_pack_cnt;
	int last_pack_id = -1;
	pthread_t my_pid = pthread_self();
	pthread_t recv_pid;
	int recv_id;
	bool saw_anything;
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
	while (true) {
		sent_len = 0;
		// Send a packet with only a packet number string inside
		// printf("Sending packet %d\n", i);
		snprintf(buf, BUFLEN, "%lu %d", my_pid, i);
		while (sent_len < BUFLEN) {
			retval = send(sockfd, buf + sent_len, BUFLEN - sent_len, MSG_NOSIGNAL);
			if (retval < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					sched_yield();
				} else {
					printf("Send error %d\n", errno);
					return NULL;
				}
			} else {
				sent_len += retval;
			}
		}
		// printf("Done send\n");
		saw_my_packet = false;
		saw_anything = false;
		gettimeofday(&last_send_tv, NULL);
		seen_pack_cnt = 0;
		do {
			// printf("Recive packet\n");
			retval = recv(sockfd, recv_buf, sizeof(recv_buf), MSG_WAITALL);
			if (retval < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					sched_yield();
				} else {
					printf("Recieve error %d\n", errno);
					return NULL;
				}
			} else {
				saw_anything = true;
				sscanf(recv_buf, "%lu %d", &recv_pid, &recv_id);
				if (recv_pid == my_pid) {
					seen_pack_cnt += 1;
					saw_my_packet = true;
					if (last_pack_id != -1) {
						if (last_pack_id == recv_id) {
							// printf("Saw it twice\n");
						} else if (last_pack_id + 1 != recv_id) {
							printf("Sequence break %d -> %d\n", last_pack_id, recv_id);
						}
					}
					last_pack_id = recv_id;
				}
			}
			// printf("iDone Recive packet\n");
			gettimeofday(&current_tv, NULL);
		} while (1000000 * current_tv.tv_sec + current_tv.tv_usec < 1000000 * last_send_tv.tv_sec + last_send_tv.tv_usec + WAIT_TIME);
		if (!saw_my_packet && saw_anything)
			printf("Did not see my packet!\n");
		// if (seen_pack_cnt > 1)
		// 	printf("Saw %d packets\n", seen_pack_cnt);
		i += 1;
	}
	printf("Exiting\n");
	return NULL;
}

int main(int argc, char *argv[]) {
	struct server_args *args = parse_args(argc, argv);
	thread_count = getthread_count(args);
	pthread_t pids[thread_count];
	int _fds[thread_count];
	for (int i = 0; i < thread_count; i++)
		_fds[i] = -1;
	fds = _fds;

	printf("Launching %d threads\n", thread_count);
	for (int i = 0; i < thread_count; i++)
		pthread_create(&pids[i], NULL, &client_thread, (void*)args);
	for (int i = 0; i < thread_count; i++)
		pthread_join(pids[i], NULL);
	return 0;
}
