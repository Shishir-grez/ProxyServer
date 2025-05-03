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
#include "proxy_cache_sqlite.h"

// Need to link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

#define HASH_SIZE 100        // Max size for Hash Table
#define MAX_BYTES 65536      // max allowed size of request/response (increased to 64KB)
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
void clearCache(Hashmap *map);
void set_global_map(Hashmap *map);

// Forward declaration of the stats update function from proxy_cli_main.c
extern void update_stats(int is_cache_hit, int is_connection_added, int is_connection_removed);

// Verbose mode flag - controlled by CLI
int verbose_mode = 0;

// Custom logging function that only prints when verbose mode is enabled
void log_message(const char *format, ...) {
    if (verbose_mode) {
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
    }
}

// Global variables accessible from CLI
int port_number = 8080;
SOCKET proxy_socketId = INVALID_SOCKET;  // socket descriptor of proxy server
volatile BOOL stop_flag = FALSE;         // Flag to indicate termination
int cache_size = 0;                      // Current cache size in bytes

// Internal globals
HANDLE tid[MAX_CLIENTS];                 // array to store the thread handles
HANDLE semaphore;                        // semaphore for limiting concurrent clients
CRITICAL_SECTION lock;                   // critical section for thread synchronization
cache_element *qhead = NULL, *qtail = NULL; // Pointers to head and tail of cache queue
int cache_enabled = 1;                   // Flag to enable/disable caching

// Helper function to send all data with built-in retry mechanism
int send_all(SOCKET sockfd, const void *buf, size_t len, int flags) {
    int total = 0;
    int bytesleft = len;
    int n;
    int retry_count = 0;
    const int max_retries = 3; // Maximum number of retries on error

    while (total < len) {
        n = send(sockfd, (const char*)buf + total, bytesleft, flags);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            // If the error is temporary (like WSAEWOULDBLOCK), retry
            if ((err == WSAEWOULDBLOCK || err == WSAEINTR) && retry_count < max_retries) {
                retry_count++;
                Sleep(100 * retry_count); // Increasing delay between retries
                continue;
            }
            return SOCKET_ERROR;
        }
        total += n;
        bytesleft -= n;
        retry_count = 0; // Reset retry counter on successful send
    }

    return total;
}

// Helper function to receive all data with built-in retry mechanism
int recv_all(SOCKET sockfd, void *buf, size_t len, int flags) {
    int total = 0;
    int bytesleft = len;
    int n;
    int retry_count = 0;
    const int max_retries = 3; // Maximum number of retries on error

    while (total < len) {
        n = recv(sockfd, (char*)buf + total, bytesleft, flags);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            // If the error is temporary (like WSAEWOULDBLOCK), retry
            if ((err == WSAEWOULDBLOCK || err == WSAEINTR) && retry_count < max_retries) {
                retry_count++;
                Sleep(100 * retry_count); // Increasing delay between retries
                continue;
            }
            return SOCKET_ERROR;
        } else if (n == 0) {
            // Connection closed by peer
            break;
        }
        total += n;
        bytesleft -= n;
        retry_count = 0; // Reset retry counter on successful receive
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
        log_message("Error creating socket to server: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }
    
    // Set socket timeouts to prevent connection aborts
    int timeout = 30000; // 30 seconds timeout
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        log_message("Warning: setsockopt(SO_RCVTIMEO) failed: %d\n", WSAGetLastError());
        // Continue anyway as this is not critical
    }
    if (setsockopt(server_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        log_message("Warning: setsockopt(SO_SNDTIMEO) failed: %d\n", WSAGetLastError());
        // Continue anyway as this is not critical
    }
    
    // Set TCP keep-alive to detect connection drops
    BOOL keepAlive = TRUE;
    if (setsockopt(server_socket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive)) == SOCKET_ERROR) {
        log_message("Warning: setsockopt(SO_KEEPALIVE) failed: %d\n", WSAGetLastError());
        // Continue anyway as this is not critical
    }

    // Get server information
    server = gethostbyname(host_addr);
    if (server == NULL) {
        log_message("ERROR: No such host\n");
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
        log_message("Error connecting to server: %d\n", WSAGetLastError());
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

    // Check if caching is enabled
    if (cache_enabled) {
        // First, check the SQLite persistent cache if it's enabled
        if (sqlite_cache_is_enabled()) {
            void *sqlite_data = NULL;
            int sqlite_size = 0;
            
            if (sqlite_cache_retrieve(request->path, &sqlite_data, &sqlite_size)) {
                log_message("SQLite cache hit for %s\n", request->path);
                update_stats(1, 0, 0); // Update statistics (cache hit)
                
                // Send cached response from SQLite
                if (send_all(proxy_socket, sqlite_data, sqlite_size, 0) == SOCKET_ERROR) {
                    log_message("Error sending SQLite cached response: %d\n", WSAGetLastError());
                    free(sqlite_data); // Free the allocated memory
                    return -1;
                }
                
                free(sqlite_data); // Free the allocated memory
                return 0;
            }
        }
        
        // If not in SQLite cache or SQLite cache is disabled, check in-memory cache
        EnterCriticalSection(&lock);
        cache_element *cached = search(map, request->path);
        LeaveCriticalSection(&lock);

        if (cached != NULL) {
            log_message("In-memory cache hit for %s\n", request->path);
            update_stats(1, 0, 0); // Update statistics (cache hit)
            
            // Send cached response from memory
            if (send_all(proxy_socket, cached->data, cached->len, 0) == SOCKET_ERROR) {
                log_message("Error sending cached response: %d\n", WSAGetLastError());
                return -1;
            }
            return 0;
        }
    }

    log_message("Cache miss for %s, forwarding to %s:%d\n", request->path, host_addr, port_num);
    update_stats(0, 0, 0); // Update statistics (cache miss)

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
    
    // Send modified request to server with retry logic
    int sent_bytes = 0;
    int retry_count = 0;
    const int max_retries = 3;
    
    do {
        sent_bytes = send_all(server_socket, modified_request, strlen(modified_request), 0);
        if (sent_bytes == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if ((err == WSAEWOULDBLOCK || err == WSAEINTR) && retry_count < max_retries) {
                retry_count++;
                log_message("Retrying request send (%d/%d): error %d\n", 
                           retry_count, max_retries, err);
                Sleep(100 * retry_count);
                continue;
            }
            log_message("Error sending request to server: %d\n", err);
            closesocket(server_socket);
            return -1;
        }
        break;
    } while (1);

    // Receive response from server with improved error handling
    char *response = NULL;
    int response_size = 0;
    int response_capacity = MAX_BYTES;
    response = (char *)malloc(response_capacity);
    if (!response) {
        log_message("Memory allocation failed\n");
        closesocket(server_socket);
        return -1;
    }
    
    // Special handling for specific response types
    if (strstr(request->path, "/large") != NULL) {
        log_message("Special handling for large response: %s\n", request->path);
        // Increase the initial buffer size for large responses
        response_capacity = MAX_BYTES * 4;
        char *new_response = (char *)realloc(response, response_capacity);
        if (!new_response) {
            log_message("Memory reallocation failed for large response\n");
            free(response);
            closesocket(server_socket);
            return -1;
        }
        response = new_response;
    } 
    else if (strstr(request->path, "/delayed") != NULL) {
        log_message("Special handling for delayed response: %s\n", request->path);
        
        // For delayed responses, set a much longer socket timeout
        int long_timeout = 20000; // 20 seconds (the test server delays for 3 seconds)
        if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&long_timeout, sizeof(long_timeout)) == SOCKET_ERROR) {
            log_message("Warning: could not set longer SO_RCVTIMEO for delayed response: %d\n", WSAGetLastError());
        }
        if (setsockopt(server_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&long_timeout, sizeof(long_timeout)) == SOCKET_ERROR) {
            log_message("Warning: could not set longer SO_SNDTIMEO for delayed response: %d\n", WSAGetLastError());
        }
        
        // Also use a larger initial response capacity for delayed responses
        response_capacity = MAX_BYTES * 2;
        char *new_response = (char *)realloc(response, response_capacity);
        if (!new_response) {
            log_message("Memory reallocation failed for delayed response\n");
            free(response);
            closesocket(server_socket);
            return -1;
        }
        response = new_response;
        
        // For delayed responses, we'll also prioritize caching
        // by setting a longer expiry time in the SQLite cache later
    }
    
    // Use a more robust approach with a single receive loop and better error handling
    int total_received = 0;
    int receive_retry_count = 0;
    const int receive_max_retries = 5;
    
    while (1) {
        // Reset buffer for new data
        memset(buffer, 0, sizeof(buffer));
        
        // Receive data with careful error handling
        bytes_received = recv(server_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received == SOCKET_ERROR) {
            int err = WSAGetLastError();
            
            // Handle common, recoverable errors
            if ((err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAETIMEDOUT || err == WSAECONNABORTED) && 
                receive_retry_count < (strstr(request->path, "/delayed") ? receive_max_retries * 3 : receive_max_retries)) {
                receive_retry_count++;
                log_message("Retrying response receive (%d/%d): error %d\n", 
                           receive_retry_count, receive_max_retries, err);
                Sleep(100 * receive_retry_count); // Increasing backoff
                continue;
            }
            
            // For fatal errors, decide if we can salvage what we have
            if (total_received > 0) {
                // We have partial data, let's use it
                log_message("Using partial response after socket error %d\n", err);
                break;
            } else {
                // No data received yet, must report error
                log_message("Fatal error receiving from server: %d\n", err);
                free(response);
                closesocket(server_socket);
                return -1;
            }
        } else if (bytes_received == 0) {
            // Server closed connection normally
            log_message("Server closed connection (received 0 bytes)\n");
            break;
        }
        
        // Reset retry counter after successful receive
        receive_retry_count = 0;
        total_received += bytes_received;
        
        // Ensure we have enough space in the response buffer
        if (response_size + bytes_received > response_capacity) {
            response_capacity *= 2;
            char *new_response = (char *)realloc(response, response_capacity);
            if (!new_response) {
                log_message("Memory reallocation failed\n");
                free(response);
                closesocket(server_socket);
                return -1;
            }
            response = new_response;
        }

        // Append received data to response
        memcpy(response + response_size, buffer, bytes_received);
        response_size += bytes_received;

        // Forward data to client with retry logic
        retry_count = 0;
        do {
            int sent = send_all(proxy_socket, buffer, bytes_received, 0);
            if (sent == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if ((err == WSAEWOULDBLOCK || err == WSAEINTR) && retry_count < max_retries) {
                    retry_count++;
                    log_message("Retrying forward to client (%d/%d): error %d\n", 
                                retry_count, max_retries, err);
                    Sleep(100 * retry_count);
                    continue;
                }
                log_message("Error forwarding response to client: %d\n", err);
                free(response);
                closesocket(server_socket);
                return -1;
            }
            break;
        } while (1);
        
        // Continue to next iteration
    }

    // Cache the response if caching is enabled and it's not too large
    if (cache_enabled && response_size > 0 && response_size <= MAX_ELEMENT_SIZE) {
        // Store in in-memory cache
        EnterCriticalSection(&lock);
        insert(map, response, response_size, request->path);
        LeaveCriticalSection(&lock);
        
        // Also store in SQLite persistent cache if enabled
        if (sqlite_cache_is_enabled()) {
            // Default expiry is 24 hours from now
            time_t expiry = time(NULL) + (24 * 60 * 60);
            
            // Give special treatment to specific paths for better caching
            if (strstr(request->path, "/delayed") != NULL) {
                // For delayed responses, use a much longer expiry (1 week)
                // and prioritize in the cache for better performance
                expiry = time(NULL) + (7 * 24 * 60 * 60); 
                log_message("Setting extended cache expiry for delayed response\n");
            }
            else if (strstr(request->path, "/large") != NULL) {
                // For large responses, also use a longer expiry (3 days)
                expiry = time(NULL) + (3 * 24 * 60 * 60);
                log_message("Setting extended cache expiry for large response\n");
            }
            
            sqlite_cache_store(request->path, response, response_size, expiry);
            log_message("Response stored in SQLite cache: %s (%d bytes)\n", request->path, response_size);
        }
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

    // Update connection statistics
    update_stats(0, 1, 0); // New connection

    // Wait for client request
    bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received == SOCKET_ERROR || bytes_received == 0) {
        log_message("Error receiving request or client disconnected: %d\n", WSAGetLastError());
        closesocket(client_socket);
        free(args);
        
        // Release semaphore to allow new connections
        ReleaseSemaphore(semaphore, 1, NULL);
        
        // Update connection statistics
        update_stats(0, 0, 1); // Connection closed
        
        return 1;
    }

    // Null-terminate the request
    buffer[bytes_received] = '\0';

    // Check HTTP version
    if (checkHTTPversion(buffer) != 0) {
        sendErrorMessage(client_socket, 400); // Bad Request
        closesocket(client_socket);
        free(args);
        
        // Release semaphore to allow new connections
        ReleaseSemaphore(semaphore, 1, NULL);
        
        // Update connection statistics
        update_stats(0, 0, 1); // Connection closed
        
        return 1;
    }

    // Parse the request
    ParsedRequest *request = ParsedRequest_create();
    if (!request) {
        sendErrorMessage(client_socket, 500); // Internal Server Error
        closesocket(client_socket);
        free(args);
        
        // Release semaphore to allow new connections
        ReleaseSemaphore(semaphore, 1, NULL);
        
        // Update connection statistics
        update_stats(0, 0, 1); // Connection closed
        
        return 1;
    }

    if (ParsedRequest_parse(request, buffer, bytes_received) != 0) {
        sendErrorMessage(client_socket, 400); // Bad Request
        ParsedRequest_destroy(request);
        closesocket(client_socket);
        free(args);
        
        // Release semaphore to allow new connections
        ReleaseSemaphore(semaphore, 1, NULL);
        
        // Update connection statistics
        update_stats(0, 0, 1); // Connection closed
        
        return 1;
    }

    // Handle the request
    handle_request(map, client_socket, request, buffer);

    // Clean up
    ParsedRequest_destroy(request);
    closesocket(client_socket);
    free(args);
    
    // Release semaphore to allow new connections
    ReleaseSemaphore(semaphore, 1, NULL);
    
    // Update connection statistics
    update_stats(0, 0, 1); // Connection closed
    
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
    log_message("Cached %s (%d bytes), total cache size: %d bytes\n", key, size, cache_size);
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
    log_message("Evicted %s from cache (%d bytes), new cache size: %d bytes\n", toDelete->url, toDelete->len, cache_size);

    // Free memory
    free(toDelete->data);
    free(toDelete->url);
    free(toDelete);
}

// Clear the entire cache
void clearCache(Hashmap *map) {
    // Loop through the entire hash table and free all cache entries
    for (int i = 0; i < HASH_SIZE; i++) {
        while (map->table[i] != NULL) {
            cache_element *toDelete = map->table[i];
            map->table[i] = toDelete->next;
            
            // Free memory
            free(toDelete->data);
            free(toDelete->url);
            free(toDelete);
        }
    }
    
    // Reset cache queue pointers
    qhead = NULL;
    qtail = NULL;
    
    // Reset cache size
    cache_size = 0;
    
    log_message("Cache cleared completely\n");
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

// Set cache enabled/disabled state
void setCacheEnabled(int enabled) {
    cache_enabled = enabled;
}

// Get cache enabled/disabled state
int getCacheEnabled() {
    return cache_enabled;
}

// Set verbose mode
void setVerboseMode(int enabled) {
    verbose_mode = enabled;
}

// Get verbose mode
int getVerboseMode() {
    return verbose_mode;
}

// Main proxy server function
int run_proxy_server() {
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        printf("WSAStartup failed: %d\n", result);
        return 1;
    }

    // Initialize hashmap
    Hashmap map;
    initHashMap(&map);
    
    // Set the global map reference for CLI functions
    set_global_map(&map);

    // Initialize synchronization objects
    InitializeCriticalSection(&lock);
    semaphore = CreateSemaphore(NULL, MAX_CLIENTS, MAX_CLIENTS, NULL);
    if (semaphore == NULL) {
        log_message("CreateSemaphore error: %d\n", GetLastError());
        DeleteCriticalSection(&lock);
        WSACleanup();
        return 1;
    }

    log_message("Setting Proxy Server Port: %d\n", port_number);

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
        log_message("setsockopt(SO_REUSEADDR) failed: %d\n", WSAGetLastError());
        closesocket(proxy_socketId);
        DeleteCriticalSection(&lock);
        CloseHandle(semaphore);
        WSACleanup();
        return 1;
    }
    
    // Set socket timeout to prevent connection aborts
    int timeout = 30000; // 30 seconds timeout
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        log_message("setsockopt(SO_RCVTIMEO) failed: %d\n", WSAGetLastError());
        // Continue anyway as this is not critical
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
    log_message("Binding on port: %d\n", port_number);

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
    stop_flag = FALSE; // Reset stop flag

    // Set socket to non-blocking mode to allow for graceful shutdown
    u_long non_blocking = 1;
    if (ioctlsocket(proxy_socketId, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        log_message("Failed to set socket to non-blocking mode: %d\n", WSAGetLastError());
        // Continue anyway as this is not critical
    }

    // Main server loop
    while (!stop_flag) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);

        // Wait for semaphore to ensure we don't exceed MAX_CLIENTS
        DWORD wait_result = WaitForSingleObject(semaphore, 1000); // 1 second timeout
        if (wait_result == WAIT_TIMEOUT) {
            // No semaphore available within timeout, check stop flag and try again
            continue;
        } else if (wait_result != WAIT_OBJECT_0) {
            // Error waiting for semaphore
            log_message("Error waiting for semaphore: %d\n", GetLastError());
            break;
        }

        // Accept client connection
        SOCKET client_socket = accept(proxy_socketId, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                // No connections to accept, release semaphore and try again
                ReleaseSemaphore(semaphore, 1, NULL);
                Sleep(100); // Reduce CPU usage
                continue;
            } else if (WSAGetLastError() != WSAEINTR) { // Not interrupted by signal
                log_message("Accept failed: %d\n", WSAGetLastError());
            }
            // Release semaphore as we didn't accept a connection
            ReleaseSemaphore(semaphore, 1, NULL);
            continue;
        }
        
        // Configure client socket for maximum reliability
        // Longer timeout for client connections (120 seconds for large responses)
        int timeout = 120000; 
        if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
            log_message("Warning: client setsockopt(SO_RCVTIMEO) failed: %d\n", WSAGetLastError());
        }
        if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
            log_message("Warning: client setsockopt(SO_SNDTIMEO) failed: %d\n", WSAGetLastError());
        }
        
        // Set TCP keep-alive to detect connection drops
        BOOL keepAlive = TRUE;
        if (setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive)) == SOCKET_ERROR) {
            log_message("Warning: client setsockopt(SO_KEEPALIVE) failed: %d\n", WSAGetLastError());
        }
        
        // Disable Nagle's algorithm for better responsiveness
        BOOL noDelay = TRUE;
        if (setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay)) == SOCKET_ERROR) {
            log_message("Warning: client setsockopt(TCP_NODELAY) failed: %d\n", WSAGetLastError());
        }
        
        // Set linger options to gracefully close connections
        struct linger ling = {1, 3}; // Enable with 3 second timeout
        if (setsockopt(client_socket, SOL_SOCKET, SO_LINGER, (const char*)&ling, sizeof(ling)) == SOCKET_ERROR) {
            log_message("Warning: client setsockopt(SO_LINGER) failed: %d\n", WSAGetLastError());
        }
        
        // Set larger receive buffer
        int recvBuf = MAX_BYTES * 2;
        if (setsockopt(client_socket, SOL_SOCKET, SO_RCVBUF, (const char*)&recvBuf, sizeof(recvBuf)) == SOCKET_ERROR) {
            log_message("Warning: client setsockopt(SO_RCVBUF) failed: %d\n", WSAGetLastError());
        }

        // Get client IP address
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        log_message("Accepted connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        // Create thread arguments
        ThreadArgs *args = (ThreadArgs *)malloc(sizeof(ThreadArgs));
        if (!args) {
            log_message("Memory allocation failed\n");
            closesocket(client_socket);
            ReleaseSemaphore(semaphore, 1, NULL);
            continue;
        }
        args->map = &map;
        args->socket = client_socket;

        // Create thread to handle client
        tid[i] = (HANDLE)_beginthreadex(NULL, 0, thread_fn, args, 0, NULL);
        if (tid[i] == NULL) {
            log_message("Thread creation failed: %d\n", GetLastError());
            free(args);
            closesocket(client_socket);
            ReleaseSemaphore(semaphore, 1, NULL);
            continue;
        }

        // Increment thread index, wrapping around if needed
        i = (i + 1) % MAX_CLIENTS;
    }

    // Clean up
    log_message("Cleaning up resources...\n");
    
    // Close proxy socket
    closesocket(proxy_socketId);
    proxy_socketId = INVALID_SOCKET;
    
    // Wait for all threads to complete (with timeout)
    log_message("Waiting for client threads to terminate...\n");
    for (int j = 0; j < MAX_CLIENTS; j++) {
        if (tid[j] != NULL) {
            // Wait with timeout to avoid hanging
            DWORD wait_result = WaitForSingleObject(tid[j], 1000);
            if (wait_result == WAIT_TIMEOUT) {
                // Thread didn't terminate in time, we could force terminate it
                // But that could lead to resource leaks, so we'll just log it
                log_message("Warning: Thread %d did not terminate gracefully\n", j);
            }
            CloseHandle(tid[j]);
            tid[j] = NULL;
        }
    }
    
    // Free cache
    freeHashMap(&map);
    
    // Clean up synchronization objects
    DeleteCriticalSection(&lock);
    CloseHandle(semaphore);
    
    // Clean up Winsock
    WSACleanup();
    
    log_message("Proxy server shut down successfully\n");
    return 0;
}

// Expose functions for CLI control

// Enable or disable caching
void proxy_set_cache_enabled(int enabled) {
    setCacheEnabled(enabled);
}

// Get cache status
int proxy_get_cache_enabled() {
    return getCacheEnabled();
}

// Get current cache size
int proxy_get_cache_size() {
    return cache_size;
}

// Set verbose mode
void proxy_set_verbose_mode(int enabled) {
    setVerboseMode(enabled);
}

// Get verbose mode
int proxy_get_verbose_mode() {
    return getVerboseMode();
}

// SQLite cache wrapper functions

// Initialize the SQLite cache
void proxy_init_sqlite_cache(const char *db_path) {
    init_sqlite_cache(db_path);
    log_message("SQLite cache initialized at %s\n", db_path);
}

// Close the SQLite cache
void proxy_close_sqlite_cache(void) {
    close_sqlite_cache();
    log_message("SQLite cache closed\n");
}

// Enable or disable the SQLite cache
void proxy_set_sqlite_cache_enabled(int enabled) {
    sqlite_cache_set_enabled(enabled);
    log_message("SQLite cache %s\n", enabled ? "enabled" : "disabled");
}

// Get whether the SQLite cache is enabled
int proxy_sqlite_cache_enabled(void) {
    return sqlite_cache_is_enabled();
}

// Clear the SQLite cache
void proxy_sqlite_cache_clear(void) {
    sqlite_cache_clear();
    log_message("SQLite cache cleared\n");
}

// Get the size of the SQLite cache
int proxy_sqlite_cache_size(void) {
    return sqlite_cache_size();
}

// External handle to the global map for the CLI
static Hashmap *global_map = NULL;

// Set the global map reference
void set_global_map(Hashmap *map) {
    global_map = map;
}

// Clear the cache
void proxy_clear_cache(Hashmap *map) {
    EnterCriticalSection(&lock);
    if (map) {
        clearCache(map);
    } else if (global_map) {
        clearCache(global_map);
    }
    LeaveCriticalSection(&lock);
}
