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

# -march=native applies to every build type, not just Release. The hand written
# kernels in src/ml compile to a scalar fallback without it, and a sanitizer
# build that quietly tests the fallback instead of the code that ships is worse
# than no sanitizer build at all.
target_compile_options(ltx_flags INTERFACE
  $<$<BOOL:${LTX_NATIVE}>:-march=native>
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-fno-omit-frame-pointer>
  $<$<CONFIG:Debug>:-O0>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
)
target_link_options(ltx_flags INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# Appended after everything else, so it wins. scripts/run_determinism.sh uses it
# to build the same sources at a different optimisation level and require the
# same event hashes, which is a check that nothing here depends on undefined
# behaviour the optimiser is free to read differently.
set(LTX_EXTRA_FLAGS "" CACHE STRING "flags appended last, for the determinism check")
if(LTX_EXTRA_FLAGS)
  separate_arguments(LTX_EXTRA_FLAGS_LIST NATIVE_COMMAND "${LTX_EXTRA_FLAGS}")
  target_compile_options(ltx_flags INTERFACE ${LTX_EXTRA_FLAGS_LIST})
endif()

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

add_library(ltx_wire STATIC
  src/wire/decoder.cpp
)
target_link_libraries(ltx_wire PUBLIC ltx_flags ltx_engine)

add_executable(bench bench/bench_main.cpp)
target_link_libraries(bench PRIVATE ltx_engine ltx_wire)

add_executable(replay src/replay/replay_main.cpp)
target_link_libraries(replay PRIVATE ltx_engine ltx_data)

add_executable(sim src/sim/sim_main.cpp)
target_link_libraries(sim PRIVATE ltx_engine ltx_data)

add_executable(itchgen tools/itchgen.cpp)
target_link_libraries(itchgen PRIVATE ltx_engine ltx_data ltx_wire)

add_executable(itchfeed tools/itchfeed.cpp)
target_link_libraries(itchfeed PRIVATE ltx_engine ltx_data ltx_wire)

add_executable(mlgen tools/mlgen.cpp)
target_link_libraries(mlgen PRIVATE ltx_engine ltx_data)

add_executable(mltrain tools/mltrain.cpp)
target_link_libraries(mltrain PRIVATE ltx_engine ltx_data)

add_executable(ticktotrade tools/ticktotrade.cpp)
target_link_libraries(ticktotrade PRIVATE ltx_engine ltx_wire)

add_executable(ouchgw tools/ouchgw.cpp)
target_link_libraries(ouchgw PRIVATE ltx_engine ltx_wire)

add_executable(liveshadow tools/liveshadow.cpp)
target_link_libraries(liveshadow PRIVATE ltx_engine ltx_data)


include(CTest)
enable_testing()
foreach(t test_order_book test_spsc test_id_map test_reconstruct test_wire test_ml
        test_ouch)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE ltx_engine ltx_data ltx_wire)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
