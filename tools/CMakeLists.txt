add_executable(bench_pool bench_pool.cpp)
target_link_libraries(bench_pool PRIVATE so_core)

add_executable(bench_reader bench_reader.cpp)
target_link_libraries(bench_reader PRIVATE so_core)

add_executable(embed_crops embed_crops.cpp)
target_link_libraries(embed_crops PRIVATE so_core)
