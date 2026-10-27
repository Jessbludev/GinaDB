// GinaDB Lite 0.7 — gina: JSON CLI over the C ABI.
// Usable from ANY language via subprocess (no linking needed):
//
//   gina <db-dir> create users '{"columns":[{"name":"id","type":"text"}],"indexes":["email"]}'
//   gina <db-dir> put    users '{"id":"u1","name":"Gina"}'
//   gina <db-dir> get    users u1
//   gina <db-dir> update users u1 '{"name":"GinaDB"}'
//   gina <db-dir> del    users u1
//   gina <db-dir> all    users
//   gina <db-dir> select users '{"where":{"op":"eq","col":"email","val":"gina@db.io"}}'
//   gina <db-dir> index  users email      (índice secundario)
//   gina <db-dir> unindex users email
//   gina <db-dir> knn    media vec 0.1,0.2,0.3 5
//   gina <db-dir> search users  bio  "base datos"
//   gina <db-dir> changes users 0
//   gina <db-dir> exec    '[{"op":"put","table":"users","row":{...}}]'
//   gina <db-dir> schema  users
//   gina <db-dir> stats | tables | checkpoint | compact
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gina/cabi.h"

static int fail(const char* why) {
    std::fprintf(stderr, "error: %s (status=%d)\n", why, gina_last_status());
    return 2;
}

static void print_and_free(char* s) {
    if (s) {
        std::puts(s);
        gina_free(s);
    } else {
        std::puts("null");
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <db-dir> <command> [args...]\n"
                     "commands: create put get update del all count select knn "
                     "search index unindex changes exec schema stats tables "
                     "checkpoint compact\n",
                     argv[0]);
        return 1;
    }
    gina_db* db = gina_open(argv[1], 0, 1000, 4096, 16u << 20);
    if (!db) return fail("cannot open database");
    const std::string cmd = argv[2];
    int rc = 0;

    if (cmd == "create" && argc >= 4) {
        rc = gina_create_table(db, argv[3], argc > 4 ? argv[4] : "{}");
        if (rc != 0) rc = fail(gina_last_error(nullptr));
    } else if (cmd == "put" && argc >= 5) {
        rc = gina_insert(db, argv[3], argv[4]);
        if (rc != 0) rc = fail(gina_last_error(nullptr));
    } else if (cmd == "get" && argc >= 5) {
        char* s = gina_get(db, argv[3], argv[4]);
        if (!s && gina_last_status() == GINA_ENOENT) { std::puts("null"); }
        else if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "update" && argc >= 6) {
        rc = gina_update(db, argv[3], argv[4], argv[5]);
        if (rc != 0) rc = fail(gina_last_error(nullptr));
    } else if (cmd == "del" && argc >= 5) {
        const int r = gina_erase(db, argv[3], argv[4]);
        std::printf("%s\n", r == 1 ? "true" : r == 0 ? "false" : "error");
        if (r < 0) rc = fail(gina_last_error(nullptr));
    } else if (cmd == "all" && argc >= 4) {
        char* s = gina_all(db, argv[3]);
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "count" && argc >= 4) {
        std::printf("%lld\n", static_cast<long long>(gina_count(db, argv[3])));
    } else if (cmd == "knn" && argc >= 7) {
        // gina <db> knn <table> <column> <v1,v2,...> <k> [metric]
        std::vector<float> q;
        for (char* tok = std::strtok(argv[5], ","); tok;
             tok = std::strtok(nullptr, ","))
            q.push_back(static_cast<float>(std::atof(tok)));
        char* s = gina_knn(db, argv[3], argv[4], q.data(),
                           static_cast<std::uint32_t>(q.size()),
                           static_cast<std::uint32_t>(std::atoi(argv[6])),
                           argc > 7 && std::strcmp(argv[7], "l2") == 0 ? 1 : 0);
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "search" && argc >= 6) {
        char* s = gina_search(db, argv[3], argv[4], argv[5], 50);
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "select" && argc >= 4) {
        char* s = gina_select(db, argv[3], argc > 4 ? argv[4] : "{}");
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "index" && argc >= 5) {
        rc = gina_create_index(db, argv[3], argv[4]);
        if (rc != 0) rc = fail(gina_last_error(nullptr));
        else std::puts("ok");
    } else if (cmd == "unindex" && argc >= 5) {
        rc = gina_drop_index(db, argv[3], argv[4]);
        if (rc != 0) rc = fail(gina_last_error(nullptr));
        else std::puts("ok");
    } else if (cmd == "changes" && argc >= 5) {
        char* s = gina_changes(db, argv[3],
                               static_cast<std::uint64_t>(std::strtoull(argv[4], nullptr, 10)));
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "exec" && argc >= 4) {
        rc = gina_exec(db, argv[3]);
        if (rc != 0) rc = fail(gina_last_error(nullptr));
    } else if (cmd == "schema" && argc >= 4) {
        char* s = gina_schema(db, argv[3]);
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "stats") {
        char* s = gina_stats(db);
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "tables") {
        char* s = gina_tables(db);
        if (!s) rc = fail(gina_last_error(nullptr));
        else print_and_free(s);
    } else if (cmd == "checkpoint") {
        rc = gina_checkpoint(db);
        if (rc != 0) rc = fail(gina_last_error(nullptr));
        else std::puts("ok");
    } else if (cmd == "compact") {
        rc = gina_compact(db);
        if (rc != 0) rc = fail(gina_last_error(nullptr));
        else std::puts("ok");
    } else {
        std::fprintf(stderr, "unknown or incomplete command: %s\n", argv[2]);
        rc = 1;
    }

    gina_close(db);
    return rc;
}
