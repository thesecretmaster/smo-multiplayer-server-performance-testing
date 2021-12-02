#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "parse_args.h"

struct server_args {
	int port;
	char *host;
};

static struct server_args allargs;

// Parse options
// --port=1234 or -p 1234
// default is 8080
struct server_args *parse_args(int argc, char *argv[]) {
	int port = 8080;
	char *host = "127.0.0.1";

	// Parse options
	// --port=1234 or -p 1234
	// default is 8080
	static struct option long_options[] = {
		{"port", optional_argument, 0, 'p'},
		{"host", optional_argument, 0, 'h'}
	};
	int option_index = 0;
	while (true) {
		int c = getopt_long(argc, argv, "p:h:", long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
			case 'p':
				port = strtol(optarg, NULL, 10);
				break;
			case 'h':
				host = strdup(optarg);
				break;
			case '?':
				break;
			default:
				abort();
		}
	}
	allargs.port = port;
	allargs.host = host;
	return &allargs;
}
int getport(struct server_args *server_args) {
	return server_args->port;
}
char *gethost(struct server_args *server_args) {
	return server_args->host;
}
