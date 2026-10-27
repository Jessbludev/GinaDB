// GinaDB-Lite 0.6 — hijo de prueba de crash: inserta claves secuenciales con
// durable=true (fsync por commit). El padre lo mata con SIGKILL en un momento
// aleatorio; al reabrir, las filas presentes DEBEN ser un prefijo contiguo
// k0000..kNN (atomicidad + durabilidad por commit).
import { GinaDB } from "./client.ts";

const dir = process.argv[2] ?? "ts-data/crash";
const total = Number(process.argv[3] ?? 5000);
const db = await GinaDB.open(dir, { durable: true, checkpointEvery: 0 });
let done = 0;
for (let i = 0; i < total; i++) {
  const key = "k" + String(i).padStart(6, "0");
  db.insert("victim", { id: key, n: i });
  done++;
}
// si llega aquí, el padre no llegó a matarlo a tiempo
console.log(JSON.stringify({ completed: done }));
db.close();
