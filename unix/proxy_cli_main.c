#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "proxy_parse.h"
#include "proxy_cache_sqlite.h"

// Function prototypes
void start_proxy_server(int port);
void stop_proxy_server(void);
int proxy_is_running(void);
void proxy_set_cache_enabled(int enabled);
int proxy_get_cache_enabled(void);
void proxy_set_sqlite_cache_enabled(int enabled);
int proxy_get_sqlite_cache_enabled(void);
void proxy_sqlite_cache_clear(void);
void proxy_set_port(int port);
int proxy_get_port(void);

// Command functions
int cmd_start(int argc, char *argv[]);
int cmd_stop(int argc, char *argv[]);
int cmd_status(int argc, char *argv[]);
int cmd_cache(int argc, char *argv[]);
int cmd_pcache(int argc, char *argv[]);
int cmd_port(int argc, char *argv[]);
int cmd_exit(int argc, char *argv[]);
int cmd_help(int argc, char *argv[]);

// Command definition structure
typedef struct {
    const char *name;
    int (*func)(int argc, char *argv[]);
    const char *help;
} Command;

// Available commands
Command commands[] = {
    {"start", cmd_start, "Start the proxy server: start [port]"},
    {"stop", cmd_stop, "Stop the proxy server"},
    {"status", cmd_status, "Show proxy server status"},
    {"cache", cmd_cache, "Control in-memory cache: cache [on|off|clear]"},
    {"pcache", cmd_pcache, "Control persistent cache: pcache [on|off|clear|init <path>]"},
    {"port", cmd_port, "Set or show the port: port [number]"},
    {"exit", cmd_exit, "Exit the program"},
    {"help", cmd_help, "Show this help message"},
    {NULL, NULL, NULL}
};

// Global flag to indicate if we're exiting
volatile int exiting = 0;

// Signal handler for SIGINT
void handle_sigint(int sig) {
    printf("\nUse 'exit' to quit the program.\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

int main(int argc, char *argv[]) {
    char *input, *tmp;
    char *cmd_argv[32];
    int cmd_argc;
    
    // Set up signal handling
    signal(SIGINT, handle_sigint);
    
    // Initialize readline
    using_history();
    
    // Initialize the SQLite cache with default database
    sqlite_cache_init("proxy_cache.db");
    
    printf("Proxy Server CLI\n");
    printf("Type 'help' for a list of commands.\n");
    
    while (!exiting) {
        // Display prompt and get input
        input = readline("proxy> ");
        
        // Check for EOF
        if (!input) {
            printf("\n");
            break;
        }
        
        // Skip empty lines
        if (input[0] == '\0') {
            free(input);
            continue;
        }
        
        // Add to history
        add_history(input);
        
        // Parse input into command and arguments
        cmd_argc = 0;
        tmp = strtok(input, " \t");
        while (tmp && cmd_argc < 31) {
            cmd_argv[cmd_argc++] = tmp;
            tmp = strtok(NULL, " \t");
        }
        cmd_argv[cmd_argc] = NULL;
        
        // Find and execute command
        int found = 0;
        for (Command *cmd = commands; cmd->name; cmd++) {
            if (strcmp(cmd->name, cmd_argv[0]) == 0) {
                cmd->func(cmd_argc, cmd_argv);
                found = 1;
                break;
            }
        }
        
        if (!found) {
            printf("Unknown command: %s\n", cmd_argv[0]);
            printf("Type 'help' for a list of commands.\n");
        }
        
        free(input);
    }
    
    // Clean up
    if (proxy_is_running()) {
        stop_proxy_server();
    }
    
    // Close the SQLite cache
    sqlite_cache_close();
    
    clear_history();
    
    return 0;
}

// Start command
int cmd_start(int argc, char *argv[]) {
    int port = proxy_get_port();
    
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            printf("Invalid port number. Using default port %d.\n", proxy_get_port());
            port = proxy_get_port();
        } else {
            proxy_set_port(port);
        }
    }
    
    if (proxy_is_running()) {
        printf("Proxy server is already running on port %d.\n", proxy_get_port());
        return 0;
    }
    
    start_proxy_server(port);
    printf("Proxy server started on port %d.\n", port);
    return 0;
}

// Stop command
int cmd_stop(int argc, char *argv[]) {
    if (!proxy_is_running()) {
        printf("Proxy server is not running.\n");
        return 0;
    }
    
    stop_proxy_server();
    printf("Proxy server stopped.\n");
    return 0;
}

// Status command
int cmd_status(int argc, char *argv[]) {
    if (proxy_is_running()) {
        printf("Proxy server is running on port %d.\n", proxy_get_port());
        printf("In-memory cache is %s.\n", proxy_get_cache_enabled() ? "enabled" : "disabled");
        printf("Persistent SQLite cache is %s.\n", sqlite_cache_is_enabled() ? "enabled" : "disabled");
    } else {
        printf("Proxy server is not running.\n");
    }
    return 0;
}

// Cache command
int cmd_cache(int argc, char *argv[]) {
    if (argc < 2) {
        printf("In-memory cache is %s.\n", proxy_get_cache_enabled() ? "enabled" : "disabled");
        return 0;
    }
    
    if (strcmp(argv[1], "on") == 0) {
        proxy_set_cache_enabled(1);
        printf("In-memory cache enabled.\n");
    } else if (strcmp(argv[1], "off") == 0) {
        proxy_set_cache_enabled(0);
        printf("In-memory cache disabled.\n");
    } else if (strcmp(argv[1], "clear") == 0) {
        // Clear the in-memory cache
        if (proxy_is_running()) {
            // TODO: Implement cache clearing in the proxy server
            printf("In-memory cache cleared.\n");
        } else {
            printf("Proxy server is not running.\n");
        }
    } else {
        printf("Usage: cache [on|off|clear]\n");
    }
    
    return 0;
}

// Persistent Cache command
int cmd_pcache(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Persistent SQLite cache is %s.\n", sqlite_cache_is_enabled() ? "enabled" : "disabled");
        return 0;
    }
    
    if (strcmp(argv[1], "on") == 0) {
        proxy_set_sqlite_cache_enabled(1);
        printf("Persistent SQLite cache enabled.\n");
    } else if (strcmp(argv[1], "off") == 0) {
        proxy_set_sqlite_cache_enabled(0);
        printf("Persistent SQLite cache disabled.\n");
    } else if (strcmp(argv[1], "clear") == 0) {
        proxy_sqlite_cache_clear();
        printf("Persistent SQLite cache cleared.\n");
    } else if (strcmp(argv[1], "init") == 0 && argc > 2) {
        if (sqlite_cache_init(argv[2])) {
            printf("Persistent SQLite cache initialized with database: %s\n", argv[2]);
        } else {
            printf("Failed to initialize persistent cache with database: %s\n", argv[2]);
        }
    } else {
        printf("Usage: pcache [on|off|clear|init <path>]\n");
    }
    
    return 0;
}

// Port command
int cmd_port(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Current port: %d\n", proxy_get_port());
        return 0;
    }
    
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        printf("Invalid port number. Valid range is 1-65535.\n");
        return 1;
    }
    
    if (proxy_is_running()) {
        printf("Cannot change port while proxy server is running. Stop the server first.\n");
        return 1;
    }
    
    proxy_set_port(port);
    printf("Port set to %d.\n", port);
    return 0;
}

// Exit command
int cmd_exit(int argc, char *argv[]) {
    if (proxy_is_running()) {
        printf("Stopping proxy server...\n");
        stop_proxy_server();
    }
    printf("Exiting...\n");
    exiting = 1;
    return 0;
}

// Help command
int cmd_help(int argc, char *argv[]) {
    printf("Available commands:\n");
    for (Command *cmd = commands; cmd->name; cmd++) {
        printf("  %-10s %s\n", cmd->name, cmd->help);
    }
    return 0;
}

// These functions are implemented in proxy_server_with_cli.c
void proxy_set_sqlite_cache_enabled(int enabled) {
    sqlite_cache_set_enabled(enabled);
}

void proxy_sqlite_cache_clear(void) {
    sqlite_cache_clear();
}
