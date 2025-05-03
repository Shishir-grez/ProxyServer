/*
 * Windows version of the multithreaded proxy server with caching
 * Ported from the Unix version
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "proxy_parse.h"

// Need to link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

#define HASH_SIZE 100        // Max size for Hash Table
#define MAX_BYTES 4096       // max allowed size of request/response
#define MAX_CLIENTS 400      // max number of client requests served at a time
#define MAX_SIZE 200 * (1 << 20)        // size of the cache 200MB
#define MAX_ELEMENT_SIZE 10 * (1 << 20) // max size of an element in cache 10 mb

// Forward declarations for cache element structure
typedef struct cache_element cache_element;

// Hashmap structure
typedef struct Hashmap {
    cache_element *table[HASH_SIZE];
} Hashmap;

// Thread arguments structure
typedef struct ThreadArgs {
    Hashmap *map;
    SOCKET socket;
} ThreadArgs;

// Cache element structure
struct cache_element {
    char *data;
    int len;
    char *url;
    struct cache_element *next;
    struct cache_element *left;
    struct cache_element *right;
};

// Function declarations
int sendErrorMessage(SOCKET socket, int status_code);
SOCKET connectToRemoteServer(char *host_addr, int port_num);
int handle_request(Hashmap *map, SOCKET proxy_socket, ParsedRequest *request, char *storeHttpRequest);
int checkHTTPversion(char *msg);
unsigned int __stdcall thread_fn(void *args);
unsigned int hash(const char *key);
void initHashMap(Hashmap *map);
cache_element *createNode(char *data, int size, char *key);
void insert(Hashmap *map, char *data, int size, const char *key);
cache_element *search(Hashmap *map, const char *key);
void deleteNode(Hashmap *map);
void freeHashMap(Hashmap *map);
int send_all(SOCKET sockfd, const void *buf, size_t len, int flags);
int recv_all(SOCKET sockfd, void *buf, size_t len, int flags);

// Global variables
int port_number;
SOCKET proxy_socketId;                    // socket descriptor of proxy server
HANDLE tid[MAX_CLIENTS];                  // array to store the thread handles
HANDLE semaphore;                         // semaphore for limiting concurrent clients
HANDLE cache_lock;                        // mutex for cache access
CRITICAL_SECTION lock;                    // critical section for thread synchronization

cache_element *qhead = NULL, *qtail = NULL; // Pointer to head and tail of queue
int cache_size;                             // cache_size denotes the current size of the cache

// Global flag to indicate program termination
volatile BOOL stop_flag = FALSE;

// Function to handle Ctrl+C signal
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
        stop_flag = TRUE;
        printf("\nReceived Ctrl+C. Cleaning up and exiting...\n");
        shutdown(proxy_socketId, SD_BOTH);
        return TRUE;
    }
    return FALSE;
}

// Helper function to send all data
int send_all(SOCKET sockfd, const void *buf, size_t len, int flags) {
    int total = 0;
    int bytesleft = len;
    int n;

    while (total < len) {
        n = send(sockfd, (const char*)buf + total, bytesleft, flags);
        if (n == SOCKET_ERROR) {
            return SOCKET_ERROR;
        }
        total += n;
        bytesleft -= n;
    }

    return total;
}

// Helper function to receive all data
int recv_all(SOCKET sockfd, void *buf, size_t len, int flags) {
    int total = 0;
    int bytesleft = len;
    int n;

    while (total < len) {
        n = recv(sockfd, (char*)buf + total, bytesleft, flags);
        if (n == SOCKET_ERROR || n == 0) {
            return (n == 0) ? total : SOCKET_ERROR;
        }
        total += n;
        bytesleft -= n;
    }

    return total;
}

// Send error message to client
int sendErrorMessage(SOCKET socket, int status_code) {
    char *status_phrase;
    char response[MAX_BYTES];

    switch (status_code) {
        case 400:
            status_phrase = "Bad Request";
            break;
        case 403:
            status_phrase = "Forbidden";
            break;
        case 404:
            status_phrase = "Not Found";
            break;
        case 500:
            status_phrase = "Internal Server Error";
            break;
        case 501:
            status_phrase = "Not Implemented";
            break;
        case 502:
            status_phrase = "Bad Gateway";
            break;
        case 503:
            status_phrase = "Service Unavailable";
            break;
        default:
            status_phrase = "Unknown Error";
    }

    sprintf(response, "HTTP/1.0 %d %s\r\n"
                     "Content-Type: text/html\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "<html><body>\r\n"
                     "<h1>%d %s</h1>\r\n"
                     "</body></html>\r\n",
            status_code, status_phrase,
            status_code, status_phrase);

    return send_all(socket, response, strlen(response), 0);
}

// Connect to remote server
SOCKET connectToRemoteServer(char *host_addr, int port_num) {
    SOCKET server_socket;
    struct sockaddr_in server_address;
    struct hostent *server;

    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        printf("Error creating socket to server: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }

    // Get server information
    server = gethostbyname(host_addr);
    if (server == NULL) {
        printf("ERROR: No such host\n");
        closesocket(server_socket);
        return INVALID_SOCKET;
    }

    // Set up server address
    memset((char *)&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    memcpy((char *)&server_address.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
    server_address.sin_port = htons(port_num);

    // Connect to server
    if (connect(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        printf("Error connecting to server: %d\n", WSAGetLastError());
        closesocket(server_socket);
        return INVALID_SOCKET;
    }

    return server_socket;
}

// Handle HTTP request
int handle_request(Hashmap *map, SOCKET proxy_socket, ParsedRequest *request, char *storeHttpRequest) {
    SOCKET server_socket;
    char buffer[MAX_BYTES];
    int bytes_received;
    int port_num;
    char *host_addr;

    // Get host and port from request
    host_addr = request->host;
    if (request->port) {
        port_num = atoi(request->port);
    } else {
        port_num = 80; // Default HTTP port
    }

    // Check if the request is in cache
    EnterCriticalSection(&lock);
    cache_element *cached = search(map, request->path);
    LeaveCriticalSection(&lock);

    if (cached != NULL) {
        printf("Cache hit for %s\n", request->path);
        // Send cached response
        if (send_all(proxy_socket, cached->data, cached->len, 0) == SOCKET_ERROR) {
            printf("Error sending cached response: %d\n", WSAGetLastError());
            return -1;
        }
        return 0;
    }

    printf("Cache miss for %s, forwarding to %s:%d\n", request->path, host_addr, port_num);

    // Connect to remote server
    server_socket = connectToRemoteServer(host_addr, port_num);
    if (server_socket == INVALID_SOCKET) {
        sendErrorMessage(proxy_socket, 502); // Bad Gateway
        return -1;
    }

    // Create a modified request with just the path instead of the full URL
    char modified_request[MAX_BYTES];
    // Start with the request line (e.g., "GET /path HTTP/1.0")
    sprintf(modified_request, "%s %s HTTP/1.0\r\n", request->method, request->path);
    
    // Add the Host header explicitly
    sprintf(modified_request + strlen(modified_request), "Host: %s", host_addr);
    if (port_num != 80) {
        sprintf(modified_request + strlen(modified_request), ":%d", port_num);
    }
    strcat(modified_request, "\r\n");
    
    // Copy other headers from the original request, but skip any we've already added
    const char *headers_start = strstr(storeHttpRequest, "\r\n") + 2; // Skip the request line
    const char *headers_end = strstr(headers_start, "\r\n\r\n");
    
    if (headers_end) {
        // Add all original headers except Host (which we already added)
        char *header_line = _strdup(headers_start);
        char *saveptr;
        char *line = strtok_s(header_line, "\r\n", &saveptr);
        
        while (line) {
            // Skip the Host header as we already added it
            if (strncmp(line, "Host:", 5) != 0) {
                strcat(modified_request, line);
                strcat(modified_request, "\r\n");
            }
            line = strtok_s(NULL, "\r\n", &saveptr);
        }
        
        free(header_line);
        
        // Add the final empty line to indicate end of headers
        strcat(modified_request, "\r\n");
        
        // If there's a request body, append it
        if (headers_end + 4 < storeHttpRequest + strlen(storeHttpRequest)) {
            strcat(modified_request, headers_end + 4);
        }
    } else {
        // If we couldn't parse headers properly, just add an empty line
        strcat(modified_request, "\r\n");
    }
    
    printf("Modified request: \n%s\n", modified_request);

    // Send modified request to server
    if (send_all(server_socket, modified_request, strlen(modified_request), 0) == SOCKET_ERROR) {
        printf("Error sending request to server: %d\n", WSAGetLastError());
        closesocket(server_socket);
        return -1;
    }

    // Receive response from server
    char *response = NULL;
    int response_size = 0;
    int response_capacity = MAX_BYTES;
    response = (char *)malloc(response_capacity);
    if (!response) {
        printf("Memory allocation failed\n");
        closesocket(server_socket);
        return -1;
    }

    while ((bytes_received = recv(server_socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
        // Ensure we have enough space in the response buffer
        if (response_size + bytes_received > response_capacity) {
            response_capacity *= 2;
            char *new_response = (char *)realloc(response, response_capacity);
            if (!new_response) {
                printf("Memory reallocation failed\n");
                free(response);
                closesocket(server_socket);
                return -1;
            }
            response = new_response;
        }

        // Append received data to response
        memcpy(response + response_size, buffer, bytes_received);
        response_size += bytes_received;

        // Forward data to client
        if (send_all(proxy_socket, buffer, bytes_received, 0) == SOCKET_ERROR) {
            printf("Error forwarding response to client: %d\n", WSAGetLastError());
            free(response);
            closesocket(server_socket);
            return -1;
        }
    }

    // Cache the response if it's not too large
    if (response_size > 0 && response_size <= MAX_ELEMENT_SIZE) {
        EnterCriticalSection(&lock);
        insert(map, response, response_size, request->path);
        LeaveCriticalSection(&lock);
    } else {
        free(response);
    }

    closesocket(server_socket);
    return 0;
}

// Check HTTP version
int checkHTTPversion(char *msg) {
    char *version;
    version = strstr(msg, "HTTP/");
    if (version == NULL) {
        return -1;
    }
    version += 5; // Move past "HTTP/"
    if (strncmp(version, "1.0", 3) == 0 || strncmp(version, "1.1", 3) == 0) {
        return 0;
    }
    return -1;
}

// Thread function to handle client requests
unsigned int __stdcall thread_fn(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    SOCKET client_socket = args->socket;
    Hashmap *map = args->map;
    char buffer[MAX_BYTES];
    int bytes_received;

    // Wait for client request
    bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received == SOCKET_ERROR || bytes_received == 0) {
        printf("Error receiving request or client disconnected: %d\n", WSAGetLastError());
        closesocket(client_socket);
        free(args);
        return 1;
    }

    // Null-terminate the request
    buffer[bytes_received] = '\0';

    // Check HTTP version
    if (checkHTTPversion(buffer) != 0) {
        sendErrorMessage(client_socket, 400); // Bad Request
        closesocket(client_socket);
        free(args);
        return 1;
    }

    // Parse the request
    ParsedRequest *request = ParsedRequest_create();
    if (!request) {
        sendErrorMessage(client_socket, 500); // Internal Server Error
        closesocket(client_socket);
        free(args);
        return 1;
    }

    if (ParsedRequest_parse(request, buffer, bytes_received) != 0) {
        sendErrorMessage(client_socket, 400); // Bad Request
        ParsedRequest_destroy(request);
        closesocket(client_socket);
        free(args);
        return 1;
    }

    // Handle the request
    handle_request(map, client_socket, request, buffer);

    // Clean up
    ParsedRequest_destroy(request);
    closesocket(client_socket);
    free(args);
    return 0;
}

// Hash function for cache keys
unsigned int hash(const char *key) {
    unsigned int hash_value = 0;
    while (*key) {
        hash_value = (hash_value << 5) + *key++;
    }
    return hash_value % HASH_SIZE;
}

// Initialize hash map
void initHashMap(Hashmap *map) {
    for (int i = 0; i < HASH_SIZE; i++) {
        map->table[i] = NULL;
    }
    cache_size = 0;
}

// Create a new cache node
cache_element *createNode(char *data, int size, char *key) {
    cache_element *newNode = (cache_element *)malloc(sizeof(cache_element));
    if (!newNode) {
        printf("Memory allocation failed for cache node\n");
        return NULL;
    }

    newNode->data = data;
    newNode->len = size;
    newNode->url = _strdup(key);
    newNode->next = NULL;
    newNode->left = NULL;
    newNode->right = NULL;

    return newNode;
}

// Insert into cache
void insert(Hashmap *map, char *data, int size, const char *key) {
    unsigned int index = hash(key);
    cache_element *newNode = createNode(data, size, (char *)key);
    if (!newNode) return;

    // Check if we need to make space in the cache
    while (cache_size + size > MAX_SIZE && qhead != NULL) {
        deleteNode(map);
    }

    // Insert at the head of the linked list
    newNode->next = map->table[index];
    map->table[index] = newNode;

    // Update queue (for LRU eviction)
    if (qtail == NULL) {
        qhead = qtail = newNode;
    } else {
        qtail->right = newNode;
        newNode->left = qtail;
        qtail = newNode;
    }

    cache_size += size;
    printf("Cached %s (%d bytes), total cache size: %d bytes\n", key, size, cache_size);
}

// Search in cache
cache_element *search(Hashmap *map, const char *key) {
    unsigned int index = hash(key);
    cache_element *current = map->table[index];

    while (current != NULL) {
        if (strcmp(current->url, key) == 0) {
            // Move to end of queue (most recently used)
            if (current != qtail) {
                // Remove from current position
                if (current == qhead) {
                    qhead = current->right;
                    if (qhead) qhead->left = NULL;
                } else {
                    current->left->right = current->right;
                    if (current->right) current->right->left = current->left;
                }

                // Add to end
                current->right = NULL;
                current->left = qtail;
                qtail->right = current;
                qtail = current;
            }
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Delete node from cache (LRU eviction)
void deleteNode(Hashmap *map) {
    if (qhead == NULL) return;

    cache_element *toDelete = qhead;
    unsigned int index = hash(toDelete->url);

    // Remove from queue
    qhead = qhead->right;
    if (qhead) qhead->left = NULL;
    else qtail = NULL;

    // Remove from hash table
    if (map->table[index] == toDelete) {
        map->table[index] = toDelete->next;
    } else {
        cache_element *current = map->table[index];
        while (current && current->next != toDelete) {
            current = current->next;
        }
        if (current) current->next = toDelete->next;
    }

    // Update cache size
    cache_size -= toDelete->len;
    printf("Evicted %s from cache (%d bytes), new cache size: %d bytes\n", toDelete->url, toDelete->len, cache_size);

    // Free memory
    free(toDelete->data);
    free(toDelete->url);
    free(toDelete);
}

// Free hash map
void freeHashMap(Hashmap *map) {
    for (int i = 0; i < HASH_SIZE; i++) {
        cache_element *current = map->table[i];
        while (current != NULL) {
            cache_element *temp = current;
            current = current->next;
            free(temp->data);
            free(temp->url);
            free(temp);
        }
        map->table[i] = NULL;
    }
    cache_size = 0;
    qhead = qtail = NULL;
}

int main(int argc, char *argv[]) {
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        printf("WSAStartup failed: %d\n", result);
        return 1;
    }

    // Register console handler for Ctrl+C
    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        printf("ERROR: Could not set control handler\n");
        WSACleanup();
        return 1;
    }

    // Initialize hashmap
    Hashmap map;
    initHashMap(&map);

    // Initialize synchronization objects
    InitializeCriticalSection(&lock);
    semaphore = CreateSemaphore(NULL, MAX_CLIENTS, MAX_CLIENTS, NULL);
    if (semaphore == NULL) {
        printf("CreateSemaphore error: %d\n", GetLastError());
        DeleteCriticalSection(&lock);
        WSACleanup();
        return 1;
    }

    // Parse command line arguments
    if (argc == 2) {
        port_number = atoi(argv[1]);
    } else {
        printf("Usage: %s <port>\n", argv[0]);
        DeleteCriticalSection(&lock);
        CloseHandle(semaphore);
        WSACleanup();
        return 1;
    }

    printf("Setting Proxy Server Port: %d\n", port_number);

    // Create proxy socket
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId == INVALID_SOCKET) {
        printf("Failed to create socket: %d\n", WSAGetLastError());
        DeleteCriticalSection(&lock);
        CloseHandle(semaphore);
        WSACleanup();
        return 1;
    }

    // Set socket option for address reuse
    BOOL reuse = TRUE;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) == SOCKET_ERROR) {
        printf("setsockopt(SO_REUSEADDR) failed: %d\n", WSAGetLastError());
        closesocket(proxy_socketId);
        DeleteCriticalSection(&lock);
        CloseHandle(semaphore);
        WSACleanup();
        return 1;
    }

    // Set up server address
    struct sockaddr_in server_addr;
    memset((char *)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind socket
    if (bind(proxy_socketId, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        closesocket(proxy_socketId);
        DeleteCriticalSection(&lock);
        CloseHandle(semaphore);
        WSACleanup();
        return 1;
    }
    printf("Binding on port: %d\n", port_number);

    // Listen for connections
    if (listen(proxy_socketId, MAX_CLIENTS) == SOCKET_ERROR) {
        printf("Listen failed: %d\n", WSAGetLastError());
        closesocket(proxy_socketId);
        DeleteCriticalSection(&lock);
        CloseHandle(semaphore);
        WSACleanup();
        return 1;
    }

    int i = 0; // Thread index
    SOCKET connected_sockets[MAX_CLIENTS]; // Array to store client sockets

    // Main server loop
    while (!stop_flag) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);

        // Accept client connection
        SOCKET client_socket = accept(proxy_socketId, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket == INVALID_SOCKET) {
            if (WSAGetLastError() != WSAEINTR) { // Not interrupted by signal
                printf("Accept failed: %d\n", WSAGetLastError());
            }
            continue;
        }

        // Check if we've reached max clients
        if (i >= MAX_CLIENTS) {
            printf("Max clients reached. Rejecting connection.\n");
            closesocket(client_socket);
            continue;
        }

        // Store client socket
        connected_sockets[i] = client_socket;

        // Get client IP address
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Accepted connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        // Wait for semaphore
        DWORD wait_result = WaitForSingleObject(semaphore, 0);
        if (wait_result == WAIT_TIMEOUT) {
            printf("Too many concurrent clients. Connection queued.\n");
            wait_result = WaitForSingleObject(semaphore, INFINITE);
        }

        // Create thread arguments
        ThreadArgs *args = (ThreadArgs *)malloc(sizeof(ThreadArgs));
        if (!args) {
            printf("Memory allocation failed\n");
            closesocket(client_socket);
            ReleaseSemaphore(semaphore, 1, NULL);
            continue;
        }
        args->map = &map;
        args->socket = client_socket;

        // Create thread to handle client
        tid[i] = (HANDLE)_beginthreadex(NULL, 0, thread_fn, args, 0, NULL);
        if (tid[i] == NULL) {
            printf("Thread creation failed: %d\n", GetLastError());
            free(args);
            closesocket(client_socket);
            ReleaseSemaphore(semaphore, 1, NULL);
            continue;
        }

        // Increment thread index
        i++;
    }

    // Clean up
    printf("Cleaning up resources...\n");
    
    // Close all client sockets
    for (int j = 0; j < i; j++) {
        closesocket(connected_sockets[j]);
        WaitForSingleObject(tid[j], INFINITE);
        CloseHandle(tid[j]);
    }

    // Close proxy socket
    closesocket(proxy_socketId);
    
    // Free cache
    freeHashMap(&map);
    
    // Clean up synchronization objects
    DeleteCriticalSection(&lock);
    CloseHandle(semaphore);
    
    // Clean up Winsock
    WSACleanup();
    
    printf("Proxy server shut down successfully\n");
    return 0;
}
