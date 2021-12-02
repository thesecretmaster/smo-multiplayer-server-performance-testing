import argparse
from os import listdir, system
import re

parser = argparse.ArgumentParser(description='Process some integers.')
backend_choices = list(map(lambda s: re.compile('(.+)_backend\.c').search(s).group(1), filter(lambda s: re.compile('.+_backend\.c').match(s), listdir("backends"))))
parser.add_argument('--backend', required=True, choices=backend_choices)
queue_choices = list(map(lambda s: re.compile('(.*)\.c').search(s).group(1), filter(lambda s: re.compile('.*\.c').match(s), listdir("queues"))))
queue_choices.remove("linked_list_seq_core")
parser.add_argument('--queue', required=True, choices=queue_choices)
args = parser.parse_args()

server_command_string = "clang -Wall -pthread -o server -O3 server.c parse_args.c queues/" + args.queue + ".c backends/" + args.backend + "_backend.c"
print(server_command_string)
system(server_command_string)
client_command_string = "clang -Wall -o client -O3 client.c parse_args.c"
print(client_command_string)
system(client_command_string)
