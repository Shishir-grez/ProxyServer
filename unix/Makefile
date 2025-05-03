CC=gcc
CFLAGS= -g -Wall -O2
LDFLAGS= -lpthread -lsqlite3 -lreadline

all: proxy proxy_cli

proxy: proxy_server_with_cache.c
	$(CC) $(CFLAGS) -c proxy_parse.c -o proxy_parse.o
	$(CC) $(CFLAGS) -c proxy_server_with_cache.c -o proxy.o
	$(CC) $(CFLAGS) proxy_parse.o proxy.o -o proxy $(LDFLAGS)

# CLI version with SQLite persistent cache
proxy_cli: proxy_server_with_cli.c proxy_cache_sqlite.c
	$(CC) $(CFLAGS) -c proxy_parse.c -o proxy_parse.o
	$(CC) $(CFLAGS) -c proxy_cache_sqlite.c -o proxy_cache_sqlite.o
	$(CC) $(CFLAGS) -c proxy_server_with_cli.c -o proxy_server_with_cli.o
	$(CC) $(CFLAGS) -c proxy_cli_main.c -o proxy_cli_main.o
	$(CC) $(CFLAGS) proxy_parse.o proxy_cache_sqlite.o proxy_server_with_cli.o proxy_cli_main.o -o proxy_cli $(LDFLAGS)

clean:
	rm -f proxy proxy_cli *.o

tar:
	tar -cvzf proxy-server.tgz proxy_server_with_cache.c proxy_server_with_cli.c proxy_cache_sqlite.c proxy_cache_sqlite.h proxy_cli_main.c README Makefile proxy_parse.c proxy_parse.h
