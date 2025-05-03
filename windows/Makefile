# Makefile for Windows Proxy Server
# Use with Microsoft NMAKE utility

CC = cl
CFLAGS = /W3 /O2 /nologo /D_CRT_SECURE_NO_WARNINGS
LDFLAGS = /nologo
LIBS = ws2_32.lib

# Target executable
PROXY = proxy_server.exe

# Source files
SOURCES = proxy_server_with_cache.c proxy_parse.c

# Object files
OBJECTS = $(SOURCES:.c=.obj)

# Build rules
all: $(PROXY)

$(PROXY): $(OBJECTS)
	$(CC) $(LDFLAGS) /Fe:$@ $(OBJECTS) $(LIBS)

.c.obj:
	$(CC) $(CFLAGS) /c $<

clean:
	del *.obj *.exe *.pdb *.ilk

# Dependencies
proxy_server_with_cache.obj: proxy_server_with_cache.c proxy_parse.h
proxy_parse.obj: proxy_parse.c proxy_parse.h
