# Windows Port of Multithreaded Proxy Server

This directory contains the Windows port of the multithreaded proxy server with caching functionality. The code has been adapted from the Unix version to work with Windows networking APIs and threading model.

## Key Changes from Unix Version

1. **Networking API**: Replaced Unix socket APIs with Windows Winsock2 APIs
2. **Threading Model**: Replaced POSIX threads with Windows threads using `_beginthreadex`
3. **Synchronization**: Replaced POSIX semaphores and mutexes with Windows semaphores and critical sections
4. **Memory Management**: Adapted memory allocation and string handling for Windows
5. **Signal Handling**: Replaced Unix signal handling with Windows console control handlers

## Requirements

- Windows 7 or later
- Microsoft Visual C++ compiler (MSVC)
- Windows SDK

## Building the Proxy Server

### Using Visual Studio Command Prompt

1. Open a Visual Studio Command Prompt
2. Navigate to this directory
3. Run `nmake` to build the project

### Using Visual Studio IDE

1. Create a new Empty C/C++ Project
2. Add the source files to the project
3. Configure the project to link with `ws2_32.lib`
4. Build the project

## Running the Proxy Server

```
proxy_server.exe <port>
```

Where `<port>` is the port number you want the proxy server to listen on.

## Features

- HTTP proxy with caching capability
- Multithreaded to handle multiple client connections
- LRU cache eviction policy
- Support for HTTP/1.0 and HTTP/1.1

## Dependencies

- Windows Sockets 2 (Winsock2)
- Windows API for threading and synchronization

## Known Limitations

- HTTPS is not supported in this version
- Only supports HTTP/1.0 and HTTP/1.1
- Limited to IPv4 addresses
