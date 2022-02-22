import argparse
from os import listdir, system
import re

parser = argparse.ArgumentParser(description='Process some integers.')
subparsers = parser.add_subparsers(dest='backend')
parser.add_argument('--o', choices=["y", "n"], default="y")

backend_choices = list(map(lambda s: re.compile('(.+)_backend\.c').search(s).group(1), filter(lambda s: re.compile('.+_backend\.c').match(s), listdir("backends"))))
for backend in backend_choices:
    backend_parser = subparsers.add_parser(backend)

    needs_queue = True
    needs_auth = False

    if backend == "send_optimized":
        needs_queue = False
        needs_auth = True

    if needs_queue:
        queue_choices = list(map(lambda s: re.compile('(.*)\.c').search(s).group(1), filter(lambda s: re.compile('.*\.c').match(s), listdir("queues"))))
        queue_choices.remove("linked_list_seq_core")

        backend_parser.add_argument('--queue', required=True, choices=queue_choices)
    else:
        backend_parser.add_argument('--queue', default=False)
    backend_parser.add_argument('--auth', default=needs_auth)

args = parser.parse_args()

ostring = ""
if args.o == "y":
    ostring = " -O3 "

print(not not args.queue)
server_command_string = "clang -Wall -pthread -g -o server" + ostring + "server.c parse_args.c" + (" queues/" + args.queue + ".c " if args.queue else " ") + "backends/" + args.backend + "_backend.c"
print(server_command_string)
system(server_command_string)
client_command_string = "clang " + ("-DAUTH " if args.auth else "") + "-DCLIENT -g -pthread -Wall -o client" + ostring + "client.c parse_args.c"
print(client_command_string)
system(client_command_string)
