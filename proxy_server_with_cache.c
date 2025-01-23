/**
 * @file proxy_server.c
 * @brief Proxy server that listens for client connections and handles requests.
 *
 * This program sets up a proxy server that listens on a specified port, accepts
 * client connections, and processes the requests in separate threads. It uses a
 * hashmap to manage request data and synchronizes concurrent access using semaphores
 * and mutexes.
 *
 * @dependencies
 *   - `stdio.h`    : Standard input/output functions.
 *   - `stdlib.h`   : General-purpose utility functions like memory management.
 *   - `string.h`   : String manipulation functions.
 *   - `sys/types.h`: System-level types.
 *   - `sys/socket.h`: Socket programming functions.
 *   - `netinet/in.h`: Definitions for Internet addresses and protocols.
 *   - `netdb.h`    : Functions for host information.
 *   - `arpa/inet.h`: Functions for IP address manipulation.
 *   - `unistd.h`   : Unix standard library for file and process control.
 *   - `fcntl.h`    : File control operations.
 *   - `time.h`     : Functions for working with time.
 *   - `sys/wait.h` : Functions for handling child processes.
 *   - `errno.h`    : Error handling definitions.
 *   - `pthread.h`  : POSIX threads for multithreading.
 *   - `semaphore.h`: POSIX semaphores for synchronization.
 *	 - `proxy_parse.h` : Library to Parse HTTP requests. Supports only GET requests for now.
 */

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

/**
 * @struct Hashmap
 * @brief Represents a hash table structure for storing cached data.
 *
 * The `Hashmap` struct holds an array of pointers to `cache_element` structures. 
 * Each element in the array corresponds to a bucket in the hash table, and is used
 * to resolve collisions via linked lists. This structure is used to efficiently store 
 * and retrieve cached data based on URL keys.
 *
 * @field table An array of pointers to `cache_element` structures. Each entry in the 
 *              array represents a bucket in the hash table. 
 *              The size of the table is defined by `HASH_SIZE`.
 */

typedef struct Hashmap
{
	cache_element *table[HASH_SIZE]; 

} Hashmap;

/**
 * @struct ThreadArgs
 * @brief Arguments passed to each thread that handles a client request.
 *
 * This structure holds the arguments needed by each thread to process a client's request.
 * It contains a pointer to the shared `Hashmap` structure for accessing cached data
 * and the socket descriptor used for communication with the client.
 *
 * @field map Pointer to the shared `Hashmap` that stores cached data for the proxy server.
 * @field socket The client socket descriptor used for communication between the proxy server 
 *               and the client.
 */

typedef struct ThreadArgs
{
	Hashmap *map; 
	int socket;	  

} ThreadArgs;


/**
 * @struct cache_element
 * @brief Represents a single element in the cache.
 *
 * The `cache_element` structure stores the cached data for a particular URL. It includes 
 * information about the data (e.g., the actual cached response and its length), the URL 
 * associated with the cached data, and pointers to manage cache eviction using the Least 
 * Recently Used (LRU) cache replacement policy.
 *
 * @field data A pointer to the cached data (e.g., HTTP response) that is stored.
 *
 * @field len The length of the cached data. This typically holds the size of the `data` in bytes.
 *
 * @field url The URL associated with the cached data. This is the key used to store/retrieve 
 *             the cached response.
 *
 * @field lru_time_track The timestamp indicating the last time this element was accessed. 
 *                        Used for managing the LRU cache eviction policy.
 *
 * @field next Pointer to the next `cache_element` in the linked list. Used for handling 
 *             collisions in the hash table (separate chaining).
 *
 * @field left Pointer to the previous `cache_element` in the LRU queue.
 * @field right Pointer to the next `cache_element` in the LRU queue.
 */

struct cache_element
{
	char *data;					
	int len;					
	char *url;					
	time_t lru_time_track;		
	struct cache_element *next; 
	struct cache_element *left;
	struct cache_element *right; 
};

typedef struct cache_element cache_element;

int sendErrorMessage(int socket, int status_code);

int connectToProxy(char *host_addr, int port_num);

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
int proxy_socketId;							// socket descriptor of proxy server
pthread_t tid[MAX_CLIENTS]; 				// array to store the thread ids of clients
sem_t semaphore;							// if client requests exceeds the max_clients this  puts the
											// waiting threads to sleep and wakes them when traffic on queue decreases
sem_t cache_lock;
pthread_mutex_t lock;						// lock is used for locking the cache

cache_element *qhead = NULL, *qtail = NULL; // Pointer to head and tail of queue
int cache_size;								// cache_size denotes the current size of the cache

/**
 * @file proxy_server_with_cache.c
 * @brief The entry point of a multi-threaded proxy server.
 *
 * This function initializes the proxy server, creates the necessary sockets,
 * handles client connections, and spawns threads to process each client's request.
 * It uses a hashmap for caching, semaphores to limit the number of concurrent
 * clients, and mutexes for synchronization.
 *
 * @usage
 *   - Run the program with a port number as an argument: `./proxy_server 8080`.
 *   - The server listens on the provided port and accepts client connections.
 *
 * @param argc The number of arguments passed to the program.
 * @param argv The arguments passed to the program, where argv[1] contains the port number.
 * @return 0 on successful execution, exits on errors.
 */

int main(int argc, char *argv[])
{
	Hashmap map;
	initHashMap(&map);

	ThreadArgs *args = (ThreadArgs *)malloc(sizeof(ThreadArgs));

	if (args == NULL)
	{
		fprintf(stderr, "Failed to allocate memory for thread arguments\n");
		exit(1);
	}

	args->map = &map;

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
		perror("setsockopt(SO_REUSEADDR) failed\n");

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
	while (1)
	{

		memset((char *)&client_addr, 0, sizeof(client_addr)); // Clears struct client_addr for next request
		client_len = sizeof(client_addr);

		client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);

		// Accept is a thread blocking call (unless specified to run in background), listens for any connection made from browser to the proxy socket
		// Immediately Switches to the thread spawned by most recent connection

		if (client_socketId < 0)
		{
			fprintf(stderr, "Error in Accepting connection !\n");
			exit(1);
		}
		else
		{
			Connected_socketId[i] = client_socketId; // Storing accepted clients into array
		}

		args->socket = Connected_socketId[i];

		// Getting IP address and port number of client
		struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr;
		struct in_addr ip_addr = client_pt->sin_addr;
		char str[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN: Default ip address size
		inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
		printf("Client is connected with port number: %d and ip address: %s \n", ntohs(client_addr.sin_port), str);

		pthread_create(&tid[i], NULL, (void *(*)(void *))thread_fn, args); // Creating a thread for each client accepted
		i++;
	}

	// Waiting for all threads to finish before freeing 'args'
	for (int j = 0; j < i; j++)
	{
		pthread_join(tid[j], NULL);
	}
	free(args);
	args = NULL;
	close(proxy_socketId);
	freeHashMap(&map);
	return 0;
}

/**
 * @brief Sends an HTTP error message to the client based on the provided status code.
 *
 * This function constructs an HTTP error response message in HTML format for common
 * HTTP status codes (400, 403, 404, 500, 501, 505) and sends it to the client via
 * the specified socket. The response includes the status code, a descriptive message,
 * the current date and time, and relevant headers like `Content-Length`, `Content-Type`, 
 * and `Server`. The function also prints the status code to the console for logging purposes.
 *
 * @param socket The socket descriptor representing the connection with the client.
 * @param status_code The HTTP status code to be sent in the error message.
 *                    It should be one of the following: 400, 403, 404, 500, 501, or 505.
 *
 * @return Returns 1 on success if the error message was sent successfully.
 *         Returns -1 if an unsupported status code is provided.
 *
 * @details This function constructs a formatted HTML page with an appropriate message
 *          corresponding to the provided HTTP status code and sends the response using
 *          the `send()` system call. The `Date` header in the response reflects the
 *          current time at the moment the error message is generated.
 *
 * @example
 *   int client_socket = ...;  // A valid client socket descriptor.
 *   sendErrorMessage(client_socket, 404);  // Sends a "404 Not Found" error.
 */

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

/**
 * @brief Establishes a connection to a proxy server.
 *
 * This function creates a socket and attempts to connect to the specified proxy server
 * using its host address and port number. It resolves the provided host address to an
 * IP address using `gethostbyname()` and establishes a connection using the `connect()` system call.
 *
 * @param host_addr The hostname or IP address of the proxy server.
 * @param port_num The port number to connect to on the proxy server.
 *
 * @return Returns the client socket descriptor if the connection is successfully established.
 *         Returns -1 if an error occurs during socket creation, host resolution, or connection.
 *
 * @details This function handles creating a socket, resolving the proxy server's address using
 *          `gethostbyname()`, and then connecting to the server at the specified port. The socket
 *          descriptor is returned for further communication. If any error occurs (e.g., the host cannot
 *          be resolved or the connection cannot be made), an error message is printed and -1 is returned.
 *
 * @example
 *   int clientSocket = connectToProxy("proxy.example.com", 8080);
 *   if (clientSocket < 0) {
 *       // Handle error
 *   }
 */

int connectToProxy(char *host_addr, int port_num)
{

	int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (clientSocket < 0)
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

	// Connect to proxy server, Proxy server is listening to incoming requests in while loop has blocked the main thread
	// Make a connection which will spawn a new thread

	if (connect(clientSocket, (struct sockaddr *)&server_addr, (socklen_t)sizeof(server_addr)) < 0)
	{
		fprintf(stderr, "Error in connecting !\n");
		return -1;
	}
	return clientSocket;
}

/**
 * @brief Handles a client HTTP request by forwarding it to the proxy server and caching the response.
 *
 * This function builds an HTTP request string from a parsed request, connects to the appropriate proxy server,
 * sends the request, receives the response, and caches it in the provided `Hashmap`. The response is also
 * forwarded to the client.
 *
 * @param map A pointer to the `Hashmap` where the response should be cached.
 * @param proxy_socket The client socket descriptor used to forward the response back to the client.
 * @param request A pointer to the parsed HTTP request.
 * @param storeHttpRequest A string used as a key for caching the response in the hashmap.
 *
 * @return Returns 0 if the request is handled successfully. Returns a non-zero value (typically 0) if memory allocation
 *         or network communication fails during the process.
 *
 * @details This function performs the following steps:
 *          - Allocates memory to build the HTTP request.
 *          - Sets necessary headers (`Connection`, `Host`).
 *          - Constructs and sends the HTTP request to the proxy server.
 *          - Receives the server's response in chunks, caches it, and sends it to the client.
 *          - Frees allocated memory and closes the client socket.
 *
 * @example
 *   Hashmap *map = ...;  // A valid pointer to a hashmap.
 *   ParsedRequest request = ...;  // A parsed HTTP request.
 *   char *storeHttpRequest = "example_request_key";  // A key for caching.
 *   int proxy_socket = ...;  // A valid client socket to forward the response.
 *   int result = handle_request(map, proxy_socket, &request, storeHttpRequest);
 *   if (result == 0) {
 *       // Request handled successfully
 *   } else {
 *       // Handle error
 *   }
 */

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

	int client_socket = connectToProxy(request->host, server_port);

	if (client_socket < 0)
		return -1;

	int bytes_send = send(client_socket, method_http_v_buf, strlen(method_http_v_buf), 0);

	memset(method_http_v_buf, 0, MAX_BYTES);

	bytes_send = recv(client_socket, method_http_v_buf, MAX_BYTES - 1, 0);

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
		bytes_send = send(proxy_socket, method_http_v_buf, bytes_send, 0);

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

		bytes_send = recv(client_socket, method_http_v_buf, MAX_BYTES - 1, 0);
	}

	to_be_cached[cache_index] = '\0';
	free(method_http_v_buf);
	insert(map, to_be_cached, strlen(to_be_cached), storeHttpRequest);
	printf("Done\n");
	free(to_be_cached);
	method_http_v_buf = NULL;
	to_be_cached = NULL;
	close(client_socket);
	return 0;
}

/**
 * @brief Checks the version of an HTTP message.
 *
 * This function checks whether the provided HTTP message corresponds to a specific version (either HTTP/1.0 or HTTP/1.1).
 * It matches the beginning of the string to determine the version and returns an integer indicating the version.
 * HTTP/1.0 and HTTP/1.1 are handled similarly in this implementation.
 *
 * @param msg A string representing the HTTP message to check (usually the first line of the HTTP request).
 *
 * @return Returns:
 *         - 1 if the message corresponds to either HTTP/1.0 or HTTP/1.1.
 *         - -1 if the version is unsupported or unrecognized.
 *
 * @details This function assumes that the HTTP version appears at the beginning of the message.
 *          It supports HTTP/1.0 and HTTP/1.1 only. If the version is recognized, it returns 1, otherwise, it returns -1.
 *
 * @example
 *   char *message = "HTTP/1.1 200 OK";
 *   int version = checkHTTPversion(message);
 *   if (version == 1) {
 *       // Handle HTTP/1.0 or HTTP/1.1 request
 *   } else {
 *       // Handle unsupported version
 *   }
 */

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

/**
 * @brief Handles a client request in a separate thread by receiving, processing, and responding to it.
 *
 * This function is executed in a separate thread to handle incoming client requests in a proxy server.
 * It receives an HTTP request from the client, checks the cache for the requested data, and if not found,
 * forwards the request to the intended server. It then sends the response to the client, either from the cache
 * or from the server's response. Additionally, it handles errors and manages semaphore synchronization for concurrency.
 *
 * @param arg A pointer to `ThreadArgs` struct containing the shared `Hashmap` and client socket descriptor.
 *            The `arg` should be cast to a `ThreadArgs` pointer within the function.
 *
 * @return Returns `NULL` upon completion. It does not return a specific value.
 *
 * @details The function handles the following tasks:
 *          - Receives the HTTP request from the client.
 *          - Checks if the request is cached.
 *          - If found in cache, sends the cached response back to the client.
 *          - If not found in cache, forwards the request to the target server, handles the response, caches it, and then sends it to the client.
 *          - Ensures thread synchronization using semaphores.
 *          - Properly handles memory allocation, socket management, and client disconnects.
 *
 * @example
 *   // Example code using thread_fn function in a thread pool
 *   pthread_create(&thread, NULL, thread_fn, (void *)&args);
 *   // Where `args` contains the necessary data (e.g., Hashmap pointer and client socket).
 */

void *thread_fn(void *arg)
{
	ThreadArgs *args = (ThreadArgs *)arg; // Extracting args from arg

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
	return NULL;
}


/**
 * @brief Computes the hash value for a given string (URL).
 *
 * This function uses a polynomial hash function to compute the hash value of a string.
 * The hash value is calculated by iterating over the characters of the string and updating
 * the hash using the formula `hashValue = (hashValue * 31) + *key`, where `31` is a constant multiplier.
 * The result is then modulo'd by `HASH_SIZE` to ensure that the hash value fits within the range of the hash table.
 *
 * @param key A pointer to the string (URL) for which the hash value needs to be computed.
 *
 * @return Returns the computed hash value, which is the index in the hash table.
 *         The value is between `0` and `HASH_SIZE - 1`.
 *
 * @details The function computes the hash of a string and returns an index value for storing/retrieving
 *          the string in a hashmap (of fixed size). It ensures the resulting value fits within the table bounds.
 *
 * @example
 *   const char *url = "https://example.com";
 *   unsigned int index = hash(url);
 *   // Use `index` to store or retrieve the corresponding data in a hashmap.
 */

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

/**
 * @brief Initializes the hashmap by setting all table entries to `NULL`.
 *
 * This function initializes the hashmap, setting all the entries in the hash table
 * to `NULL`. It ensures that the hashmap is ready to store key-value pairs.
 * The `table` in the `Hashmap` struct is an array of pointers, and this function
 * clears them to prevent undefined behavior when inserting or retrieving elements.
 *
 * @param map A pointer to the `Hashmap` that needs to be initialized.
 *
 * @details This function should be called before any operations on the hashmap (insertion or retrieval).
 *          It sets up an empty hash table where each entry is initialized to `NULL`.
 *
 * @example
 *   Hashmap map;
 *   initHashMap(&map);
 *   // Now the map is ready for insertion.
 */

void initHashMap(Hashmap *map)
{

	for (int i = 0; i < HASH_SIZE; i++)
	{
		map->table[i] = NULL;
	}
}

/**
 * @brief Inserts a new URL and its associated data into the hashmap, with cache eviction if necessary.
 *
 * This function inserts a new cache element into the hashmap. It first checks if the URL already exists
 * in the cache; if so, it skips insertion. If not, it allocates memory for a new cache element, adds it
 * to the hashmap at the appropriate index, and also updates the least recently used (LRU) queue for cache eviction.
 * If the cache size exceeds the maximum size (`MAX_SIZE`), it evicts the least recently used (LRU) element.
 *
 * @param map A pointer to the `Hashmap` where the data will be inserted.
 * @param data The data (e.g., the HTTP response) associated with the URL to store.
 * @param size The size of the `data` (in bytes).
 * @param key The URL (key) to associate with the `data`.
 *
 * @return This function doesn't return anything. It performs the insertion or handles cache eviction.
 *
 * @details The function performs the following:
 *          - Computes the hash index based on the URL (`key`).
 *          - Checks if the URL already exists in the cache and skips insertion if it does.
 *          - If not, it allocates memory for a new cache element and inserts it at the calculated hash index.
 *          - If the cache exceeds the size limit (`MAX_SIZE`), it evicts the least recently used element.
 *          - It also updates the LRU queue to keep track of recently accessed elements.
 * 
 * @note This function uses a mutex lock (`lock`) to ensure thread safety during insertion, and it uses a
 *       LRU-based eviction policy when the cache exceeds its size.
 *
 * @example
 *   // Example of using `insert` function
 *   char *data = "<HTML>...</HTML>";
 *   const char *key = "https://example.com";
 *   insert(&map, data, strlen(data), key);
 *   // The URL and its corresponding data are now stored in the hashmap.
 */

void insert(Hashmap *map, char *data, int size, const char *key)
{
	unsigned int index = hash(key);

	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Add Cache Lock Acquired %d\n", temp_lock_val);

	cache_element *existingNode = map->table[index];

	// Check if the key already exists in the hashmap
	while (existingNode)
	{
		if (strcmp(existingNode->url, key) == 0)
		{
			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Already Present \nAdd Cache Lock Unlocked %d\n", temp_lock_val);
			return;
		}
		existingNode = existingNode->next;
	}

	// If not found, insert a new node
	int element_size = size + 1 + strlen(key) + sizeof(cache_element); // Size of the new element which will be added to the cache
	if (element_size > MAX_ELEMENT_SIZE)
	{
		temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
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
			printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);

			return;
		}

		element->data = (char *)malloc(size + 1);

		if (element->data == NULL)
		{
			free(element);
			element = NULL;
			fprintf(stderr, "Failed to allocate memory for cache element's data \n");

			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);
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
			printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);
			return;
		}

		strcpy(element->url, key);

		element->lru_time_track = time(NULL); // Updating the time_track

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
		printf("Add Cache Lock Unlocked %d\n", temp_lock_val);

		return;
	}
	return;
}

/**
 * @brief Searches for a cache element associated with a given URL in the hashmap.
 *
 * This function searches the hashmap for a cache element that corresponds to the given URL.
 * If the URL is found in the hashmap, it updates the Least Recently Used (LRU) timestamp to
 * reflect the most recent access and moves the element to the front of the LRU queue (i.e., 
 * the head of the queue). This function ensures the LRU cache eviction policy is followed.
 * If the URL is not found, the function returns `NULL`.
 *
 * @param map A pointer to the `Hashmap` where the search is performed.
 * @param key A pointer to the URL (key) to search for in the hashmap.
 *
 * @return A pointer to the `cache_element` associated with the given URL if found, otherwise `NULL`.
 *
 * @details The function performs the following:
 *          - Computes the hash index for the given URL.
 *          - Searches through the linked list at the computed index.
 *          - If the URL is found, it updates the LRU time and moves the element to the front of the LRU queue.
 *          - If not found, the function returns `NULL`.
 * 
 * @example
 *   Hashmap map;
 *   cache_element *element = search(&map, "https://example.com");
 *   if (element != NULL) {
 *       printf("Found element with URL: %s\n", element->url);
 *   } else {
 *       printf("URL not found\n");
 *   }
 */

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
			printf("LRU Time Track Before : %ld", node->lru_time_track);
			printf("\nurl found\n");
			// Updating the time_track
			node->lru_time_track = time(NULL);
			printf("LRU Time Track After : %ld", node->lru_time_track);

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
			printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);

			return node;
		}
		node = node->next;
	}
	printf("\nurl not found\n");

	temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);
	return NULL;
}

/**
 * @brief Deletes a cache element from the hashmap and the LRU queue.
 *
 * This function evicts the least recently used (LRU) cache element from both the hashmap and the LRU queue.
 * It is typically called when the cache exceeds its size limit, and the LRU eviction policy needs to be applied.
 * The node is removed from the hashmap (using its hash index), the memory is freed, and the cache size is updated.
 *
 * @param map A pointer to the `Hashmap` from which the node will be deleted.
 *
 * @details The function performs the following:
 *          - Locks the cache to ensure thread-safe deletion.
 *          - Retrieves the least recently used node from the tail of the LRU queue.
 *          - Removes the node from the hashmap by traversing the list at the hash index.
 *          - Frees the memory associated with the URL and cache element.
 *          - Updates the cache size after deletion.
 * 
 * @example
 *   deleteNode(&map);
 *   // Deletes the least recently used element from the cache.
 */

void deleteNode(Hashmap *map)
{
	int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Remove Cache Lock Acquired %d\n", temp_lock_val);

	if (qtail == NULL)
	{
		temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);
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

			free(node->url);
			free(node);

			// Decrement the size of cache
			cache_size -= node->len;

			temp_lock_val = pthread_mutex_unlock(&lock);
			printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);

			return;
		}
		prevNode = node;
		node = node->next;
	}
	temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Remove Cache Lock Unlocked %d\n", temp_lock_val);
}

/**
 * @brief Frees all memory allocated for the cache elements in the hashmap.
 *
 * This function iterates through all the entries in the hashmap and frees the memory allocated
 * for each cache element, including its URL and data. It is typically used when the program terminates
 * or when the cache is no longer needed.
 *
 * @param map A pointer to the `Hashmap` to be freed.
 *
 * @details The function performs the following:
 *          - Iterates through the hashmap and frees each cache element.
 *          - Frees the memory allocated for the URL and data of each cache element.
 *          - The memory for the cache elements is fully cleaned up to prevent memory leaks.
 * 
 * @example
 *   freeHashMap(&map);
 *   // Frees all memory used by the cache.
 */

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