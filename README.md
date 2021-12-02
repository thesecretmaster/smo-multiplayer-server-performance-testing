## SMO Multiplayer Server Performance Comparison

I've thrown together a few different approaches to designing the multiplayer server, a few different
types of queues, and a few differnt threading models. The implimentations for the threading models
are in the `backends` folder and the implimentations for the queues are in the `queues` folder.

To compile and run the code, use `python setup.py --queue <queue> --backend <backend>`. To see the
list of backends and queues, run `python setup.py --help`. Try them out!
