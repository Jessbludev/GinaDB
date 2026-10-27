/* GinaDB Lite 0.7 — C ABI (extern "C"), so any language can use GinaDB:
 * Python (ctypes), TypeScript/Node (FFI), Bun (bun:ffi), Rust, Zig, Lua, ...
 *
 * Memory rules:
 *   - Every `char*` returned by these functions is heap-allocated; release it
 *     with gina_free(). NULL + gina_last_status() == GINA_ENOENT means
 *     "not found" (not an error).
 *   - Strings passed as callbacks are only valid during the callback.
 *   - A handle is thread-safe; watch callbacks run on the committing thread
 *     and must not call write operations on the same database.
 */
#ifndef GINA_CABI_H
#define GINA_CABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gina_db gina_db;

enum gina_status {
    GINA_OK = 0,
    GINA_ENOENT = 1,     /* key/table not found (result, not failure)   */
    GINA_EEXISTS = 2,    /* table already exists                        */
    GINA_ESCHEMA = 3,    /* row violates the table schema               */
    GINA_EKEY = 4,       /* bad primary key                             */
    GINA_ETX = 5,        /* transaction already finished                */
    GINA_EIO = 6,        /* filesystem error                            */
    GINA_ECORRUPT = 7,   /* storage corruption detected                 */
    GINA_ELIMIT = 8,     /* blob too large / limit exceeded             */
    GINA_EARG = 9,       /* invalid argument                            */
    GINA_ERROR = -1,     /* unexpected failure: see gina_last_error()   */
};

/* lifecycle */
gina_db* gina_open(const char* dir, int32_t durable, uint32_t checkpoint_every,
                   uint64_t changelog_capacity, uint64_t max_blob_size);
void gina_close(gina_db* db);

/* DDL — json_schema: {"primary_key":"id","schemaless":false,
 *                     "columns":[{"name":"id","type":"text","nullable":false,
 *                                 "fts":false}]} */
int32_t gina_create_table(gina_db* db, const char* name, const char* json_schema);
int32_t gina_drop_table(gina_db* db, const char* name);
char* gina_schema(gina_db* db, const char* table);

/* CRUD — json_row / json_patch are JSON objects; bytes as {"$bytes":"<b64>"},
 * vectors as {"$vec":[0.1,0.2,...]} */
int32_t gina_insert(gina_db* db, const char* table, const char* json_row);
int32_t gina_update(gina_db* db, const char* table, const char* key,
                    const char* json_patch);
int32_t gina_erase(gina_db* db, const char* table, const char* key);
char* gina_get(gina_db* db, const char* table, const char* key);
char* gina_all(gina_db* db, const char* table);
int64_t gina_count(gina_db* db, const char* table);

/* batch (atomic transaction):
 * [{"op":"put","table":"t","row":{...}}, {"op":"del","table":"t","key":"k"}] */
int32_t gina_exec(gina_db* db, const char* json_ops);

/* multimodal */
char* gina_knn(gina_db* db, const char* table, const char* column,
               const float* query, uint32_t dims, uint32_t k, int32_t metric);
char* gina_search(gina_db* db, const char* table, const char* column,
                  const char* query, uint32_t limit);

/* real-time */
char* gina_changes(gina_db* db, const char* table, uint64_t since);
int64_t gina_watch(gina_db* db, const char* table,
                   void (*cb)(const char* json_event, void* userdata),
                   void* userdata);
int32_t gina_unwatch(gina_db* db, int64_t watch_id);

/* queries (0.6) — json_query:
 * {"where":{"op":"eq","col":"email","val":"a@b.c"},
 *  "order":[{"col":"age","desc":true}], "limit":10,
 *  "fields":["id","name"]}
 * where ops: eq ne gt ge lt le between(col,lo,hi) in(col,vals[]) and/or(args[]).
 * Returns a JSON array of rows (projected if "fields" given). The planner
 * pushes eq/range down to the PK B+Tree and secondary indexes. */
char* gina_select(gina_db* db, const char* table, const char* json_query);

/* secondary indexes (0.6) — atomic, WAL-logged, survive restarts */
int32_t gina_create_index(gina_db* db, const char* table, const char* column);
int32_t gina_drop_index(gina_db* db, const char* table, const char* column);

/* maintenance + info */
int32_t gina_checkpoint(gina_db* db);   /* incremental (compact if due) */
int32_t gina_compact(gina_db* db);      /* full rewrite + delta reset */
char* gina_stats(gina_db* db);
char* gina_tables(gina_db* db);

/* helpers */
void gina_free(char* s);
const char* gina_version(void);
const char* gina_last_error(gina_db* db);
int32_t gina_last_status(void);

#ifdef __cplusplus
}
#endif
#endif /* GINA_CABI_H */
