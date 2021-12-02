#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "packet_meta.h"
#include <stdbool.h>
#include <unistd.h>
#include "parse_args.h"

// We want to simulate a 60fps game submitting a packet every 3 frames
// This is in microseconds because that's what `usleep` takes
#define WAIT_TIME ((1000000 /* microseconds in 1 second */ / 60 /* fps */) * 3 /* frames between sends */)

int main(int argc, char *argv[]) {
	int sockfd;
	struct sockaddr_in servaddr;
	struct server_args *args = parse_args(argc, argv);
	int port = getport(args);
	char *host = gethost(args);

	// socket create and varification
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		printf("socket creation failed...\n");
		return -1;
	}

	// assign IP, PORT
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(host);
	servaddr.sin_port = htons(port);

	// connect the client socket to server socket
	if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) != 0) {
		printf("connection with the server failed...\n");
		return -1;
	}

	printf("Connected to %s on port %d\n", host, port);

	char buf[BUFLEN];
	int sent_len, retval;
	int i = 0;
	while (true) {
		sent_len = 0;
		// Send a packet with only a packet number string inside
		printf("Sending packet %d\n", i);
		snprintf(buf, BUFLEN, "%d", i);
		while (sent_len < BUFLEN) {
			retval = send(sockfd, buf, BUFLEN, 0x0);
			if (retval < 0)
				break;
			sent_len += retval;
		}
		if (retval < 0)
			break;
		usleep(WAIT_TIME);
		i += 1;
	}
	return 0;
}
