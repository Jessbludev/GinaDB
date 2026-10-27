cmake_minimum_required(VERSION 3.20)
project(GinaDB LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ginadb STATIC
    src/codec.cpp src/db.cpp src/api.cpp src/pager.cpp src/fts.cpp src/json.cpp)
target_include_directories(ginadb PUBLIC include)
target_compile_options(ginadb PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(ginadb PUBLIC Threads::Threads)

add_library(ginadb_shared SHARED
    src/codec.cpp src/db.cpp src/api.cpp src/pager.cpp src/fts.cpp src/json.cpp
    src/cabi.cpp)
target_include_directories(ginadb_shared PUBLIC include)
target_link_libraries(ginadb_shared PUBLIC Threads::Threads)
set_target_properties(ginadb_shared PROPERTIES OUTPUT_NAME ginadb)

add_executable(gina_demo tools/demo.cpp)
target_link_libraries(gina_demo PRIVATE ginadb)
add_executable(ginadump tools/ginadump.cpp)
target_link_libraries(ginadump PRIVATE ginadb)
add_executable(gina tools/gina_shell.cpp)
target_link_libraries(gina PRIVATE ginadb_shared)

enable_testing()
add_executable(unit_test tests/unit_test.cpp)
target_link_libraries(unit_test PRIVATE ginadb)
add_test(NAME unit COMMAND unit_test)
add_executable(crash_test tests/crash_test.cpp)
target_link_libraries(crash_test PRIVATE ginadb)
add_test(NAME crash COMMAND crash_test)

add_executable(bench benchmark/bench.cpp)
target_link_libraries(bench PRIVATE ginadb)
