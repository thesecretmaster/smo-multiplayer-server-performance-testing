#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "backends/backend.h"
#include "parse_args.h"

int main(int argc, char *argv[]) {
	int sockfd, connfd;
	socklen_t len;
	struct sockaddr_in servaddr, cli;
	struct server_args *args = parse_args(argc, argv);
	int port = getport(args);
	char *host = gethost(args);

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		printf("socket creation failed...\n");
		exit(0);
	}

	// assign IP, PORT
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(host);
	servaddr.sin_port = htons(port);
	// Binding newly created socket to given IP and verification
	if ((bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr))) != 0) {
		printf("socket bind failed...\n");
		exit(0);
	}

	// Now server is ready to listen and verification
	if ((listen(sockfd, 5)) != 0) {
		printf("Listen failed...\n");
		exit(0);
	}
	len = sizeof(cli);
	printf("Listening on port %d\n", port);

	backend_setup();

	printf("Setup complete!\n");

	while (true) {
		// Accept the data packet from client and verification
		connfd = accept(sockfd, (struct sockaddr*)&cli, &len);
		backend_newfd(connfd);
	}
	return 0;
}
