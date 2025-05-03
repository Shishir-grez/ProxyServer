/**
 * proxy_cache_sqlite.h
 * SQLite-based persistent cache for the proxy server
 * Unix version
 */

#ifndef PROXY_CACHE_SQLITE_H
#define PROXY_CACHE_SQLITE_H

#include <time.h>

/**
 * Initialize the SQLite cache
 * @param db_path Path to the SQLite database file
 * @return 1 on success, 0 on failure
 */
int sqlite_cache_init(const char *db_path);

/**
 * Close the SQLite cache database
 */
void sqlite_cache_close(void);

/**
 * Store data in the SQLite cache
 * @param key The cache key
 * @param data The data to cache
 * @param size Size of the data
 * @param expiry Expiry time (unix timestamp)
 * @return 1 on success, 0 on failure
 */
int sqlite_cache_store(const char *key, const void *data, int size, time_t expiry);

/**
 * Retrieve data from the SQLite cache
 * @param key The cache key
 * @param data Pointer to store the retrieved data (will be allocated)
 * @param size Pointer to store the size of the retrieved data
 * @return 1 on success, 0 on failure
 */
int sqlite_cache_retrieve(const char *key, void **data, int *size);

/**
 * Clear all entries from the SQLite cache
 */
void sqlite_cache_clear(void);

/**
 * Check if the SQLite cache is enabled
 * @return 1 if enabled, 0 if disabled
 */
int sqlite_cache_is_enabled(void);

/**
 * Set whether the SQLite cache is enabled
 * @param enabled 1 to enable, 0 to disable
 */
void sqlite_cache_set_enabled(int enabled);

#endif /* PROXY_CACHE_SQLITE_H */
