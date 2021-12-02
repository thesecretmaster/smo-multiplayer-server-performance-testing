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

	char buf[BUFLEN];
	int sent_len, retval;
	int i = 0;
	struct timeval last_send_tv, current_tv;
	char recv_buf[BUFLEN];
	bool saw_my_packet;
	pthread_t my_pid = pthread_self();
	pthread_t recv_pid;
	int recv_id;
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
	while (true) {
		sent_len = 0;
		// Send a packet with only a packet number string inside
		// printf("Sending packet %d\n", i);
		snprintf(buf, BUFLEN, "%lu %d", my_pid, i);
		while (sent_len < BUFLEN) {
			retval = send(sockfd, buf + sent_len, BUFLEN - sent_len, 0x0);
			if (retval < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					sched_yield();
				} else  {
					printf("Send error\n");
					return NULL;
				}
			} else {
				sent_len += retval;
			}
		}
		// printf("Done send\n");
		if (retval < 0)
			break;
		saw_my_packet = false;
		gettimeofday(&last_send_tv, NULL);
		do {
			// printf("Recive packet\n");
			retval = recv(sockfd, recv_buf, sizeof(recv_buf), MSG_WAITALL);
			if (retval < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					sched_yield();
				} else {
					printf("Recieve error\n");
					return NULL;
				}
			} else {
				sscanf(recv_buf, "%lu %d", &recv_pid, &recv_id);
				if (recv_pid == my_pid)
					saw_my_packet = true;
			}
			// printf("iDone Recive packet\n");
			gettimeofday(&current_tv, NULL);
		} while (1000000 * current_tv.tv_sec + current_tv.tv_usec < 1000000 * last_send_tv.tv_sec + last_send_tv.tv_usec + WAIT_TIME);
		if (!saw_my_packet)
			printf("Did not see my packet!\n");
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
