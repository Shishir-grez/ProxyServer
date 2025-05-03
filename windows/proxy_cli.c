#include "proxy_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdarg.h>

// Maximum input line length
#define MAX_INPUT_LENGTH 1024

// CLI history management
#define MAX_HISTORY 50
static char *history[MAX_HISTORY];
static int history_count = 0;
static int history_index = 0;

// Initialize the CLI
void cli_init(CLIState *state) {
    state->running = 1;
    state->proxy_running = 0;
    state->proxy_thread = NULL;
    state->proxy_port = 8080; // Default port
    state->verbose = 0;
    state->cache_enabled = 1; // Cache enabled by default
    state->cache_size = 0;
    state->cache_hits = 0;
    state->cache_misses = 0;
    state->connections = 0;
    state->last_error[0] = '\0';
    
    InitializeCriticalSection(&state->lock);
    
    // Set console code page to UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    // Set up console for color output
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hConsole, &mode);
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hConsole, mode);
}

// Clean up CLI resources
void cli_cleanup(CLIState *state) {
    // Free history
    for (int i = 0; i < history_count; i++) {
        free(history[i]);
    }
    
    DeleteCriticalSection(&state->lock);
}

// Print with color
void cli_printf(int color, const char *format, ...) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
    WORD saved_attributes;
    
    // Save current attributes
    GetConsoleScreenBufferInfo(hConsole, &consoleInfo);
    saved_attributes = consoleInfo.wAttributes;
    
    // Set the color
    SetConsoleTextAttribute(hConsole, color);
    
    // Print the message
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    // Restore original attributes
    SetConsoleTextAttribute(hConsole, saved_attributes);
}

// Set an error message
void cli_set_error(CLIState *state, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(state->last_error, sizeof(state->last_error), format, args);
    va_end(args);
    
    // Print the error in red if verbose is enabled
    if (state->verbose) {
        cli_printf(CLI_COLOR_RED, "ERROR: %s\n", state->last_error);
    }
}

// Add command to history
static void add_to_history(const char *cmd) {
    // Don't add empty commands or duplicates of the last command
    if (!cmd[0] || (history_count > 0 && strcmp(history[history_count-1], cmd) == 0)) {
        return;
    }
    
    if (history_count < MAX_HISTORY) {
        history[history_count++] = _strdup(cmd);
    } else {
        // Shift history and add at the end
        free(history[0]);
        memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof(char*));
        history[MAX_HISTORY - 1] = _strdup(cmd);
    }
    
    history_index = history_count;
}

// Get line with arrow key navigation
static char *get_line_with_history(char *prompt) {
    static char line[MAX_INPUT_LENGTH];
    int pos = 0;
    int ch;
    int escape_sequence = 0;
    
    memset(line, 0, sizeof(line));
    printf("%s", prompt);
    
    while (1) {
        ch = _getch();
        
        // Handle arrow keys (escape sequences)
        if (ch == 224) { // Special key prefix
            escape_sequence = 1;
            continue;
        }
        
        if (escape_sequence) {
            escape_sequence = 0;
            
            switch (ch) {
                case 72: // Up arrow
                    if (history_index > 0) {
                        history_index--;
                        // Clear current line
                        while (pos > 0) {
                            printf("\b \b");
                            pos--;
                        }
                        // Print history item
                        strcpy(line, history[history_index]);
                        printf("%s", line);
                        pos = strlen(line);
                    }
                    break;
                    
                case 80: // Down arrow
                    if (history_index < history_count) {
                        history_index++;
                        // Clear current line
                        while (pos > 0) {
                            printf("\b \b");
                            pos--;
                        }
                        
                        if (history_index < history_count) {
                            // Print history item
                            strcpy(line, history[history_index]);
                            printf("%s", line);
                            pos = strlen(line);
                        } else {
                            // Empty line at the end of history
                            line[0] = '\0';
                            pos = 0;
                        }
                    }
                    break;
                    
                case 75: // Left arrow
                    if (pos > 0) {
                        pos--;
                        printf("\b");
                    }
                    break;
                    
                case 77: // Right arrow
                    if (pos < strlen(line)) {
                        printf("%c", line[pos]);
                        pos++;
                    }
                    break;
            }
            continue;
        }
        
        // Handle regular keys
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            break;
        } else if (ch == '\b' || ch == 127) { // Backspace
            if (pos > 0) {
                pos--;
                // Shift all characters left
                memmove(line + pos, line + pos + 1, strlen(line) - pos);
                // Print the updated line
                printf("\b%s \b", line + pos);
                // Move cursor back to position
                for (int i = 0; i < strlen(line) - pos; i++) {
                    printf("\b");
                }
            }
        } else if (ch == 3) { // Ctrl+C
            printf("^C\n");
            line[0] = '\0';
            return line;
        } else if (ch == 4) { // Ctrl+D
            if (pos == 0) {
                return NULL; // EOF
            }
        } else if (ch == 9) { // Tab - could implement tab completion here
            // Placeholder for tab completion
        } else if (ch >= 32 && ch < 127) { // Printable characters
            if (pos < MAX_INPUT_LENGTH - 1) {
                // Make room for the new character
                memmove(line + pos + 1, line + pos, strlen(line) - pos + 1);
                line[pos] = ch;
                pos++;
                
                // Print the updated line
                printf("%s", line + pos - 1);
                // Move cursor back to position
                for (int i = 0; i < strlen(line) - pos; i++) {
                    printf("\b");
                }
            }
        }
    }
    
    add_to_history(line);
    return line;
}

// Parse a command line into arguments
static int parse_args(char *line, char **argv) {
    int argc = 0;
    char *token;
    char *context;
    
    token = strtok_s(line, " \t\n", &context);
    while (token && argc < 64) {
        argv[argc++] = token;
        token = strtok_s(NULL, " \t\n", &context);
    }
    
    argv[argc] = NULL;
    return argc;
}

// Display CLI prompt with status
static void display_prompt(CLIState *state) {
    if (state->proxy_running) {
        cli_printf(CLI_COLOR_GREEN, "proxy");
        cli_printf(CLI_COLOR_DEFAULT, ":%d> ", state->proxy_port);
    } else {
        cli_printf(CLI_COLOR_RED, "proxy");
        cli_printf(CLI_COLOR_DEFAULT, "> ");
    }
}

// Find a command by name
static Command *find_command(Command *commands, int command_count, const char *name) {
    for (int i = 0; i < command_count; i++) {
        if (strcmp(commands[i].name, name) == 0) {
            return &commands[i];
        }
    }
    return NULL;
}

// Run the CLI main loop
void cli_run(CLIState *state, Command *commands, int command_count) {
    char *cmd_line;
    char *args[65]; // Max 64 arguments + NULL
    int argc;
    
    // Welcome message
    cli_printf(CLI_COLOR_CYAN, "\n╔══════════════════════════════════════════════════╗\n");
    cli_printf(CLI_COLOR_CYAN, "║                                                  ║\n");
    cli_printf(CLI_COLOR_CYAN, "║  ");
    cli_printf(CLI_COLOR_WHITE, "        Multithreaded Proxy Server CLI         ");
    cli_printf(CLI_COLOR_CYAN, "  ║\n");
    cli_printf(CLI_COLOR_CYAN, "║                                                  ║\n");
    cli_printf(CLI_COLOR_CYAN, "╚══════════════════════════════════════════════════╝\n\n");
    cli_printf(CLI_COLOR_DEFAULT, "Type '");
    cli_printf(CLI_COLOR_GREEN, "help");
    cli_printf(CLI_COLOR_DEFAULT, "' to see available commands\n\n");
    
    while (state->running) {
        display_prompt(state);
        
        // Get input with history support
        cmd_line = get_line_with_history("");
        if (!cmd_line || !*cmd_line) {
            continue; // Skip empty lines
        }
        
        // Make a copy of the line for parsing
        char *line_copy = _strdup(cmd_line);
        if (!line_copy) {
            cli_printf(CLI_COLOR_RED, "Memory allocation error\n");
            continue;
        }
        
        // Parse the command line
        argc = parse_args(line_copy, args);
        if (argc == 0) {
            free(line_copy);
            continue;
        }
        
        // Find and execute the command
        Command *command = find_command(commands, command_count, args[0]);
        if (command) {
            int result = command->func(argc, args);
            if (result != 0 && state->verbose) {
                cli_printf(CLI_COLOR_RED, "Command failed with code %d: %s\n", 
                          result, state->last_error);
            }
        } else {
            cli_printf(CLI_COLOR_RED, "Unknown command: %s\n", args[0]);
            cli_printf(CLI_COLOR_DEFAULT, "Type '");
            cli_printf(CLI_COLOR_GREEN, "help");
            cli_printf(CLI_COLOR_DEFAULT, "' to see available commands\n");
        }
        
        free(line_copy);
    }
}
