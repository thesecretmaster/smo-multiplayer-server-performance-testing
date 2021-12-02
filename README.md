## SMO Multiplayer Server Performance Comparison

I've thrown together a few different approaches to designing the multiplayer server, a few different
types of queues, and a few different threading models. The implimentations for the threading models
are in the `backends` folder and the implimentations for the queues are in the `queues` folder.

To compile and run the code, use `python setup.py --queue <queue> --backend <backend>`. To see the
list of backends and queues, run `python setup.py --help`. Try them out!

To run the server, use the `python setup.py` script to compile it and then run `./server`. To run
a simulated client, run `./client`.  You can run as many clients as you like. Both the server and
client executables will log their activity to STDOUT. Both the client and the server take port and
host arguments via `-p 8080` and `-h 127.0.0.1` (which are the defaults), and the client takes an
additional `-t 64` thread count argument which tells it how many clients to simulate.

To benchmark, try the different backends with different numbers of clients (by passing different
thread counts to the client)! On my system, threadpool_backend handled up to around 64 concurrent
clients, while at that level twothread_backend was falling behind. If a client doesn't get the same
packet it send back within a 3 frame window, the script will tell you that, which reflects the server
being overloaded. If the clients don't output anything, you know the server is serving packets
within the 3 frame window!
