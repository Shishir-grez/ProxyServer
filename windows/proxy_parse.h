#ifndef _PROXY_PARSE
#define _PROXY_PARSE

/* For Windows compatibility */
#include <winsock2.h>
#include <ws2tcpip.h>

#define MAX_HEADER_SIZE 8192

/* This structure is used to pass the parsed values */
typedef struct ParsedRequest {
  char *method;
  char *protocol;
  char *host;
  char *port;
  char *path;
  char *version;
  char *buf;
  size_t buflen;
  char *headers;
  char *headersPtr;
  /* private values */
  size_t maxbuf;
  int headerOff;
  int headersLen;
} ParsedRequest;

/* Create a ParsedRequest structure */
ParsedRequest *ParsedRequest_create();

/* Parse the request buffer in buf (with length buflen) */
int ParsedRequest_parse(ParsedRequest *parse, const char *buf, size_t buflen);

/* Destroy the ParsedRequest struct and free its resources */
void ParsedRequest_destroy(ParsedRequest *parse);

/* Return the value of a header, or NULL if not found */
const char *ParsedHeader_lookup(ParsedRequest *parse, const char *key);

/* Set the value of a header, creating it if necessary */
int ParsedHeader_set(ParsedRequest *parse, const char *key, const char *value);

/* Remove the header with the specified key */
int ParsedHeader_remove(ParsedRequest *parse, const char *key);

/* Return the value of the content-length header or -1 if not found */
int ParsedRequest_getContentLength(ParsedRequest *parse);

#endif /* _PROXY_PARSE */
