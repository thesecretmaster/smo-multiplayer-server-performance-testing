#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "parse_args.h"

struct server_args {
	int port;
	char *host;
	int thread_count;
};

static struct server_args allargs;

// Parse options
// --port=1234 or -p 1234
// default is 8080
struct server_args *parse_args(int argc, char *argv[]) {
	int port = 8080;
	int thread_count = 1;
	char *host = "127.0.0.1";

	// Parse options
	// --port=1234 or -p 1234
	// default is 8080
	static struct option long_options[] = {
		{"port", optional_argument, 0, 'p'},
		{"host", optional_argument, 0, 'h'},
#ifdef CLIENT
		{"thread_count", optional_argument, 0, 't'},
#endif
	};
#ifdef CLIENT
#define ARG_STRING "t:p:h:"
#else
#define ARG_STRING "p:h:"
#endif
	int option_index = 0;
	while (true) {
		int c = getopt_long(argc, argv, ARG_STRING, long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
			case 't':
				thread_count = strtol(optarg, NULL, 10);
				break;
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
	allargs.thread_count = thread_count;
	return &allargs;
}
int getthread_count(struct server_args *server_args) {
	return server_args->thread_count;
}
int getport(struct server_args *server_args) {
	return server_args->port;
}
char *gethost(struct server_args *server_args) {
	return server_args->host;
}
