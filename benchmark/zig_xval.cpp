// BD de prueba para validación cruzada del lector Zig (misma forma que 0.6):
// updates + deletes + tabla creada y borrada + índices + blob overflow.
#include "gina/db.hpp"
#include <cstdio>
#include <filesystem>
using namespace gina;

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "zig-xval-db";
    std::filesystem::remove_all(dir);
    {
        Database db(dir, Options{.checkpoint_every = 0});
        db["users"].create({.primary_key = "id",
                            .columns = {{"id", T::Text}, {"name", T::Text},
                                        {"age", T::Int}, {"email", T::Text},
                                        {"data", T::Bytes}},
                            .indexes = {"email", "age"}});
        for (int i = 0; i < 12; i++)
            db["users"].emplace("id", ("u" + std::to_string(i)).c_str(),
                                "name", ("user" + std::to_string(i)).c_str(),
                                "age", 20 + i,
                                "email", ("u" + std::to_string(i) + "@x.io").c_str());
        db.checkpoint();               // IC con las 12 filas
        db["users"].patch("u3", "age", 99);   // update en delta
        db["users"].erase("u4");              // delete en delta
        db["temp"].create({.schemaless = true});
        db["temp"].emplace("id", "t1", "v", 1);
        db.checkpoint();               // IC: u3, u4, temp-schema, t1
        db["temp"].drop();             // drop en delta (sin IC)
        db["users"].create_index("name");    // re-schema en delta
        Bytes blob(9000, 0x5A);              // blob → registro L_OVF del delta
        db["users"].emplace("id", "blob", "name", "blob", "age", 1,
                            "email", "blob@x.io", "data", blob);
        db.checkpoint();
    }
    printf("BD de validación creada en %s\n", dir.c_str());
    {
        Database db(dir, Options{.checkpoint_every = 0});
        printf("motor: users=%zu (esperado 12: 11 originales + blob; u4 borrado)\n",
               db["users"].count());
        printf("motor: u3.age=%lld (esperado 99)\n",
               db["users"].get("u3")->at("age").as_int());
        printf("motor: u4=%s (esperado NO)\n",
               db["users"].get("u4").has_value() ? "SÍ" : "NO");
        printf("motor: tablas=%zu (esperado 1: temp borrada)\n",
               db.tables().size());
        printf("motor: índices de users=%zu (esperado 3: email,age,name)\n",
               db["users"].schema().indexes.size());
        const auto st = db.stats();
        printf("motor: delta live=%llu free=%llu corrupt=%llu broken_ovf=%llu\n",
               (unsigned long long)st.delta_live_pages,
               (unsigned long long)st.delta_free_pages,
               (unsigned long long)st.delta_corrupt_pages,
               (unsigned long long)st.delta_broken_ovf_records);
        printf("motor: blob=%zu bytes (esperado 9000)\n",
               db["users"].get("blob")->at("data").as_bytes().size());
    }
    return 0;
}
