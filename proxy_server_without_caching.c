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

#define MAX_BYTES 4096
#define MAX_CLIENTS 400
#define MAX_SIZE 200 * (1 << 20)
#define MAX_ELEMENT_SIZE 10 * (1 << 20)

typedef struct cache_element cache_element;

struct cache_element
{
    char *data;
    int len;
    char *url;
    time_t lru_time_track;
    cache_element *next;
};

pthread_mutex_t lock;

int port_number = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
sem_t seamaphore;

cache_element *head;
int cache_size = 0;

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
        snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
        printf("400 Bad Request\n");
        send(socket, str, strlen(str), 0);
        break;
    case 403:
        snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
        printf("403 Forbidden\n");
        send(socket, str, strlen(str), 0);
        break;
    case 404:
        snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
        printf("404 Not Found\n");
        send(socket, str, strlen(str), 0);
        break;
    case 500:
        snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
        send(socket, str, strlen(str), 0);
        break;
    case 501:
        snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
        printf("501 Not Implemented\n");
        send(socket, str, strlen(str), 0);
        break;
    case 505:
        snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
        printf("505 HTTP Version Not Supported\n");
        send(socket, str, strlen(str), 0);
        break;
    default:
        return -1;
    }
    return 1;
}

int connectRemoteServer(char *host_addr, int port_num)
{
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (remoteSocket < 0)
    {
        printf("Error in Creating Socket.\n");
        return -1;
    }
    struct hostent *host = gethostbyname(host_addr);
    if (host == NULL)
    {
        fprintf(stderr, "No such host exists.\n");
        return -1;
    }
    struct sockaddr_in server_addr;
    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    bcopy((char *)host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);
    if (connect(remoteSocket, (struct sockaddr *)&server_addr, (socklen_t)sizeof(server_addr)) < 0)
    {
        fprintf(stderr, "Error in connecting !\n");
        return -1;
    }
    return remoteSocket;
}

int handle_request(int clientSocket, struct ParsedRequest *request, char *buf, char *tempReq)
{
    strcpy(buf, "GET ");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");
    size_t len = strlen(buf);
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
    if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0)
    {
        printf("unparse failed\n");
    }
    int server_port = 80;
    if (request->port != NULL)
        server_port = atoi(request->port);
    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if (remoteSocketID < 0)
        return -1;
    int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);
    bzero(buf, MAX_BYTES);
    bytes_send = recv(remoteSocketID, buf, MAX_BYTES - 1, 0);
    char *temp_buffer = (char *)malloc(sizeof(char) * MAX_BYTES);
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;
    while (bytes_send > 0)
    {
        bytes_send = send(clientSocket, buf, bytes_send, 0);
        for (int i = 0; i < bytes_send / sizeof(char); i++)
        {
            temp_buffer[temp_buffer_index] = buf[i];
            temp_buffer_index++;
        }
        temp_buffer_size += MAX_BYTES;
        temp_buffer = (char *)realloc(temp_buffer, temp_buffer_size);
        if (bytes_send < 0)
        {
            perror("Error in sending data to client socket.\n");
            break;
        }
        bzero(buf, MAX_BYTES);
        bytes_send = recv(remoteSocketID, buf, MAX_BYTES - 1, 0);
    }
    temp_buffer[temp_buffer_index] = '\0';
    free(temp_buffer);
    free(tempReq);
    printf("Done\n");
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
        version = 1;
    }
    else
    {
        version = -1;
    }
    return version;
}

void *thread_fn(void *socketNew)
{
    sem_wait(&seamaphore);
    int p;
    sem_getvalue(&seamaphore, &p);
    printf("semaphore value:%d\n", p);
    int *t = (int *)(socketNew);
    int socket = *t;
    int bytes_send_client, len;
    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);
    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0);
    while (bytes_send_client > 0)
    {
        len = strlen(buffer);
        if (strstr(buffer, "\r\n\r\n") == NULL)
        {
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
        }
        else
        {
            break;
        }
    }
    char *tempReq = (char *)malloc(strlen(buffer) * sizeof(char) + 10);
    for (int i = 0; i < strlen(buffer); i++)
    {
        tempReq[i] = buffer[i];
    }

    struct ParsedRequest *parsedRequest = ParsedRequest_parse(buffer);
    int status = checkHTTPversion(parsedRequest->version);
    if (status == 1)
    {
        int result = handle_request(socket, parsedRequest, buffer, tempReq);
    }
    else
    {
        sendErrorMessage(socket, 505);
    }
    pthread_exit(NULL);
}

void initServer(int port)
{
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    bind(proxy_socketId, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(proxy_socketId, MAX_CLIENTS);
}

int main()
{
    initServer(port_number);
    sem_init(&seamaphore, 0, 1);
    while (1)
    {
        struct sockaddr_in client_addr;
        int len = sizeof(client_addr);
        int new_sock = accept(proxy_socketId, (struct sockaddr *)&client_addr, &len);
        if (new_sock < 0)
        {
            perror("Error in accepting client socket\n");
            continue;
        }
        pthread_create(&(tid[0]), NULL, &thread_fn, (void *)&new_sock);
        pthread_join(tid[0], NULL);
    }
    return 0;
}
