#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "proxy_parse.h"
#include "proxy_cli.h"
#include "proxy_cache_sqlite.h"

// Need to link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

// Global state
CLIState cli_state;

// Forward declarations
unsigned int __stdcall proxy_server_thread(void *arg);
extern int port_number; // From proxy_server_with_cli.c
extern SOCKET proxy_socketId; // From proxy_server_with_cli.c
extern volatile BOOL stop_flag; // From proxy_server_with_cli.c
extern int run_proxy_server(void); // From proxy_server_with_cli.c

// Proxy control function declarations
void proxy_set_cache_enabled(int enabled);
int proxy_get_cache_enabled(void);
int proxy_get_cache_size(void);
void proxy_clear_cache(void *map);
void proxy_set_verbose_mode(int enabled);
int proxy_get_verbose_mode(void);

// SQLite cache control function declarations
void proxy_init_sqlite_cache(const char *db_path);
void proxy_close_sqlite_cache(void);
int proxy_sqlite_cache_enabled(void);
void proxy_set_sqlite_cache_enabled(int enabled);
void proxy_sqlite_cache_clear(void);
int proxy_sqlite_cache_size(void);

// Statistics for CLI
extern int cache_size; // From proxy_server_with_cache.c
int total_connections = 0;
int active_connections = 0;
int cache_hits = 0;
int cache_misses = 0;
CRITICAL_SECTION stats_lock;

// Print help information for all commands
int cmd_help(int argc, char **argv) {
    extern Command commands[];
    extern int command_count;
    
    cli_printf(CLI_COLOR_CYAN, "\nAvailable commands:\n\n");
    
    for (int i = 0; i < command_count; i++) {
        cli_printf(CLI_COLOR_GREEN, "  %-10s", commands[i].name);
        cli_printf(CLI_COLOR_DEFAULT, "- %s\n", commands[i].description);
        cli_printf(CLI_COLOR_BLUE, "    Usage: %s\n\n", commands[i].usage);
    }
    
    return 0;
}

// Start the proxy server
int cmd_start(int argc, char **argv) {
    int port = cli_state.proxy_port;
    
    if (cli_state.proxy_running) {
        cli_set_error(&cli_state, "Proxy server is already running");
        return 1;
    }
    
    // Parse port argument if provided
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            cli_set_error(&cli_state, "Invalid port number. Must be between 1 and 65535");
            return 1;
        }
        cli_state.proxy_port = port;
    }
    
    // Set the port number for the proxy server
    port_number = port;
    
    // Start the proxy server in a new thread
    cli_state.proxy_thread = (HANDLE)_beginthreadex(NULL, 0, proxy_server_thread, NULL, 0, NULL);
    if (cli_state.proxy_thread == NULL) {
        cli_set_error(&cli_state, "Failed to create proxy server thread");
        return 1;
    }
    
    cli_state.proxy_running = 1;
    cli_printf(CLI_COLOR_GREEN, "Proxy server started on port %d\n", port);
    
    return 0;
}

// Stop the proxy server
int cmd_stop(int argc, char **argv) {
    if (!cli_state.proxy_running) {
        cli_set_error(&cli_state, "Proxy server is not running");
        return 1;
    }
    
    // Signal the proxy server to stop
    stop_flag = TRUE;
    shutdown(proxy_socketId, SD_BOTH);
    
    // Wait for the thread to terminate with a timeout
    DWORD result = WaitForSingleObject(cli_state.proxy_thread, 5000);
    if (result == WAIT_TIMEOUT) {
        cli_printf(CLI_COLOR_YELLOW, "Proxy server did not terminate gracefully, forcing...\n");
        TerminateThread(cli_state.proxy_thread, 1);
    }
    
    CloseHandle(cli_state.proxy_thread);
    cli_state.proxy_thread = NULL;
    cli_state.proxy_running = 0;
    
    cli_printf(CLI_COLOR_GREEN, "Proxy server stopped\n");
    
    return 0;
}

// Show proxy server status and statistics
int cmd_status(int argc, char **argv) {
    time_t current_time;
    char time_str[100];
    
    time(&current_time);
    ctime_s(time_str, sizeof(time_str), &current_time);
    
    cli_printf(CLI_COLOR_CYAN, "\n╔══════════════════════════════════════════════════╗\n");
    cli_printf(CLI_COLOR_CYAN, "║              PROXY SERVER STATUS                 ║\n");
    cli_printf(CLI_COLOR_CYAN, "╠══════════════════════════════════════════════════╣\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Status: ");
    if (cli_state.proxy_running) {
        cli_printf(CLI_COLOR_GREEN, "Running");
    } else {
        cli_printf(CLI_COLOR_RED, "Stopped");
    }
    cli_printf(CLI_COLOR_CYAN, "                                  ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Port: %d", cli_state.proxy_port);
    cli_printf(CLI_COLOR_CYAN, "                                    ║\n");
    
    EnterCriticalSection(&stats_lock);
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "In-memory Cache: ");
    if (cli_state.cache_enabled) {
        cli_printf(CLI_COLOR_GREEN, "Enabled");
    } else {
        cli_printf(CLI_COLOR_RED, "Disabled");
    }
    cli_printf(CLI_COLOR_CYAN, "                      ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "In-memory Size: %d KB", cache_size / 1024);
    cli_printf(CLI_COLOR_CYAN, "                        ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "SQLite Cache: ");
    if (proxy_sqlite_cache_enabled()) {
        cli_printf(CLI_COLOR_GREEN, "Enabled");
    } else {
        cli_printf(CLI_COLOR_RED, "Disabled");
    }
    cli_printf(CLI_COLOR_CYAN, "                         ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "SQLite Size: %d KB", proxy_sqlite_cache_size() / 1024);
    cli_printf(CLI_COLOR_CYAN, "                          ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Cache Hits: %d", cache_hits);
    cli_printf(CLI_COLOR_CYAN, "                                ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Cache Misses: %d", cache_misses);
    cli_printf(CLI_COLOR_CYAN, "                              ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Hit Ratio: ");
    if (cache_hits + cache_misses > 0) {
        cli_printf(CLI_COLOR_WHITE, "%.2f%%", (float)cache_hits / (cache_hits + cache_misses) * 100);
    } else {
        cli_printf(CLI_COLOR_WHITE, "N/A");
    }
    cli_printf(CLI_COLOR_CYAN, "                               ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Total Connections: %d", total_connections);
    cli_printf(CLI_COLOR_CYAN, "                        ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Active Connections: %d", active_connections);
    cli_printf(CLI_COLOR_CYAN, "                       ║\n");
    
    LeaveCriticalSection(&stats_lock);
    
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "Time: %s", time_str);
    cli_printf(CLI_COLOR_CYAN, "      ║\n");
    
    cli_printf(CLI_COLOR_CYAN, "╚══════════════════════════════════════════════════╝\n\n");
    
    return 0;
}

// Set the cache mode (enable/disable)
int cmd_cache(int argc, char **argv) {
    if (argc < 2) {
        cli_printf(CLI_COLOR_WHITE, "Cache is currently ");
        if (proxy_get_cache_enabled()) {
            cli_printf(CLI_COLOR_GREEN, "enabled\n");
        } else {
            cli_printf(CLI_COLOR_RED, "disabled\n");
        }
        return 0;
    }
    
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "enable") == 0) {
        proxy_set_cache_enabled(1);
        cli_state.cache_enabled = 1;
        cli_printf(CLI_COLOR_GREEN, "Cache enabled\n");
    } else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "disable") == 0) {
        proxy_set_cache_enabled(0);
        cli_state.cache_enabled = 0;
        cli_printf(CLI_COLOR_GREEN, "Cache disabled\n");
    } else if (strcmp(argv[1], "clear") == 0 || strcmp(argv[1], "flush") == 0) {
        // Call the cache clearing function with NULL (it will use the global map)
        proxy_clear_cache(NULL);
        cli_printf(CLI_COLOR_GREEN, "Cache cleared\n");
    } else {
        cli_set_error(&cli_state, "Invalid cache command. Use 'on', 'off', or 'clear'");
        return 1;
    }
    
    return 0;
}

// Set the verbosity level
int cmd_verbose(int argc, char **argv) {
    if (argc < 2) {
        cli_printf(CLI_COLOR_WHITE, "Verbose mode is currently ");
        if (proxy_get_verbose_mode()) {
            cli_printf(CLI_COLOR_GREEN, "enabled\n");
        } else {
            cli_printf(CLI_COLOR_RED, "disabled\n");
        }
        return 0;
    }
    
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "enable") == 0) {
        proxy_set_verbose_mode(1);
        cli_state.verbose = 1;
        cli_printf(CLI_COLOR_GREEN, "Verbose mode enabled\n");
    } else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "disable") == 0) {
        proxy_set_verbose_mode(0);
        cli_state.verbose = 0;
        cli_printf(CLI_COLOR_GREEN, "Verbose mode disabled\n");
    } else {
        cli_set_error(&cli_state, "Invalid verbose command. Use 'on' or 'off'");
        return 1;
    }
    
    return 0;
}

// Change the port number
int cmd_port(int argc, char **argv) {
    if (argc < 2) {
        cli_printf(CLI_COLOR_WHITE, "Current port: %d\n", cli_state.proxy_port);
        return 0;
    }
    
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        cli_set_error(&cli_state, "Invalid port number. Must be between 1 and 65535");
        return 1;
    }
    
    if (cli_state.proxy_running) {
        cli_printf(CLI_COLOR_YELLOW, "Warning: Port change will take effect after restart\n");
    }
    
    cli_state.proxy_port = port;
    cli_printf(CLI_COLOR_GREEN, "Port set to %d\n", port);
    
    return 0;
}

// Exit the CLI
int cmd_exit(int argc, char **argv) {
    if (cli_state.proxy_running) {
        cli_printf(CLI_COLOR_YELLOW, "Stopping proxy server before exit...\n");
        cmd_stop(0, NULL);
    }
    
    // Close SQLite cache before exiting
    proxy_close_sqlite_cache();
    
    cli_state.running = 0;
    cli_printf(CLI_COLOR_GREEN, "Exiting CLI. Goodbye!\n");
    
    return 0;
}

// Manage persistent SQLite cache
int cmd_pcache(int argc, char **argv) {
    if (argc < 2) {
        cli_printf(CLI_COLOR_WHITE, "Persistent cache is currently ");
        if (proxy_sqlite_cache_enabled()) {
            cli_printf(CLI_COLOR_GREEN, "enabled\n");
        } else {
            cli_printf(CLI_COLOR_RED, "disabled\n");
        }
        cli_printf(CLI_COLOR_WHITE, "Persistent cache size: %d KB\n", proxy_sqlite_cache_size() / 1024);
        return 0;
    }
    
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "enable") == 0) {
        proxy_set_sqlite_cache_enabled(1);
        cli_printf(CLI_COLOR_GREEN, "Persistent cache enabled\n");
    } else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "disable") == 0) {
        proxy_set_sqlite_cache_enabled(0);
        cli_printf(CLI_COLOR_GREEN, "Persistent cache disabled\n");
    } else if (strcmp(argv[1], "clear") == 0) {
        proxy_sqlite_cache_clear();
        cli_printf(CLI_COLOR_GREEN, "Persistent cache cleared\n");
    } else if (strcmp(argv[1], "init") == 0) {
        if (argc < 3) {
            cli_printf(CLI_COLOR_RED, "Please specify a database path\n");
            return 1;
        }
        proxy_init_sqlite_cache(argv[2]);
        cli_printf(CLI_COLOR_GREEN, "Persistent cache initialized at %s\n", argv[2]);
    } else {
        cli_set_error(&cli_state, "Invalid pcache command. Use 'on', 'off', 'clear', or 'init <path>'");
        return 1;
    }
    
    return 0;
}

// Define the available commands
Command commands[] = {
    {
        "help",
        "Show help information",
        "help",
        cmd_help
    },
    {
        "start",
        "Start the proxy server",
        "start [port]",
        cmd_start
    },
    {
        "stop",
        "Stop the proxy server",
        "stop",
        cmd_stop
    },
    {
        "status",
        "Show proxy server status and statistics",
        "status",
        cmd_status
    },
    {
        "cache",
        "Control the cache (on/off/clear)",
        "cache [on|off|clear]",
        cmd_cache
    },
    {
        "verbose",
        "Set verbose mode",
        "verbose [on|off]",
        cmd_verbose
    },
    {
        "port",
        "Set the port number",
        "port <number>",
        cmd_port
    },
    {
        "pcache",
        "Manage persistent SQLite cache",
        "pcache [on|off|clear|init <path>]",
        cmd_pcache
    },
    {
        "exit",
        "Exit the CLI",
        "exit",
        cmd_exit
    }
};

int command_count = sizeof(commands) / sizeof(Command);

// Update stats from proxy server code
void update_stats(int is_cache_hit, int is_connection_added, int is_connection_removed) {
    EnterCriticalSection(&stats_lock);
    
    if (is_cache_hit) {
        cache_hits++;
    } else if (is_cache_hit == 0) { // Explicitly not a hit (as opposed to not relevant)
        cache_misses++;
    }
    
    if (is_connection_added) {
        total_connections++;
        active_connections++;
    }
    
    if (is_connection_removed) {
        active_connections--;
        if (active_connections < 0) active_connections = 0;
    }
    
    LeaveCriticalSection(&stats_lock);
}

// Thread function for the proxy server
unsigned int __stdcall proxy_server_thread(void *arg) {
    extern int run_proxy_server(void); // Defined in proxy_server_with_cache.c
    
    // Run the proxy server
    int result = run_proxy_server();
    
    // Update the CLI state
    EnterCriticalSection(&cli_state.lock);
    cli_state.proxy_running = 0;
    cli_state.proxy_thread = NULL;
    LeaveCriticalSection(&cli_state.lock);
    
    return result;
}

// Main function
int main(int argc, char *argv[]) {
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;
    
    // Initialize Winsock
    wVersionRequested = MAKEWORD(2, 2);
    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        printf("WSAStartup failed with error: %d\n", err);
        return 1;
    }
    
    // Initialize the CLI
    cli_init(&cli_state);
    
    // Initialize critical section for stats
    InitializeCriticalSection(&stats_lock);
    
    // Initialize SQLite cache with default database file
    proxy_init_sqlite_cache("proxy_cache.db");
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                cli_state.proxy_port = atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cli_state.verbose = 1;
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            cli_state.cache_enabled = 0;
        } else if (strcmp(argv[i], "--auto-start") == 0) {
            // Auto-start the proxy after CLI initialization
            cmd_start(1, NULL);
        }
    }
    
    // Banner
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║                                                  ║\n");
    printf("║          Multithreaded Proxy Server CLI           ║\n");
    printf("║                                                  ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
    printf("Type 'help' to see available commands\n\n");
    
    // Run the CLI
    cli_run(&cli_state, commands, command_count);
    
    // Clean up
    if (cli_state.proxy_running) {
        cmd_stop(0, NULL);
    }
    
    proxy_close_sqlite_cache();
    cli_cleanup(&cli_state);
    DeleteCriticalSection(&stats_lock);
    WSACleanup();
    
    return 0;
}
