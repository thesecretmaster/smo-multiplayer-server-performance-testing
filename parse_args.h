struct server_args;
struct server_args *parse_args(int argc, char *argv[]);
int getport(struct server_args*);
char *gethost(struct server_args*);
