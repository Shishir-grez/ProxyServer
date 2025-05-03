/*
  proxy_parse.c -- a HTTP Request Parsing Library.
  Windows version
*/

#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define DEFAULT_NHDRS 8
#define MAX_REQ_LEN 65535
#define MIN_REQ_LEN 4
#define DEBUG 0

static const char *root_abs_path = "/";

/* private function declarations */
int ParsedRequest_printRequestLine(ParsedRequest *pr, 
                                  char * buf, size_t buflen,
                                  size_t *tmp);
size_t ParsedRequest_requestLineLen(ParsedRequest *pr);

/*
 * debug() prints out debugging info if DEBUG is set to 1
 *
 * parameter format: same as printf 
 *
 */
void debug(const char * format, ...) {
     va_list args;
     if (DEBUG) {
          va_start(args, format);
          vfprintf(stderr, format, args);
          va_end(args);
     }
}

/*
 * ParsedHeader Public Methods
 */

/* Set a header with key and value */
int ParsedHeader_set(ParsedRequest *pr, const char * key, const char * value)
{
    if (!pr || !key || !value) return -1;
    
    // Remove any existing header with the same key
    ParsedHeader_remove(pr, key);
    
    // Check if we need to expand the headers array
    if (pr->headersLen <= strlen(pr->headers) + strlen(key) + strlen(value) + 4) {
        pr->headersLen = pr->headersLen * 2;
        char* newHeaders = (char*)realloc(pr->headers, pr->headersLen);
        if (!newHeaders)
            return -1;
        pr->headers = newHeaders;
    }
    
    // Append the new header
    strcat(pr->headers, key);
    strcat(pr->headers, ": ");
    strcat(pr->headers, value);
    strcat(pr->headers, "\r\n");
    
    return 0;
}

/* Get the value of a header, or NULL if not found */
const char *ParsedHeader_lookup(ParsedRequest *pr, const char *key)
{
    if (!pr || !key || !pr->headers) return NULL;
    
    char *start = pr->headers;
    size_t keyLen = strlen(key);
    
    while (start && *start) {
        // Find the next occurrence of the key
        char *keyPos = strstr(start, key);
        if (!keyPos) break;
        
        // Check if it's at the beginning of a line and followed by ':'
        if ((keyPos == start || keyPos[-1] == '\n') && keyPos[keyLen] == ':') {
            // Skip to the value
            char *valueStart = keyPos + keyLen + 1;
            // Skip whitespace
            while (*valueStart == ' ' || *valueStart == '\t') valueStart++;
            
            // Find the end of the line
            char *valueEnd = strstr(valueStart, "\r\n");
            if (!valueEnd) break;
            
            // Temporarily null-terminate the value
            *valueEnd = '\0';
            char *value = _strdup(valueStart);
            *valueEnd = '\r';
            
            // Store in headersPtr for later freeing
            if (pr->headersPtr) free(pr->headersPtr);
            pr->headersPtr = value;
            
            return value;
        }
        
        // Move past this occurrence
        start = keyPos + keyLen;
    }
    
    return NULL;
}

/* Remove the header with the specified key */
int ParsedHeader_remove(ParsedRequest *pr, const char *key)
{
    if (!pr || !key || !pr->headers) return -1;
    
    char *start = pr->headers;
    size_t keyLen = strlen(key);
    
    while (start && *start) {
        // Find the next occurrence of the key
        char *keyPos = strstr(start, key);
        if (!keyPos) break;
        
        // Check if it's at the beginning of a line and followed by ':'
        if ((keyPos == start || keyPos[-1] == '\n') && keyPos[keyLen] == ':') {
            // Find the end of the line
            char *lineEnd = strstr(keyPos, "\r\n");
            if (!lineEnd) break;
            lineEnd += 2; // Include the \r\n
            
            // Move the rest of the headers up to remove this line
            memmove(keyPos, lineEnd, strlen(lineEnd) + 1);
            return 0;
        }
        
        // Move past this occurrence
        start = keyPos + keyLen;
    }
    
    return -1;
}

/* Return the value of the content-length header or -1 if not found */
int ParsedRequest_getContentLength(ParsedRequest *pr)
{
    const char *lenStr = ParsedHeader_lookup(pr, "Content-Length");
    if (!lenStr) return -1;
    
    return atoi(lenStr);
}

/* Create a ParsedRequest structure */
ParsedRequest *ParsedRequest_create()
{
    ParsedRequest *pr = (ParsedRequest *)malloc(sizeof(ParsedRequest));
    if (!pr) return NULL;
    
    pr->method = NULL;
    pr->protocol = NULL;
    pr->host = NULL;
    pr->port = NULL;
    pr->path = NULL;
    pr->version = NULL;
    pr->buf = NULL;
    pr->buflen = 0;
    pr->headers = (char *)malloc(DEFAULT_NHDRS * 80); // Initial space for headers
    if (!pr->headers) {
        free(pr);
        return NULL;
    }
    pr->headers[0] = '\0';
    pr->headersPtr = NULL;
    pr->maxbuf = 0;
    pr->headerOff = 0;
    pr->headersLen = DEFAULT_NHDRS * 80;
    
    return pr;
}

/* Parse the request buffer in buf (with length buflen) */
int ParsedRequest_parse(ParsedRequest *pr, const char *buf, size_t buflen)
{
    if (!pr || !buf || buflen < MIN_REQ_LEN)
        return -1;
    
    // Copy the buffer
    pr->buf = (char *)malloc(buflen + 1);
    if (!pr->buf) return -1;
    memcpy(pr->buf, buf, buflen);
    pr->buf[buflen] = '\0';
    pr->buflen = buflen;
    
    // Find the end of the request line
    char *reqEnd = strstr(pr->buf, "\r\n");
    if (!reqEnd) return -1;
    *reqEnd = '\0';
    
    // Parse the request line
    char *method = pr->buf;
    char *path = strchr(method, ' ');
    if (!path) return -1;
    *path++ = '\0';
    
    char *version = strchr(path, ' ');
    if (!version) return -1;
    *version++ = '\0';
    
    // Copy the method
    pr->method = _strdup(method);
    
    // Parse the path
    if (path[0] == '/') {
        // Absolute path
        pr->path = _strdup(path);
        pr->protocol = _strdup("http");
        pr->host = NULL;
        pr->port = NULL;
    } else {
        // Full URL
        char *protocolEnd = strstr(path, "://");
        if (!protocolEnd) return -1;
        *protocolEnd = '\0';
        pr->protocol = _strdup(path);
        
        char *host = protocolEnd + 3;
        char *pathStart = strchr(host, '/');
        if (!pathStart) {
            pr->path = _strdup("/");
            pathStart = host + strlen(host);
        } else {
            pr->path = _strdup(pathStart);
            *pathStart = '\0';
        }
        
        char *portStart = strchr(host, ':');
        if (portStart) {
            *portStart = '\0';
            pr->port = _strdup(portStart + 1);
        } else {
            pr->port = _strdup("80");
        }
        
        pr->host = _strdup(host);
    }
    
    // Copy the version
    pr->version = _strdup(version);
    
    // Reset reqEnd to point to the \r\n
    *reqEnd = '\r';
    
    // Find the headers
    char *headersStart = reqEnd + 2;
    char *headersEnd = strstr(headersStart, "\r\n\r\n");
    if (!headersEnd) return -1;
    
    // Copy the headers
    size_t headersLen = headersEnd - headersStart;
    if (pr->headersLen <= headersLen) {
        pr->headersLen = headersLen + 1;
        char *newHeaders = (char *)realloc(pr->headers, pr->headersLen);
        if (!newHeaders) return -1;
        pr->headers = newHeaders;
    }
    
    memcpy(pr->headers, headersStart, headersLen);
    pr->headers[headersLen] = '\0';
    
    // If no host header was found in the URL, look for it in the headers
    if (!pr->host) {
        const char *hostHeader = ParsedHeader_lookup(pr, "Host");
        if (hostHeader) {
            char *hostCopy = _strdup(hostHeader);
            char *portStart = strchr(hostCopy, ':');
            if (portStart) {
                *portStart = '\0';
                pr->port = _strdup(portStart + 1);
            } else {
                pr->port = _strdup("80");
            }
            pr->host = hostCopy;
        }
    }
    
    return 0;
}

/* Destroy the ParsedRequest struct and free its resources */
void ParsedRequest_destroy(ParsedRequest *pr)
{
    if (!pr) return;
    
    if (pr->method) free(pr->method);
    if (pr->protocol) free(pr->protocol);
    if (pr->host) free(pr->host);
    if (pr->port) free(pr->port);
    if (pr->path) free(pr->path);
    if (pr->version) free(pr->version);
    if (pr->buf) free(pr->buf);
    if (pr->headers) free(pr->headers);
    if (pr->headersPtr) free(pr->headersPtr);
    
    free(pr);
}
