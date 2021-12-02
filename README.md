## SMO Multiplayer Server Performance Comparison

I've thrown together a few different approaches to designing the multiplayer server, a few different
types of queues, and a few differnt threading models. The implimentations for the threading models
are in the `backends` folder and the implimentations for the queues are in the `queues` folder.

To compile and run the code, use `python setup.py --queue <queue> --backend <backend>`. To see the
list of backends and queues, run `python setup.py --help`. Try them out!

To run the server, compile it and then run `./server`. To run a simulated client, run `./client`.
You can run as many clients as you like. Both the server and client executables will log their
activity to STDOUT. Both the client and the server take port and host arguments via `-p 8080`
and `-h 127.0.0.1` (which are the defaults).
