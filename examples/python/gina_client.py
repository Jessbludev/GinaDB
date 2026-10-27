#!/usr/bin/env python3
"""GinaDB Lite 0.6 — cliente Python vía ctypes sobre la C ABI (libginadb.so).

Ejemplo de "cualquier lenguaje": sin dependencias, solo la librería compartida.

    from gina_client import GinaDB
    db = GinaDB("mi-datos")
    db["users"].create(columns={"id": "text", "email": "text"}, pk="id",
                       indexes=["email"])
    db["users"].insert({"id": "u1", "email": "gina@db.io", "age": 33})
    print(db["users"].get("u1"))
    print(db["users"].select({"where": {"op": "eq", "col": "email",
                                       "val": "gina@db.io"}}))
"""
from __future__ import annotations

import ctypes
import json
import base64
import os
from typing import Any, Optional

_LIB_NAMES = ("libginadb.so", "../libginadb.so",
              os.path.join(os.path.dirname(__file__), "..", "libginadb.so"),
              os.path.join(os.path.dirname(__file__), "..", "..", "libginadb.so"))


def _load() -> ctypes.CDLL:
    for name in _LIB_NAMES:
        if os.path.exists(name):
            lib = ctypes.CDLL(os.path.abspath(name))
            break
    else:
        lib = ctypes.CDLL("libginadb.so")  # delegates to the system search path
    lib.gina_version.restype = ctypes.c_char_p
    lib.gina_last_error.restype = ctypes.c_char_p
    lib.gina_last_error.argtypes = [ctypes.c_void_p]
    return lib


class GinaError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(f"[{status}] {message}")
        self.status = status


class _Table:
    def __init__(self, db: "GinaDB", name: str):
        self._db = db
        self._name = name

    def create(self, columns: dict[str, str], pk: str = "id",
               nullable: Optional[dict[str, bool]] = None,
               fts: Optional[list[str]] = None,
               schemaless: bool = False,
               indexes: Optional[list[str]] = None) -> None:
        cols = []
        for cname, ctype in columns.items():
            cols.append({
                "name": cname, "type": ctype,
                "nullable": True if schemaless else (nullable or {}).get(cname, True),
                "fts": cname in (fts or []),
            })
        schema = {"primary_key": pk, "schemaless": schemaless, "columns": cols,
                  "indexes": list(indexes or [])}
        self._db._call("gina_create_table", self._name,
                       json.dumps(schema, ensure_ascii=False))

    def drop(self) -> None:
        self._db._call("gina_drop_table", self._name)

    def schema(self) -> dict:
        return json.loads(self._db._call("gina_schema", self._name, out=str))

    def insert(self, row: dict) -> None:
        self._db._call("gina_insert", self._name, _encode_json(row))

    def update(self, key: str, patch: dict) -> bool:
        st, _ = self._db._call("gina_update", self._name, key,
                               _encode_json(patch), want_status=True)
        return st == 0

    def erase(self, key: str) -> bool:
        rc = self._db._lib.gina_erase(self._db._h, _cstr(self._name),
                                      _cstr(key))
        self._db._check(rc in (0, 1), "erase")
        return rc == 1

    def get(self, key: str) -> Optional[dict]:
        raw = self._db._call("gina_get", self._name, key, out=str, enoent_none=True)
        return json.loads(raw) if raw is not None else None

    def all(self) -> list[dict]:
        return json.loads(self._db._call("gina_all", self._name, out=str))

    def count(self) -> int:
        return self._db._call("gina_count", self._name, out=int)

    def knn(self, column: str, query: list[float], k: int = 5,
            metric: str = "cosine") -> list[dict]:
        arr = (ctypes.c_float * len(query))(*query)
        raw = self._db._lib.gina_knn(self._db._h, _cstr(self._name),
                                     _cstr(column), arr, len(query), k,
                                     1 if metric == "l2" else 0)
        self._db._check(bool(raw), "knn")
        try:
            return json.loads(_pystr(raw))
        finally:
            self._db._lib.gina_free(raw)

    # ---- consultas con planner (0.6) -----------------------------------------
    def select(self, q: Optional[dict] = None) -> list[dict]:
        """select JSON: {"where":{"op":"eq|ne|gt|ge|lt|le|between|in|and|or",
        "col":..,"val":..,"lo":..,"hi":..,"vals":[..],"args":[..]},
        "order":["age"|{"col":"age","desc":true}], "limit":n,
        "fields":["id",...]} — el planner empuja eq/rangos al PK B+Tree y a
        los índices secundarios."""
        return json.loads(self._db._call("gina_select", self._name,
                                         json.dumps(q or {}, ensure_ascii=False),
                                         out=str))

    def create_index(self, column: str) -> None:
        self._db._call("gina_create_index", self._name, column)

    def drop_index(self, column: str) -> None:
        self._db._call("gina_drop_index", self._name, column)

    def search(self, column: str, query: str, limit: int = 50) -> list[dict]:
        raw = self._db._call("gina_search", self._name, column, query,
                             limit, out=str)
        return json.loads(raw)

    def changes(self, since: int = 0) -> dict:
        return json.loads(self._db._call("gina_changes", self._name, since,
                                         out=str))

    def watch(self, callback):
        cb = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)(
            lambda ev, _ud: callback(json.loads(ev.decode())))
        # keep a reference so the closure is not garbage-collected
        cb._gina_keep = self._db._watch_keep
        self._db._watch_keep.append(cb)
        return self._db._lib.gina_watch(self._db._h, _cstr(self._name), cb, None)

    def unwatch(self, watch_id: int) -> None:
        self._db._call("gina_unwatch", watch_id)


def _cstr(s):
    return s.encode() if isinstance(s, str) else s


def _pystr(p):
    return ctypes.cast(p, ctypes.c_char_p).value.decode()


def _encode_json(v: Any) -> str:
    def rec(x):
        if isinstance(x, bytes):
            return {"$bytes": base64.b64encode(x).decode()}
        if isinstance(x, (list, tuple)) and x and isinstance(x[0], float):
            return list(x)  # plain list of floats → {"$vec"} handled below
        return x
    # vectors: lists marked with Vec helper
    return json.dumps(_rec_encode(v), ensure_ascii=False)


def _rec_encode(x):
    if isinstance(x, bytes):
        return {"$bytes": base64.b64encode(x).decode()}
    if isinstance(x, Vec):
        return {"$vec": list(x)}
    if isinstance(x, dict):
        return {k: _rec_encode(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [_rec_encode(v) for v in x]
    return x


class Vec(list):
    """Marks a list of floats as a GinaDB vector (embedding)."""


class GinaDB:
    def __init__(self, path: str, durable: bool = False,
                 checkpoint_every: int = 1000):
        self._lib = _load()
        self._watch_keep = []
        self._h = self._lib.gina_open(_cstr(path), 1 if durable else 0,
                                      checkpoint_every, 4096, 16 * 1024 * 1024)
        if not self._h:
            raise GinaError(-1, "cannot open database")

    def __getitem__(self, table: str) -> _Table:
        return _Table(self, table)

    def tables(self) -> list[str]:
        return json.loads(self._call("gina_tables", out=str))

    def stats(self) -> dict:
        return json.loads(self._call("gina_stats", out=str))

    def exec_tx(self, ops: list[dict]) -> None:
        """Atomic batch: [{"op":"put","table":"t","row":{...}},
                          {"op":"del","table":"t","key":"k"}]"""
        self._call("gina_exec", _encode_json(ops))

    def checkpoint(self) -> None:
        self._call("gina_checkpoint")

    def compact(self) -> None:
        """0.6: checkpoint completo (reescribe data.pages, reinicia delta)."""
        self._call("gina_compact")

    def close(self) -> None:
        if getattr(self, "_h", None):
            self._lib.gina_close(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- internals ----------------------------------------------------------
    def _call(self, fn, *args, out=None, enoent_none=False, want_status=False):
        c_args = []
        for a in args:
            if isinstance(a, str):
                c_args.append(_cstr(a))
            elif isinstance(a, int) and a >= 1 << 32:
                c_args.append(ctypes.c_uint64(a))
            else:
                c_args.append(a)
        raw = getattr(self._lib, fn)(self._h, *c_args)
        status = self._lib.gina_last_status()
        if status != 0 and not (status == 1 and (enoent_none or want_status)):
            raise GinaError(status, self._lib.gina_last_error(None).decode())
        if want_status:
            return status, raw
        if out == str:
            if raw is None or not raw:
                return None
            try:
                return _pystr(raw)
            finally:
                self._lib.gina_free(raw)
        if out == int:
            self._check(raw >= 0, fn)
            return raw
        return raw

    def _check(self, ok, what):
        if not ok:
            raise GinaError(self._lib.gina_last_status(),
                            self._lib.gina_last_error(None).decode() or what)


if __name__ == "__main__":
    with GinaDB("py-demo-data") as db:
        users = db["users"]
        if "users" not in db.tables():
            users.create(columns={"id": "text", "name": "text",
                                  "bio": "text"},
                         pk="id", nullable={"id": False, "name": False},
                         fts=["bio"])
        users.insert({"id": "u1", "name": "Gina",
                      "bio": "base de datos embebida multimodal"})
        users.insert({"id": "u2", "name": "Zed",
                      "bio": "usuario de bases de datos"})
        print("get:", users.get("u1"))
        print("all:", len(users.all()), "rows")
        print("search 'base datos':", [r["id"] for r in
                                       users.search("bio", "base datos")])

        media = db["media"]
        if "media" not in db.tables():
            media.create(columns={"id": "text", "tag": "text",
                                  "vec": "vector"}, pk="id",
                         nullable={"id": False})
        media.insert({"id": "m1", "tag": "gina", "vec": Vec([0.1, 0.2, 0.3])})
        media.insert({"id": "m2", "tag": "db", "vec": Vec([0.9, 0.1, 0.0])})
        media.insert({"id": "m3", "tag": "lite", "vec": Vec([0.15, 0.25, 0.35])})
        print("knn:", media.knn("vec", [0.1, 0.2, 0.3], k=2))

        seen = []
        users.watch(lambda ev: seen.append(ev["type"] + ":" + ev["key"]))
        users.insert({"id": "u3", "name": "Ada", "bio": "pionera"})
        print("watch events:", seen)
        ch = users.changes(0)
        print("changes:", ch["next_cursor"], "events:",
              len(ch["events"]))
        print("stats:", db.stats())
