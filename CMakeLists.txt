cmake_minimum_required(VERSION 3.16)
project(latencytax CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

add_library(ltx_flags INTERFACE)
target_compile_options(ltx_flags INTERFACE -Wall -Wextra -Wpedantic)
target_include_directories(ltx_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

option(LTX_NATIVE "Build with -march=native" ON)

target_compile_options(ltx_flags INTERFACE
  $<$<CONFIG:Release>:-O3>
  $<$<AND:$<CONFIG:Release>,$<BOOL:${LTX_NATIVE}>>:-march=native>
  $<$<CONFIG:Release>:-fno-omit-frame-pointer>
  $<$<CONFIG:Debug>:-O0>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
)
target_link_options(ltx_flags INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# Thread sanitizer build for the lock free queue. ASan and TSan cannot coexist,
# so this is its own build type: -DCMAKE_BUILD_TYPE=TSan
set(CMAKE_CXX_FLAGS_TSAN "-O1 -g -fsanitize=thread -fno-omit-frame-pointer"
    CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_TSAN "-fsanitize=thread" CACHE STRING "" FORCE)

add_library(ltx_engine STATIC
  src/engine/order_book.cpp
)
target_link_libraries(ltx_engine PUBLIC ltx_flags Threads::Threads)

add_library(ltx_data STATIC
  src/util/gzline.cpp
  src/replay/loader.cpp
  src/replay/reconstruct.cpp
)
target_link_libraries(ltx_data PUBLIC ltx_flags ZLIB::ZLIB)

add_executable(bench bench/bench_main.cpp)
target_link_libraries(bench PRIVATE ltx_engine)

add_executable(replay src/replay/replay_main.cpp)
target_link_libraries(replay PRIVATE ltx_engine ltx_data)

add_executable(sim src/sim/sim_main.cpp)
target_link_libraries(sim PRIVATE ltx_engine ltx_data)


include(CTest)
enable_testing()
foreach(t test_order_book test_spsc test_id_map test_reconstruct)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE ltx_engine ltx_data)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
