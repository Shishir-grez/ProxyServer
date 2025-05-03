/**
 * proxy_cache_sqlite.c
 * SQLite-based persistent cache for the proxy server
 * Unix version
 */

#include "proxy_cache_sqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static sqlite3 *db = NULL;
static int sqlite_cache_enabled = 0;

/**
 * Log a message to stderr
 */
static void log_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

int sqlite_cache_init(const char *db_path) {
    if (db != NULL) {
        sqlite_cache_close();
    }
    
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        log_message("Cannot open SQLite database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        db = NULL;
        return 0;
    }
    
    // Create the cache table if it doesn't exist
    const char *create_table_sql = 
        "CREATE TABLE IF NOT EXISTS cache ("
        "key TEXT PRIMARY KEY, "
        "data BLOB, "
        "size INTEGER, "
        "expiry INTEGER)";
    
    char *err_msg = NULL;
    rc = sqlite3_exec(db, create_table_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        log_message("SQLite error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        db = NULL;
        return 0;
    }
    
    // Enable cache by default
    sqlite_cache_enabled = 1;
    
    log_message("SQLite cache initialized: %s\n", db_path);
    return 1;
}

void sqlite_cache_close(void) {
    if (db != NULL) {
        sqlite3_close(db);
        db = NULL;
        log_message("SQLite cache closed\n");
    }
}

int sqlite_cache_store(const char *key, const void *data, int size, time_t expiry) {
    if (db == NULL || !sqlite_cache_enabled) {
        return 0;
    }
    
    // Remove any existing entry with the same key
    sqlite_stmt *stmt;
    const char *delete_sql = "DELETE FROM cache WHERE key = ?";
    int rc = sqlite3_prepare_v2(db, delete_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_message("SQLite prepare error: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    // Insert the new entry
    const char *insert_sql = "INSERT INTO cache (key, data, size, expiry) VALUES (?, ?, ?, ?)";
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_message("SQLite prepare error: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, data, size, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, size);
    sqlite3_bind_int64(stmt, 4, expiry);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        log_message("SQLite error storing cache entry: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    
    return 1;
}

int sqlite_cache_retrieve(const char *key, void **data, int *size) {
    if (db == NULL || !sqlite_cache_enabled) {
        return 0;
    }
    
    // First, check if entry exists and is not expired
    sqlite3_stmt *stmt;
    const char *select_sql = "SELECT data, size FROM cache WHERE key = ? AND (expiry > ? OR expiry = 0)";
    int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_message("SQLite prepare error: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    
    time_t now = time(NULL);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        // No valid entry found
        sqlite3_finalize(stmt);
        return 0;
    }
    
    // Get the data
    const void *blob_data = sqlite3_column_blob(stmt, 0);
    int blob_size = sqlite3_column_int(stmt, 1);
    
    // Allocate memory for the data
    *data = malloc(blob_size);
    if (*data == NULL) {
        log_message("Memory allocation failed for cache retrieval\n");
        sqlite3_finalize(stmt);
        return 0;
    }
    
    // Copy the data
    memcpy(*data, blob_data, blob_size);
    *size = blob_size;
    
    sqlite3_finalize(stmt);
    return 1;
}

void sqlite_cache_clear(void) {
    if (db == NULL) {
        return;
    }
    
    const char *delete_sql = "DELETE FROM cache";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, delete_sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        log_message("SQLite error clearing cache: %s\n", err_msg);
        sqlite3_free(err_msg);
    } else {
        log_message("SQLite cache cleared\n");
    }
}

int sqlite_cache_is_enabled(void) {
    return sqlite_cache_enabled;
}

void sqlite_cache_set_enabled(int enabled) {
    sqlite_cache_enabled = enabled;
    log_message("SQLite cache %s\n", enabled ? "enabled" : "disabled");
}
