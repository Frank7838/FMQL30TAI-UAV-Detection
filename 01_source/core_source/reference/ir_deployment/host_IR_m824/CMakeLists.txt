# IR保留扩展构建入口；依赖原Icraft工具链，不会自动执行板端推理。
cmake_minimum_required(VERSION 3.28)
project(IR_raw_host_m824 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Icraft-HostBackend REQUIRED)
add_executable(IR_raw_host host.cpp)
target_compile_options(IR_raw_host PRIVATE /utf-8 /Zc:preprocessor /Zc:__cplusplus)
target_link_libraries(IR_raw_host PRIVATE Icraft::HostBackend)
