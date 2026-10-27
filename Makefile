# GinaDB Lite 0.7 — build mínimo (sin dependencias más allá de g++ y make)
#   make            → lib estática + compartida + herramientas + tests
#   make test       → btree + unit + crash suites
#   make bench      → benchmark 0.6
#   make bench-reopen → banco focalizado de reapertura 0.7 (100K+10K filas)
#   make zig        → parser/inspector Zig (requiere zig 0.16.0 en PATH o ZIG=...)
#   make demo       → demo interactiva de la API
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread
AR       ?= ar
ZIG      ?= $(shell command -v zig 2>/dev/null || echo /home/z/zig16/zig)

SRCS := src/codec.cpp src/srow.cpp src/db.cpp src/api.cpp src/pager.cpp src/delta.cpp src/fts.cpp src/json.cpp src/cabi.cpp
LIB_SRCS := src/codec.cpp src/srow.cpp src/db.cpp src/api.cpp src/pager.cpp src/delta.cpp src/fts.cpp src/json.cpp
TEST_LIB := $(LIB_SRCS)

.PHONY: all test bench zig demo clean package test-ts test-py

all: libginadb.a libginadb.so gina demo ginadump

libginadb.a: $(SRCS) include/gina/db.hpp include/gina/value.hpp include/gina/cabi.h
	$(CXX) $(CXXFLAGS) -Iinclude -fPIC -c $(SRCS)
	$(AR) rcs $@ codec.o srow.o db.o api.o pager.o delta.o fts.o json.o cabi.o

libginadb.so: $(SRCS)
	$(CXX) $(CXXFLAGS) -Iinclude -fPIC -shared $(SRCS) -o $@

gina: tools/gina_shell.cpp libginadb.a
	$(CXX) $(CXXFLAGS) -Iinclude tools/gina_shell.cpp libginadb.a -o $@

demo: tools/demo.cpp $(LIB_SRCS)
	$(CXX) $(CXXFLAGS) -Iinclude tools/demo.cpp $(LIB_SRCS) -o $@

ginadump: tools/ginadump.cpp $(LIB_SRCS)
	$(CXX) $(CXXFLAGS) -Iinclude tools/ginadump.cpp $(LIB_SRCS) -o $@

tests/unit_test: tests/unit_test.cpp $(LIB_SRCS) src/json.hpp
	$(CXX) $(CXXFLAGS) -Iinclude -Isrc tests/unit_test.cpp $(LIB_SRCS) -o $@

tests/btree_test: tests/btree_test.cpp src/btree.hpp
	$(CXX) $(CXXFLAGS) -Isrc tests/btree_test.cpp -o $@

tests/crash_test: tests/crash_test.cpp $(LIB_SRCS) src/codec.hpp
	$(CXX) $(CXXFLAGS) -Iinclude -Isrc tests/crash_test.cpp $(LIB_SRCS) -o $@

test: tests/unit_test tests/btree_test tests/crash_test
	./tests/btree_test
	./tests/unit_test
	./tests/crash_test

benchmark/bench: benchmark/bench.cpp $(LIB_SRCS)
	$(CXX) $(CXXFLAGS) -Iinclude benchmark/bench.cpp $(LIB_SRCS) -o $@

bench: benchmark/bench
	./benchmark/bench

# 0.7 — bancos focalizados: reapertura (filas planas + decode paralelo),
# equivalencia del CRC slice-by-8 y validación cruzada con el lector Zig
benchmark/reopen_bench: benchmark/reopen_bench.cpp $(LIB_SRCS)
	$(CXX) $(CXXFLAGS) -Iinclude benchmark/reopen_bench.cpp $(LIB_SRCS) -o $@
benchmark/crc_check: benchmark/crc_check.cpp src/codec.cpp
	$(CXX) $(CXXFLAGS) -Iinclude -Isrc benchmark/crc_check.cpp src/codec.cpp -o $@
benchmark/zig_xval: benchmark/zig_xval.cpp $(LIB_SRCS)
	$(CXX) $(CXXFLAGS) -Iinclude benchmark/zig_xval.cpp $(LIB_SRCS) -o $@

bench-reopen: benchmark/reopen_bench
	rm -rf bench-reopen-data && ./benchmark/reopen_bench bench-reopen-data

crc-check: benchmark/crc_check
	./benchmark/crc_check

xval: benchmark/zig_xval
	rm -rf bench-xval-data && ./benchmark/zig_xval bench-xval-data && \
	$(ZIG) build -C zig 2>/dev/null || (cd zig && $(ZIG) build) ; \
	./zig/zig-out/bin/gina-inspect bench-xval-data

zig:
	cd zig && $(ZIG) build && $(ZIG) build test

# suites multi-lenguaje (requieren bun/node y python3; node necesita koffi:
# cd examples/typescript && npm install)
test-ts:
	cd examples/typescript && GINA_LIB=$(CURDIR)/libginadb.so bun test
	cd examples/typescript && GINA_LIB=$(CURDIR)/libginadb.so bun src/workload.ts 5000 2000 1000

test-py:
	cd examples/python && python3 verify06.py

demo-run: demo
	rm -rf demo-data && ./demo

# tarball fuente limpio (sin binarios, BDs de prueba ni node_modules)
package: clean
	cd .. && tar czf GinaDB-Lite-0.7.tar.gz \
		--exclude='GinaDB-Lite-0.7/node_modules' \
		--exclude='GinaDB-Lite-0.7/examples/typescript/node_modules' \
		--exclude='GinaDB-Lite-0.7/examples/typescript/ts-data' \
		--exclude='GinaDB-Lite-0.7/zig/zig-out' \
		--exclude='GinaDB-Lite-0.7/zig/.zig-cache' \
		--exclude='GinaDB-Lite-0.7/examples/python/py-data*' \
		--exclude='GinaDB-Lite-0.7/examples/python/__pycache__' \
		--exclude='GinaDB-Lite-0.7/GinaDB-Lite-0.7.tar.gz' \
		--exclude='GinaDB-Lite-0.7/open' \
		--exclude='GinaDB-Lite-0.7/--help' \
		GinaDB-Lite-0.7 && mv GinaDB-Lite-0.7.tar.gz GinaDB-Lite-0.7/ && \
		echo "tarball: GinaDB-Lite-0.7/$$(basename GinaDB-Lite-0.7.tar.gz) listo"

clean:
	rm -f libginadb.a libginadb.so gina demo ginadump *.o
	rm -f tests/unit_test tests/btree_test tests/crash_test benchmark/bench
	rm -f benchmark/reopen_bench benchmark/crc_check benchmark/zig_xval
	rm -rf bench-reopen-data bench-xval-data
	rm -rf zig/zig-out zig/.zig-cache demo-data cli-data gina-data bench-data bench-knn
	rm -rf t-* c-* smoke-data reopen-data py-demo-data ffi-demo-data ts-data
