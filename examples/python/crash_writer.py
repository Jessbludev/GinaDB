#!/usr/bin/env python3
"""GinaDB 0.6 — escritor durable para la prueba de crash (lo mata SIGKILL)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gina_client import GinaDB  # noqa: E402

dir_path = sys.argv[1]
total = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
db = GinaDB(dir_path, durable=True, checkpoint_every=0)
if "victim" not in db.tables():
    db["victim"].create(columns={"id": "text", "n": "int"}, pk="id")
    db.checkpoint()
for i in range(total):
    db["victim"].insert({"id": f"k{i:06d}", "n": i})
db.close()
