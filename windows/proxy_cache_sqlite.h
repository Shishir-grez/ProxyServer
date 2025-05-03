#ifndef PROXY_CACHE_SQLITE_H
#define PROXY_CACHE_SQLITE_H

#include <windows.h>
#include <time.h>

// Function to initialize the SQLite cache
int init_sqlite_cache(const char *db_path);

// Function to close the SQLite cache
void close_sqlite_cache(void);

// Function to store a response in the SQLite cache
int sqlite_cache_store(const char *key, const void *data, int size, time_t expiry);

// Function to retrieve a response from the SQLite cache
int sqlite_cache_retrieve(const char *key, void **data, int *size);

// Function to check if a key exists in the SQLite cache
int sqlite_cache_exists(const char *key);

// Function to remove a key from the SQLite cache
int sqlite_cache_remove(const char *key);

// Function to clear the entire SQLite cache
int sqlite_cache_clear(void);

// Function to get current SQLite cache size in bytes
int sqlite_cache_size(void);

// Function to get cache hit statistics
void sqlite_cache_stats(int *hits, int *misses);

// Function to remove expired entries from the cache
int sqlite_cache_clean_expired(void);

// Function to set the maximum cache size in bytes
void sqlite_cache_set_max_size(size_t max_size);

// Function to enable or disable the SQLite cache
void sqlite_cache_set_enabled(int enabled);

// Function to get whether the SQLite cache is enabled
int sqlite_cache_is_enabled(void);

#endif /* PROXY_CACHE_SQLITE_H */
