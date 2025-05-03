#include "proxy_cache_sqlite.h"
#include "lib/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SQLite database handle
static sqlite3 *db = NULL;

// Cache settings
static int cache_enabled = 1;
static size_t max_cache_size = 200 * (1 << 20); // 200 MB by default

// Cache statistics
static int cache_hits = 0;
static int cache_misses = 0;

// Helper function to execute a simple SQL query
static int exec_sql(const char *sql) {
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }
    
    return 1;
}

// Initialize the SQLite cache
int init_sqlite_cache(const char *db_path) {
    int rc;
    
    // Open the SQLite database
    rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    
    // Create the cache table if it doesn't exist
    const char *create_table_sql = 
        "CREATE TABLE IF NOT EXISTS cache ("
        "key TEXT PRIMARY KEY,"
        "data BLOB,"
        "size INTEGER,"
        "created INTEGER,"
        "expires INTEGER,"
        "last_accessed INTEGER"
        ");";
    
    // Create the stats table if it doesn't exist
    const char *create_stats_sql = 
        "CREATE TABLE IF NOT EXISTS stats ("
        "hits INTEGER DEFAULT 0,"
        "misses INTEGER DEFAULT 0"
        ");";
    
    // Create an index on expires for faster expiration cleanup
    const char *create_index_sql = 
        "CREATE INDEX IF NOT EXISTS idx_expires ON cache(expires);";
    
    if (!exec_sql(create_table_sql) || 
        !exec_sql(create_stats_sql) || 
        !exec_sql(create_index_sql)) {
        sqlite3_close(db);
        return 0;
    }
    
    // Initialize stats if needed
    const char *init_stats_sql = 
        "INSERT OR IGNORE INTO stats VALUES (0, 0);";
    
    if (!exec_sql(init_stats_sql)) {
        sqlite3_close(db);
        return 0;
    }
    
    // Load cache statistics
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "SELECT hits, misses FROM stats", -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        cache_hits = sqlite3_column_int(stmt, 0);
        cache_misses = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    
    // Clean expired entries on startup
    sqlite_cache_clean_expired();
    
    // Enforce max cache size on startup
    if (max_cache_size > 0) {
        const char *enforce_size_sql = 
            "DELETE FROM cache WHERE key IN ("
            "  SELECT key FROM cache"
            "  ORDER BY last_accessed ASC"
            "  LIMIT (SELECT COUNT(*) FROM cache) - ("
            "    SELECT MAX(0, (? - (SELECT SUM(size) FROM cache)) / "
            "    (SELECT AVG(size) FROM cache))"
            "  )"
            ");";
        
        rc = sqlite3_prepare_v2(db, enforce_size_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, max_cache_size);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
    
    return 1;
}

// Close the SQLite cache
void close_sqlite_cache(void) {
    if (db) {
        // Update stats before closing
        char sql[100];
        snprintf(sql, sizeof(sql), "UPDATE stats SET hits = %d, misses = %d", 
                 cache_hits, cache_misses);
        exec_sql(sql);
        
        sqlite3_close(db);
        db = NULL;
    }
}

// Store a response in the SQLite cache
int sqlite_cache_store(const char *key, const void *data, int size, time_t expiry) {
    if (!db || !cache_enabled || size <= 0 || size > max_cache_size) {
        return 0;
    }
    
    sqlite3_stmt *stmt;
    int rc;
    
    // Check if we need to make room for this entry
    int current_size = sqlite_cache_size();
    if (current_size + size > max_cache_size) {
        // Remove least recently used entries until we have enough space
        const char *make_room_sql = 
            "DELETE FROM cache WHERE key IN ("
            "  SELECT key FROM cache"
            "  ORDER BY last_accessed ASC"
            "  LIMIT (SELECT MAX(0, (? - (? - (SELECT SUM(size) FROM cache))) / "
            "         (SELECT MAX(1, AVG(size)) FROM cache)))"
            ");";
        
        rc = sqlite3_prepare_v2(db, make_room_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, max_cache_size);
            sqlite3_bind_int(stmt, 2, size);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
    
    // Insert or replace the cache entry
    const char *insert_sql = 
        "INSERT OR REPLACE INTO cache (key, data, size, created, expires, last_accessed) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    time_t now = time(NULL);
    
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, data, size, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, size);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int64(stmt, 5, expiry);
    sqlite3_bind_int64(stmt, 6, now);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE);
}

// Retrieve a response from the SQLite cache
int sqlite_cache_retrieve(const char *key, void **data, int *size) {
    if (!db || !cache_enabled) {
        cache_misses++;
        return 0;
    }
    
    sqlite3_stmt *stmt;
    int rc;
    
    // First, check if the entry exists and is not expired
    const char *select_sql = 
        "SELECT data, size FROM cache "
        "WHERE key = ? AND (expires = 0 OR expires > ?);";
    
    rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cache_misses++;
        return 0;
    }
    
    time_t now = time(NULL);
    
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // Found valid entry, extract data
        const void *blob_data = sqlite3_column_blob(stmt, 0);
        *size = sqlite3_column_int(stmt, 1);
        
        // Allocate memory for the data
        *data = malloc(*size);
        if (!*data) {
            sqlite3_finalize(stmt);
            cache_misses++;
            return 0;
        }
        
        // Copy the data
        memcpy(*data, blob_data, *size);
        
        // Update last accessed time
        sqlite3_finalize(stmt);
        
        const char *update_sql = 
            "UPDATE cache SET last_accessed = ? WHERE key = ?;";
        
        rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, now);
            sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
        }
        
        sqlite3_finalize(stmt);
        cache_hits++;
        return 1;
    }
    
    sqlite3_finalize(stmt);
    cache_misses++;
    return 0;
}

// Check if a key exists in the SQLite cache
int sqlite_cache_exists(const char *key) {
    if (!db || !cache_enabled) {
        return 0;
    }
    
    sqlite3_stmt *stmt;
    int rc;
    
    const char *select_sql = 
        "SELECT 1 FROM cache "
        "WHERE key = ? AND (expires = 0 OR expires > ?);";
    
    rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    time_t now = time(NULL);
    
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    
    rc = sqlite3_step(stmt);
    int exists = (rc == SQLITE_ROW);
    
    sqlite3_finalize(stmt);
    return exists;
}

// Remove a key from the SQLite cache
int sqlite_cache_remove(const char *key) {
    if (!db) {
        return 0;
    }
    
    sqlite3_stmt *stmt;
    int rc;
    
    const char *delete_sql = "DELETE FROM cache WHERE key = ?;";
    
    rc = sqlite3_prepare_v2(db, delete_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE);
}

// Clear the entire SQLite cache
int sqlite_cache_clear(void) {
    if (!db) {
        return 0;
    }
    
    return exec_sql("DELETE FROM cache;");
}

// Get current SQLite cache size in bytes
int sqlite_cache_size(void) {
    if (!db) {
        return 0;
    }
    
    sqlite3_stmt *stmt;
    int rc;
    
    const char *size_sql = "SELECT COALESCE(SUM(size), 0) FROM cache;";
    
    rc = sqlite3_prepare_v2(db, size_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    int total_size = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total_size = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return total_size;
}

// Get cache hit statistics
void sqlite_cache_stats(int *hits, int *misses) {
    if (hits) *hits = cache_hits;
    if (misses) *misses = cache_misses;
}

// Remove expired entries from the cache
int sqlite_cache_clean_expired(void) {
    if (!db) {
        return 0;
    }
    
    sqlite3_stmt *stmt;
    int rc;
    
    const char *clean_sql = 
        "DELETE FROM cache WHERE expires > 0 AND expires < ?;";
    
    rc = sqlite3_prepare_v2(db, clean_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    time_t now = time(NULL);
    sqlite3_bind_int64(stmt, 1, now);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE);
}

// Set the maximum cache size in bytes
void sqlite_cache_set_max_size(size_t max_size) {
    max_cache_size = max_size;
    
    // If the cache is already initialized, enforce the new size limit
    if (db && max_size > 0) {
        sqlite3_stmt *stmt;
        
        const char *enforce_size_sql = 
            "DELETE FROM cache WHERE key IN ("
            "  SELECT key FROM cache"
            "  ORDER BY last_accessed ASC"
            "  LIMIT (SELECT COUNT(*) FROM cache) - ("
            "    SELECT MAX(0, (? - (SELECT SUM(size) FROM cache)) / "
            "    (SELECT MAX(1, AVG(size)) FROM cache))"
            "  )"
            ");";
        
        int rc = sqlite3_prepare_v2(db, enforce_size_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, max_size);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
}

// Enable or disable the SQLite cache
void sqlite_cache_set_enabled(int enabled) {
    cache_enabled = enabled;
}

// Get whether the SQLite cache is enabled
int sqlite_cache_is_enabled(void) {
    return cache_enabled;
}
