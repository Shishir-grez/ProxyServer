# Makefile for Windows Proxy Server using GCC/MinGW
# Run with 'mingw32-make -f GNUmakefile'

CC = gcc
CFLAGS = -Wall -O2 -D_WIN32_WINNT=0x0600
LDFLAGS = -s
LIBS = -lws2_32 -lpthread

# Target executable
PROXY = proxy_server.exe

# Source files
SOURCES = proxy_server_with_cache.c proxy_parse.c

# Object files
OBJECTS = $(SOURCES:.c=.o)

# Build rules
all: $(PROXY)

$(PROXY): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(PROXY)

# Dependencies
proxy_server_with_cache.o: proxy_server_with_cache.c proxy_parse.h
proxy_parse.o: proxy_parse.c proxy_parse.h
