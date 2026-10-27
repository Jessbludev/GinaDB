#!/usr/bin/env python3
"""GinaDB Lite 0.6 — verificación multi-lenguaje desde Python.

Somete al motor a las operaciones para las que fue diseñada (CRUD, índices
secundarios con select, rangos PK, FTS, KNN, transacciones, watch, checkpoint
incremental, compactación, re-apertura y crash real con SIGKILL) y verifica
resultados con asserts. Complementa la suite de TypeScript (bun/node).
"""
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gina_client import GinaDB, Vec  # noqa: E402

DIR = "py-data-verify"
N = 3000
rng = random.Random(606)

ok = 0
fail = 0


def check(cond, what):
    global ok, fail
    if cond:
        ok += 1
    else:
        fail += 1
        print(f"  FALLÓ: {what}")


def main():
    shutil.rmtree(DIR, ignore_errors=True)
    db = GinaDB(DIR, checkpoint_every=0)

    print("motor: GinaDB (via libginadb.so)")

    # ---- schema + carga -----------------------------------------------------
    users = db["users"]
    users.create(columns={"id": "text", "email": "text", "age": "int",
                          "city": "text", "bio": "text"},
                 pk="id", fts=["bio"], indexes=["email", "age", "city"])
    ops = []
    for i in range(N):
        ops.append({"op": "put", "table": "users", "row": {
            "id": f"u{i:06d}", "email": f"u{i}@x.io", "age": 18 + (i % 60),
            "city": ["Madrid", "Lima", "Bogotá"][i % 3],
            "bio": f"perfil {i} base datos embedding vector"}})
    db.exec_tx(ops)
    check(users.count() == N, "carga por lotes")

    # ---- índices: eq / rango / in --------------------------------------------
    hit = users.select({"where": {"op": "eq", "col": "email", "val": "u777@x.io"}})
    check(len(hit) == 1 and hit[0]["id"] == "u000777", "select eq email (índice)")

    lo, hi = 30, 39
    rows = users.select({"where": {"op": "between", "col": "age", "lo": lo, "hi": hi},
                         "order": [{"col": "age", "desc": True}], "limit": 100})
    want = sum(1 for i in range(N) if lo <= 18 + (i % 60) <= hi)
    check(len(rows) == min(want, 100), "select rango age + order desc")
    check(all(lo <= r["age"] <= hi for r in rows), "rango age correcto")
    check(rows == sorted(rows, key=lambda r: -r["age"]), "orden desc por índice")

    # rango PK inverso con límite
    last = users.select({"where": {"op": "ge", "col": "id", "val": "u002990"},
                         "order": [{"col": "id", "desc": True}], "limit": 5,
                         "fields": ["id"]})
    check([r["id"] for r in last] == ["u002999", "u002998", "u002997",
                                      "u002996", "u002995"],
          "rango PK desc + limit")

    # AND de dos índices
    both = users.select({"where": {"op": "and", "args": [
        {"op": "eq", "col": "city", "val": "Lima"},
        {"op": "between", "col": "age", "lo": 20, "hi": 29}]}})
    check(all(r["city"] == "Lima" and 20 <= r["age"] <= 29 for r in both),
          "AND(eq city, rango age)")
    check(len(both) == sum(1 for i in range(N)
                           if i % 3 == 1 and 20 <= 18 + (i % 60) <= 29),
          "AND cuenta exacta")

    # in_
    inq = users.select({"where": {"op": "in", "col": "city",
                                  "vals": ["Lima", "Bogotá"]}})
    check(len(inq) == sum(1 for i in range(N) if i % 3 != 0), "in_ ciudades")

    # ---- índice dinámico + persistencia ----------------------------------------
    users.create_index("bio")
    check("bio" in users.schema().get("indexes", []), "create_index visible")
    users.drop_index("bio")

    # ---- FTS / KNN -----------------------------------------------------------
    docs = db["docs"]
    docs.create(columns={"id": "text", "body": "text", "vec": "vector"},
                pk="id", fts=["body"])
    db.exec_tx([{"op": "put", "table": "docs", "row": {
        "id": f"d{i}", "body": f"documento {i} sobre bases de datos",
        "vec": Vec([rng.uniform(-1, 1) for _ in range(32)])}} for i in range(500)])
    fts = docs.search("body", "bases datos")
    check(len(fts) == 50, "FTS AND 2 términos (límite por defecto 50)")
    q = [rng.uniform(-1, 1) for _ in range(32)]
    knn = docs.knn("vec", q, k=5)
    check(len(knn) == 5 and knn[0]["score"] >= knn[4]["score"], "KNN ordenado")

    # ---- CRUD + watch + changelog -------------------------------------------------
    seen = []
    users.watch(lambda ev: seen.append(ev["type"]))
    users.update("u000000", {"age": 99})
    users.erase("u000001")
    users.insert({"id": "extra", "email": "extra@x.io", "age": 1,
                  "city": "Lima", "bio": "nueva fila"})
    check(seen == ["put", "del", "put"], "watch 3 eventos")
    check(users.get("u000000")["age"] == 99, "update")
    check(users.get("u000001") is None, "erase")
    ch = users.changes(0)
    check(ch["next_cursor"] >= 3, "changelog cursor")

    # ---- checkpoint incremental + reopen --------------------------------------------
    db.checkpoint()
    st = db.stats()
    check(st.get("delta_live_pages", 0) > 0, "IC produce delta vivo")
    db.close()

    db = GinaDB(DIR, checkpoint_every=0)
    users = db["users"]
    check(users.count() == N, "reopen conserva filas")  # N-1 + extra
    check(users.get("extra") is not None, "reopen conserva la fila extra")
    check(len(users.select({"where": {"op": "eq", "col": "email",
                                      "val": "u777@x.io"}})) == 1,
          "índice funciona tras reopen")

    # ---- compactación ------------------------------------------------------------------
    db.compact()
    st = db.stats()
    check(st.get("data_bytes", 0) > 0, "FC materializa base")
    check(st.get("delta_live_pages", -1) == 0, "FC reinicia delta")
    db.close()

    # ---- crash real: SIGKILL a un hijo que escribe durable ------------------------------
    print("  crash: lanzando hijo durable y matándolo con SIGKILL...")
    child = subprocess.Popen(
        [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                      "crash_writer.py"), DIR + "-crash", "200000"])
    time.sleep(0.35)
    child.send_signal(signal.SIGKILL)
    child.wait()

    t0 = time.perf_counter()
    cdb = GinaDB(DIR + "-crash", durable=True)
    rec_ms = (time.perf_counter() - t0) * 1000
    victim = cdb["victim"]
    n = victim.count()
    check(n > 0, f"crash: sobrevivieron {n} commits")
    # prefijo contiguo: muestra + frontera
    good = all(victim.get(f"k{i:06d}") is not None for i in range(0, min(n, 2000)))
    good = good and victim.get(f"k{n:06d}") is None
    check(good, "crash: prefijo contiguo de commits (atomicidad)")
    print(f"  crash: {n} commits sobrevivieron, recuperación {rec_ms:.1f} ms")
    cdb.close()

    db = GinaDB(DIR, checkpoint_every=0)
    db.close()

    print(f"\npython verify: {ok} ok, {fail} fallos")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
