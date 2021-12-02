struct server_args;
struct server_args *parse_args(int argc, char *argv[]);
int getport(struct server_args*);
#ifdef CLIENT
int getthread_count(struct server_args*);
#endif
char *gethost(struct server_args*);
