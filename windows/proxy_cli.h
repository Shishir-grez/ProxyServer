#ifndef PROXY_CLI_H
#define PROXY_CLI_H

#include <windows.h>

// CLI Command callback function type
typedef int (*CommandFunc)(int argc, char **argv);

// Structure to hold command information
typedef struct {
    const char *name;        // Command name
    const char *description; // Command description
    const char *usage;       // Usage example
    CommandFunc func;        // Function to execute
} Command;

// CLI state
typedef struct {
    int running;            // Is the CLI running
    int proxy_running;      // Is the proxy server running
    HANDLE proxy_thread;    // Handle to proxy server thread
    int proxy_port;         // Port the proxy is running on
    CRITICAL_SECTION lock;  // Lock for thread safety
    char last_error[256];   // Last error message
    int verbose;            // Verbose output flag
    int cache_enabled;      // Cache enabled flag
    int cache_size;         // Current cache size
    int cache_hits;         // Number of cache hits
    int cache_misses;       // Number of cache misses
    int connections;        // Number of connections
} CLIState;

// Initialize the CLI
void cli_init(CLIState *state);

// Run the CLI main loop
void cli_run(CLIState *state, Command *commands, int command_count);

// Clean up CLI resources
void cli_cleanup(CLIState *state);

// Print to CLI with color support
void cli_printf(int color, const char *format, ...);

// Color constants
#define CLI_COLOR_DEFAULT 7
#define CLI_COLOR_RED     12
#define CLI_COLOR_GREEN   10
#define CLI_COLOR_YELLOW  14
#define CLI_COLOR_BLUE    9
#define CLI_COLOR_MAGENTA 13
#define CLI_COLOR_CYAN    11
#define CLI_COLOR_WHITE   15

// Sets an error message in the CLI state
void cli_set_error(CLIState *state, const char *format, ...);

#endif /* PROXY_CLI_H */
