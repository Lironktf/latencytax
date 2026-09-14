# Built separately with clang, because libFuzzer is a clang feature and the rest
# of the project builds with whatever the machine's default compiler is.
#
#   CC=clang CXX=clang++ cmake -B build-fuzz fuzz -DLTX_ROOT=..
#   cmake --build build-fuzz -j
cmake_minimum_required(VERSION 3.16)
project(ltx_fuzz CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT LTX_ROOT)
  set(LTX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
get_filename_component(LTX_ROOT "${LTX_ROOT}" ABSOLUTE)

find_package(ZLIB REQUIRED)

add_library(ltx_fuzz_lib STATIC
  ${LTX_ROOT}/src/engine/order_book.cpp
  ${LTX_ROOT}/src/wire/decoder.cpp
  ${LTX_ROOT}/src/util/gzline.cpp
  ${LTX_ROOT}/src/replay/loader.cpp
)
target_include_directories(ltx_fuzz_lib PUBLIC ${LTX_ROOT}/src)
target_compile_options(ltx_fuzz_lib PUBLIC
  -O1 -g -fno-omit-frame-pointer -march=native
  -fsanitize=fuzzer-no-link,address,undefined)
target_link_libraries(ltx_fuzz_lib PUBLIC ZLIB::ZLIB)

foreach(t fuzz_itch fuzz_soup fuzz_book fuzz_json)
  add_executable(${t} ${CMAKE_CURRENT_SOURCE_DIR}/${t}.cpp)
  target_link_libraries(${t} PRIVATE ltx_fuzz_lib)
  target_link_options(${t} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
