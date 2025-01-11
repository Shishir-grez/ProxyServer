## Note
* Currently runs only on unix based systems.
* use "make all" to compile all the files into executable
* run the code by running command ./proxy <port_no>
* You can test the websites mentioned in websites.txt to check the working of caching .
* Uses Various concepts like linked lists for LRU ( Least Recently Used ) Caching, Multithreading for handling multiple request, uses semaphores and mutexes for ensuring errorless concurrent access to memory resource, use socket programming to establish connection between client and proxy server.
