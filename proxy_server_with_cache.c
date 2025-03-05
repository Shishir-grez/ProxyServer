#include <signal.h>
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
#include <time.h>

#define HASH_SIZE 100 // Max size for Hash Table

#define h_addr h_addr_list[0]			/* for backward compatibility */
#define MAX_BYTES 4096					// max allowed size of request/response
#define MAX_CLIENTS 400					// max number of client requests served at a time
#define MAX_SIZE 200 * (1 << 20)		// size of the cache 200MB
#define MAX_ELEMENT_SIZE 10 * (1 << 20) // max size of an element in cache 10 mb

typedef struct cache_element cache_element;

typedef struct Hashmap
{
	cache_element *table[HASH_SIZE];

} Hashmap;

typedef struct ThreadArgs
{
	Hashmap *map;
	int socket;

} ThreadArgs;
struct cache_element
{
	char *data;
	int len;
	char *url;
	struct cache_element *next;
	struct cache_element *left;
	struct cache_element *right;
};

int sendErrorMessage(int socket, int status_code);

int connectToRemoteServer(char *host_addr, int port_num);

int handle_request(Hashmap *map, int proxy_socket, ParsedRequest *request, char *storeHttpRequest);

int checkHTTPversion(char *msg);

void *thread_fn(void *args);

unsigned int hash(const char *key);

void initHashMap(Hashmap *map);

cache_element *createNode(char *data, int size, char *key);
void insert(Hashmap *map, char *data, int size, const char *key);

cache_element *search(Hashmap *map, const char *key);

void deleteNode(Hashmap *map);

void freeHashMap(Hashmap *map);

int port_number;
int proxy_socketId;			// socket descriptor of proxy server
pthread_t tid[MAX_CLIENTS]; // array to store the thread ids of clients
sem_t semaphore;			// if client requests exceeds the max_clients this  puts the
							// waiting threads to sleep and wakes them when traffic on queue decreases
sem_t cache_lock;
pthread_mutex_t lock; // lock is used for locking the cache

cache_element *qhead = NULL, *qtail = NULL; // Pointer to head and tail of queue
int cache_size;								// cache_size denotes the current size of the cache

// Global flag to indicate program termination
volatile sig_atomic_t stop_flag = 0;

// Signal handler for SIGINT (Ctrl+C)
void handle_sigint(int sig)
{
	stop_flag = 1;
	printf("\nReceived SIGINT. Cleaning up and exiting...\n");
	shutdown(proxy_socketId, SHUT_RDWR);
}

int main(int argc, char *argv[])
{
	// Register signal handler
	signal(SIGINT, handle_sigint);

	Hashmap map;
	initHashMap(&map);

	int client_socketId, client_len;
	struct sockaddr_in server_addr, client_addr;

	sem_init(&semaphore, 0, MAX_CLIENTS);
	pthread_mutex_init(&lock, NULL);

	if (argc == 2)
	{
		port_number = atoi(argv[1]);
	}
	else
	{
		printf("Too few arguments\n");
		exit(1);
	}

	printf("Setting Proxy Server Port : %d\n", port_number);

	// creating the proxy socket
	proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

	if (proxy_socketId < 0)
	{
		perror("Failed to create socket.\n");
		exit(1);
	}

	int reuse = 1;

	if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0)
	{
		perror("setsockopt(SO_REUSEADDR) failed\n");
		exit(1);
	}

	memset((char *)&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port_number);
	server_addr.sin_addr.s_addr = INADDR_ANY; // Any available adress assigned

	if (bind(proxy_socketId, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		perror("Port is not free\n");
		exit(1);
	}
	printf("Binding on port: %d\n", port_number);

	int listen_status = listen(proxy_socketId, MAX_CLIENTS);

	if (listen_status < 0)
	{
		perror("Error while Listening !\n");
		exit(1);
	}

	int i = 0;							 // Iterator for thread_id (tid) and Accepted Client_Socket for each thread
	int Connected_socketId[MAX_CLIENTS]; // This array stores socket descriptors of connected clients

	// Infinite Loop for accepting connections
	while (!stop_flag)
	{

		memset((char *)&client_addr, 0, sizeof(client_addr)); // Clears struct client_addr for next request
		client_len = sizeof(client_addr);

		client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);

		// Accept is a thread blocking call (unless specified to run in background), listens for any connection made from browser to the proxy socket

		// Immediately Switches to the thread spawned by most recent connection

		if (i >= MAX_CLIENTS)
		{
			fprintf(stderr, "Max clients reached. Rejecting connection.\n");
			close(client_socketId);
			continue;
		}

		if (client_socketId < 0)
		{
			perror("Error in Accepting connection");
			continue; // Skip this connection but keep the server running
		}
		else
		{
			Connected_socketId[i] = client_socketId; // Storing accepted clients into array
		}

		// Getting IP address and port number of client

		struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr;
		struct in_addr ip_addr = client_pt->sin_addr;
		char str[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN: Default ip address size
		inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);

		if (inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN) == NULL)
		{
			perror("inet_ntop failed");
			continue;
		}

		ThreadArgs *args = (ThreadArgs *)malloc(sizeof(ThreadArgs));

		if (args == NULL)
		{
			fprintf(stderr, "Failed to allocate memory for thread arguments\n");
			exit(1);
		}

		args->map = &map;
		args->socket = Connected_socketId[i];

		printf("Client is connected with port number: %d and ip address: %s \n", ntohs(client_addr.sin_port), str);

		int ret = pthread_create(&tid[i], NULL, (void *(*)(void *))thread_fn, args); // Creating a thread for each connection

		if (ret != 0)
		{
			fprintf(stderr, "Failed to create thread: %s\n", strerror(ret));
			close(Connected_socketId[i]); // Close the socket if thread creation fails
			free(args);
			continue; // Avoid crashing the program
		}

		// After successfully creating the thread, detach it

		pthread_detach(tid[i]); // Detach thread to allow automatic cleanup after it finishes

		i++;
	}

	close(proxy_socketId);
	freeHashMap(&map);
	pthread_mutex_destroy(&lock);
	sem_destroy(&semaphore);
	printf("Cleanup complete. Exiting...\n");
	return 0;
}

int send_all(int sockfd, const void *buf, size_t len, int flags)
{
	size_t total = 0;
	ssize_t bytes_left = len;
	const char *ptr = (const char *)buf;

	while (total < len)
	{
		ssize_t n = send(sockfd, ptr + total, bytes_left, flags);
		if (n == -1)
		{
			// If error occurs, handle it
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				// Retry on non-blocking socket error
				continue;
			}
			perror("send");
			return -1; // Handle other send errors
		}
		total += n;
		bytes_left -= n;
	}
	return total;
}

int recv_all(int sockfd, void *buf, size_t len, int flags)
{
	size_t total = 0;
	ssize_t bytes_left = len;
	char *ptr = (char *)buf;

	while (total < len)
	{
		ssize_t n = recv(sockfd, ptr + total, bytes_left, flags);
		if (n == -1)
		{
			// If error occurs, handle it
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				// Retry on non-blocking socket error
				continue;
			}
			perror("recv");
			return -1; // Handle other recv errors
		}
		else if (n == 0)
		{
			// Connection closed by the peer
			break;
		}
		total += n;
		bytes_left -= n;
	}
	return total;
}

int sendErrorMessage(int socket, int status_code)
{
	char str[1024];
	char currentTime[50];
	time_t now = time(0);

	struct tm data = *gmtime(&now);
	strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

	switch (status_code)
	{
	case 400:
		snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: Proxy\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
		printf("400 Bad Request\n");
		send(socket, str, strlen(str), 0);
		break;

	case 403:
		snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: Proxy\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
		printf("403 Forbidden\n");
		send(socket, str, strlen(str), 0);
		break;

	case 404:
		snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: Proxy\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
		printf("404 Not Found\n");
		send(socket, str, strlen(str), 0);
		break;

	case 500:
		snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: Proxy\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
		printf("500 Internal Server Error\n");
		send(socket, str, strlen(str), 0);
		break;

	case 501:
		snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: Proxy\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
		printf("501 Not Implemented\n");
		send(socket, str, strlen(str), 0);
		break;

	case 505:
		snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: Proxy\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
		printf("505 HTTP Version Not Supported\n");
		send(socket, str, strlen(str), 0);
		break;

	default:
		return -1;
	}
	return 1;
}

int connectToRemoteServer(char *host_addr, int port_num)
{

	int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (remoteSocket < 0)
	{
		printf("Error in Creating Socket.\n");
		return -1;
	}
	// Get host by the name or ip address provided
	struct hostent *host = gethostbyname(host_addr);
	if (host == NULL)
	{
		fprintf(stderr, "No such host exists.\n");
		return -1;
	}

	struct sockaddr_in server_addr;

	memset((char *)&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port_num);

	memcpy((char *)&server_addr.sin_addr.s_addr, (char *)host->h_addr, host->h_length);

	// Connect to remote server, the server would have some code similar to accept for C that will be listening to clients connect request

	if (connect(remoteSocket, (struct sockaddr *)&server_addr, (socklen_t)sizeof(server_addr)) < 0)
	{
		fprintf(stderr, "Error in connecting !\n");
		return -1;
	}
	return remoteSocket;
}

int handle_request(Hashmap *map, int proxy_socket, ParsedRequest *request, char *storeHttpRequest)
{

	char *method_http_v_buf = (char *)malloc(sizeof(char) * MAX_BYTES);

	if (method_http_v_buf == NULL)
	{
		fprintf(stderr, "Failed to allocate memory for cache elements(Stores http request)\n");
		return 0;
	}
	// Copying http method into this buffer
	strcpy(method_http_v_buf, "GET ");
	strcat(method_http_v_buf, request->path);
	strcat(method_http_v_buf, " ");
	strcat(method_http_v_buf, request->version);
	strcat(method_http_v_buf, "\r\n");

	size_t len = strlen(method_http_v_buf);

	if (ParsedHeader_set(request, "Connection", "close") < 0)
	{
		printf("set header key not work\n");
	}

	if (ParsedHeader_get(request, "Host") == NULL)
	{
		if (ParsedHeader_set(request, "Host", request->host) < 0)
		{
			printf("Set \"Host\" header key not working\n");
		}
	}

	if (ParsedRequest_unparse_headers(request, method_http_v_buf + len, (size_t)MAX_BYTES - len) < 0)
	{
		printf("unparse failed\n");
		// return -1;				// If this happens Still try to send request without header
	}

	int server_port = 80; // Default Remote Server Port Http - 80
	if (request->port != NULL)
		server_port = atoi(request->port);

	int remote_socket = connectToRemoteServer(request->host, server_port);

	if (remote_socket < 0)
		return -1;

	int bytes_send = send_all(remote_socket, method_http_v_buf, strlen(method_http_v_buf), 0);

	// Example of data sent : GET /path HTTP/1.1\r\nHost: example.com\r\n\r\n

	memset(method_http_v_buf, 0, MAX_BYTES);

	// data recieved is the actual response

	bytes_send = recv_all(remote_socket, method_http_v_buf, MAX_BYTES - 1, 0);

	char *to_be_cached = (char *)malloc(sizeof(char) * MAX_BYTES);

	if (to_be_cached == NULL)
	{
		free(method_http_v_buf);
		method_http_v_buf = NULL;
		fprintf(stderr, "Failed to allocate memory for to_be_cached element\n");
		return 0;
	}

	int cache_size = MAX_BYTES;

	int cache_index = 0;

	// Storing entire request inside to_be_cached

	while (bytes_send > 0)
	{
		bytes_send = send_all(proxy_socket, method_http_v_buf, bytes_send, 0);

		for (int i = 0; i < (int)(bytes_send / sizeof(char)); i++)
		{
			to_be_cached[cache_index] = method_http_v_buf[i];
			cache_index++;
		}
		cache_size += MAX_BYTES;

		char *realloc_handler = (char *)realloc(to_be_cached, cache_size);

		if (realloc_handler == NULL)
		{
			free(method_http_v_buf);
			method_http_v_buf = NULL;
			free(to_be_cached);
			to_be_cached = NULL;
			fprintf(stderr, "Failed to reallocate memory for to_be_cached\n");
			return 0;
		}

		to_be_cached = realloc_handler;

		if (bytes_send < 0)
		{
			perror("Error in sending data to client socket.\n");
			break;
		}

		memset(method_http_v_buf, 0, MAX_BYTES);

		bytes_send = recv_all(remote_socket, method_http_v_buf, MAX_BYTES - 1, 0);
	}

	to_be_cached[cache_index] = '\0';
	free(method_http_v_buf);
	insert(map, to_be_cached, strlen(to_be_cached), storeHttpRequest);
	printf("Done\n");
	free(to_be_cached);
	method_http_v_buf = NULL;
	to_be_cached = NULL;
	close(remote_socket);
	return 0;
}

int checkHTTPversion(char *msg)
{
	int version = -1;

	if (strncmp(msg, "HTTP/1.1", 8) == 0)
	{
		version = 1;
	}
	else if (strncmp(msg, "HTTP/1.0", 8) == 0)
	{
		version = 1; // Handling this similar to version 1.1
	}
	else
		version = -1;

	return version;
}

void *thread_fn(void *arg)
{
	ThreadArgs *args = (ThreadArgs *)arg; // Extracting function arguments from args

	Hashmap *map = args->map;

	int socketNew = args->socket;

	sem_wait(&semaphore);
	int p;
	sem_getvalue(&semaphore, &p);
	printf("semaphore value:%d\n", p);

	int proxy_socket = socketNew; // Represents ProxyServer Socket
	int bytes_send_client, len;	  // Bytes Transferred

	char *temp_http_req = (char *)calloc(MAX_BYTES, sizeof(char)); // Creating temp_http_req of 4kb for client
	if (temp_http_req == NULL)
	{
		fprintf(stderr, "Failed to allocate memory for client request temp_http_req\n");
		return 0;
	}

	memset(temp_http_req, 0, MAX_BYTES);

	bytes_send_client = recv(proxy_socket, temp_http_req, MAX_BYTES, 0);

	// Proxy recieves client request here
	// "https://stackoverflow.com/questions/14926062/detecting-end-of-http-header-with-r-n-r-n"

	while (bytes_send_client > 0)
	{
		len = strlen(temp_http_req);
		// loop until u find "\r\n\r\n" in the temp_http_req
		if (strstr(temp_http_req, "\r\n\r\n") == NULL)
		{
			bytes_send_client = recv(proxy_socket, temp_http_req + len, MAX_BYTES - len, 0);
		}
		else
		{
			break;
		}
	}

	char *tempReq = (char *)malloc(strlen(temp_http_req) * sizeof(char) + 1);
	if (tempReq == NULL)
	{
		free(temp_http_req);
		temp_http_req = NULL;
		fprintf(stderr, "Failed to allocate memory for tempReq(stores the http request) element\n");
		return 0;
	}
	// tempReq, temp_http_req both store the http request sent by client
	for (int i = 0; i < (int)(strlen(temp_http_req)); i++)
	{
		tempReq[i] = temp_http_req[i];
	}

	// checking for the request in cache
	cache_element *temp = search(map, tempReq);

	if (temp != NULL)
	{
		// request found in cache, so sending the response to client from proxy's cache
		int size = temp->len / sizeof(char);
		int pos = 0;
		char response[MAX_BYTES];
		while (pos < size)
		{
			memset(response, 0, MAX_BYTES);
			for (int i = 0; i < MAX_BYTES; i++)
			{
				response[i] = temp->data[pos];
				pos++;
			}
			send(proxy_socket, response, MAX_BYTES, 0);
		}
		printf("Data retrived from the Cache\n\n");

		printf("%s\n\n", response);
	}

	else if (bytes_send_client > 0)
	{
		len = strlen(temp_http_req);
		// Parsing the request
		ParsedRequest *request = ParsedRequest_create();

		// ParsedRequest_parse returns 0 on success and -1 on failure.On success it stores parsed request in
		//  the request
		if (ParsedRequest_parse(request, temp_http_req, len) < 0)
		{
			printf("Parsing failed\n");
		}
		else
		{
			memset(temp_http_req, 0, MAX_BYTES);

			if (!strcmp(request->method, "GET"))
			{

				if (request->host && request->path && (checkHTTPversion(request->version) == 1))
				{
					bytes_send_client = handle_request(map, proxy_socket, request, tempReq); // Handle GET request
					if (bytes_send_client == -1)
					{
						sendErrorMessage(proxy_socket, 500);
					}
				}
				else
					sendErrorMessage(proxy_socket, 500); // 500 Internal Error
			}
			else
			{
				printf("This code doesn't support any method other than GET\n");
			}
		}
		// freeing up the request pointer
		ParsedRequest_destroy(request);
	}

	else if (bytes_send_client < 0)
	{
		perror("Error in receiving from client.\n");
	}
	else if (bytes_send_client == 0)
	{
		printf("Client disconnected!\n");
	}

	shutdown(proxy_socket, SHUT_RDWR); // Shutdown the socket while reusing resources next time
	close(proxy_socket);
	free(temp_http_req);
	temp_http_req = NULL;
	sem_post(&semaphore);

	sem_getvalue(&semaphore, &p);
	printf("Semaphore post value:%d\n", p);
	free(tempReq);
	tempReq = NULL;
	free(arg);
	arg = NULL;
	return NULL;
}

unsigned int hash(const char *key)
{
	// Polynomial hash function for strins ( URL )
	unsigned int hashValue = 0;
	while (*key)
	{
		hashValue = (hashValue * 31) + *key;
		key++;
	}
	return hashValue % HASH_SIZE;
}

void initHashMap(Hashmap *map)
{

	for (int i = 0; i < HASH_SIZE; i++)
	{
		map->table[i] = NULL;
	}
}

void insert(Hashmap *map, char *data, int size, const char *key)
{
	unsigned int index = hash(key);

	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Mutex Lock Acquired %d\n", temp_lock_val);

	cache_element *existingNode = map->table[index];

	// Check if the key already exists in the hashmap
	while (existingNode)
	{
		if (strcmp(existingNode->url, key) == 0)
		{
			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Already Present \nMutex Lock Aquired %d\n", temp_lock_val);
			return;
		}
		existingNode = existingNode->next;
	}

	// If not found, insert a new node
	int element_size = size + 1 + strlen(key); // Size of the new element which will be added to the cache
	if (element_size > MAX_ELEMENT_SIZE)
	{
		temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Mutex Lock Aquired %d\n", temp_lock_val);
		return;
	}
	else
	{
		while (cache_size + element_size > MAX_SIZE)
		{
			deleteNode(map);
		}
		cache_element *element = (cache_element *)malloc(sizeof(cache_element));

		if (element == NULL)
		{
			fprintf(stderr, "Failed to allocate memory for new cache element\n");

			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Mutex Lock Unlocked %d\n", temp_lock_val);

			return;
		}

		element->data = (char *)malloc(size + 1);

		if (element->data == NULL)
		{
			free(element);
			element = NULL;
			fprintf(stderr, "Failed to allocate memory for cache element's data \n");

			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Mutex Lock Unlocked %d\n", temp_lock_val);
			return;
		}

		strcpy(element->data, data);

		element->url = (char *)malloc(1 + (strlen(key) * sizeof(char)));

		if (element->url == NULL)
		{
			free(element->data);
			element->data = NULL;
			free(element);
			element = NULL;
			fprintf(stderr, "Failed to allocate memory for cache element's url \n");

			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Mutex Lock Unlocked %d\n", temp_lock_val);
			return;
		}

		strcpy(element->url, key);

		element->len = element_size;
		element->next = map->table[index];

		map->table[index] = element;
		cache_size += element_size;

		// Updating Queue
		// Insertion at empty queue

		if (qhead == NULL && qtail == NULL)
		{
			element->left = NULL;
			element->right = NULL;
			qhead = element;
			qtail = qhead;
		}
		else
		{
			// Insertion at qhead
			element->left = NULL;
			element->right = qhead;

			qhead->left = element;

			qhead = element;
		}

		temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Mutex Lock Aquired %d\n", temp_lock_val);

		return;
	}
	return;
}

cache_element *search(Hashmap *map, const char *key)
{

	unsigned int index = hash(key);

	cache_element *node = map->table[index];

	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Remove Cache Lock Acquired %d\n", temp_lock_val);

	while (node != NULL)
	{

		if (strcmp(node->url, key) == 0)
		{
			printf("\nurl found\n");
			// Updating the time_track

			// If the node is already the head of the queue no action is needed

			if (node != qhead)
			{
				// Remove the node from its current position in the queue
				if (node->left)
				{
					node->left->right = node->right;
				}
				if (node->right)
				{
					node->right->left = node->left;
				}

				if (node == qtail)
				{
					qtail = node->left;
				}

				// Insert at head in queue
				node->right = qhead;
				node->left = NULL;

				if (qhead)
				{
					qhead->left = node;
				}
				qhead = node;
			}
			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Mutex lock unlocked %d\n", temp_lock_val);

			return node;
		}
		node = node->next;
	}
	printf("\nurl not found\n");

	temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Mutex Lock Unlocked %d\n", temp_lock_val);
	return NULL;
}

void deleteNode(Hashmap *map)
{
	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Remove Cache Lock Acquired %d\n", temp_lock_val);

	if (qtail == NULL)
	{
		temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Mutex Lock Unlocked %d\n", temp_lock_val);
		return;
	}

	// Locate last element in queue using tail

	cache_element *toDelete = qtail;

	// if there is more than one element in the queue

	if (qtail->left)
	{
		qtail = qtail->left;
		qtail->right = NULL;
	}
	else
	{
		qhead = NULL;
		qtail = NULL;
	}

	unsigned int index = hash(toDelete->url);

	cache_element *node = map->table[index];
	cache_element *prevNode = NULL;

	while (node)
	{
		if (strcmp(toDelete->url, node->url) == 0)
		{
			if (prevNode)
			{
				prevNode->next = node->next;
			}
			else
			{
				map->table[index] = node->next;
			}

			// Decrement the size of cache
			cache_size -= node->len;

			free(node->url);
			free(node);

			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Mutex Lock Unlocked %d\n", temp_lock_val);

			return;
		}
		prevNode = node;
		node = node->next;
	}
	temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Mutex Lock Unlocked %d\n", temp_lock_val);
}

void freeHashMap(Hashmap *map)
{
	for (int i = 0; i < HASH_SIZE; i++)
	{
		cache_element *node = map->table[i];
		while (node)
		{
			cache_element *temp = node;
			node = node->next;
			free(temp->url);
			free(temp);
		}
	}
}

/*
	Common pattern in this code ,handling of malloc / realloc failure
	Done using checking if malloc / realloc returned NULL
	(Indicating Failure)
	If yes Free any memory allocated previously + exiting the code
 */