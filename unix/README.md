# Multithreaded Proxy Server with Persistent Cache

This is a multithreaded proxy server with both in-memory and SQLite-based persistent caching capabilities. The proxy server supports HTTP requests and can cache responses to improve performance.

## Features

- Multithreaded request handling
- In-memory cache for fast response times
- SQLite-based persistent cache that survives server restarts
- Special handling for large and delayed responses
- Command-line interface (CLI) for controlling proxy server behavior
- Robust error handling and socket timeout management

## Requirements

- GCC compiler
- pthread library
- SQLite3 development libraries
- GNU Readline library

On Ubuntu/Debian, you can install the required dependencies with:

```bash
sudo apt-get install build-essential libsqlite3-dev libreadline-dev
```

## Building

To build the proxy server, simply run:

```bash
make
```

This will build both the standard proxy server (`proxy`) and the CLI version with persistent cache support (`proxy_cli`).

## Usage

### Standard Proxy Server

```bash
./proxy
```

The standard proxy server listens on port 8080 by default.

### CLI Version with Persistent Cache

```bash
./proxy_cli
```

This starts the command-line interface. Type `help` to see available commands:

```
proxy> help
Available commands:
  start      Start the proxy server: start [port]
  stop       Stop the proxy server
  status     Show proxy server status
  cache      Control in-memory cache: cache [on|off|clear]
  pcache     Control persistent cache: pcache [on|off|clear|init <path>]
  port       Set or show the port: port [number]
  exit       Exit the program
  help       Show this help message
```

#### Example Commands

- `start 8080` - Start the proxy server on port 8080
- `pcache on` - Enable persistent SQLite cache
- `pcache off` - Disable persistent SQLite cache
- `pcache clear` - Clear all entries from the persistent cache
- `pcache init /path/to/cache.db` - Use a specific SQLite database file
- `cache on` - Enable in-memory cache
- `cache off` - Disable in-memory cache
- `status` - Show current proxy server status
- `stop` - Stop the proxy server
- `exit` - Exit the program

## Implementation Details

### Caching System

The proxy server implements a two-tier caching system:

1. **SQLite Persistent Cache**: Stores responses in a SQLite database that persists across server restarts. The cache uses expiry times to ensure responses don't become stale.

2. **In-Memory Cache**: Provides fast access to recently requested resources without disk access.

When a request is received, the proxy first checks the SQLite cache, then the in-memory cache, and finally forwards the request to the origin server if needed.

### Special Request Handling

The proxy server includes special handling for certain types of requests:

- **Large Responses**: For URLs containing `/large`, the proxy allocates a larger buffer and sets a longer expiry time.
  
- **Delayed Responses**: For URLs containing `/delayed`, the proxy uses longer timeouts, more retry attempts, and extended expiry times.

### Error Recovery

The proxy implements robust error handling including:

- Socket timeout configuration
- Retry mechanisms with exponential backoff
- Partial response recovery
- Graceful handling of connection errors

## License

This project is open source and available under the MIT License.

## Author

Copyright (c) 2025, Your Name
