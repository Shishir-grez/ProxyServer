#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

#define MAX_BYTES 4096    //max allowed size of request/response
#define MAX_CLIENTS 400     //max number of client requests served at a time
#define MAX_SIZE 200*(1<<20)     //size of the cache
#define MAX_ELEMENT_SIZE 10*(1<<20)     //max size of an element in cache

typedef struct cache_element cache_element;

struct cache_element{
    char* data;         //data stores response
    int len;          //length of data i.e.. sizeof(data)...
    char* url;        //url stores the request
	time_t lru_time_track;    //lru_time_track stores the latest time the element is  accesed
    cache_element* next;    //pointer to next element
};

cache_element* find(char* url);
int add_cache_element(char* data,int size,char* url);
void remove_cache_element();

int port_number = 8080;				// Default Port
int proxy_socketId;					// socket descriptor of proxy server
pthread_t tid[MAX_CLIENTS];         //array to store the thread ids of clients
sem_t seamaphore;