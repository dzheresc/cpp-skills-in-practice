cmake_minimum_required(VERSION 3.20)
project(exchange CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Both engines are always built with the same flags so measured differences
# come from the code, not the compiler settings (skill 6.10: one change at a time).
# -O2 is the baseline; set EXCH_NATIVE=ON to add -march=native (skill 3.8:
# only if the deployment target matches the build host).
option(EXCH_NATIVE "Build with -march=native" OFF)
option(EXCH_SANITIZE "Build tests with ASan + UBSan" OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic)
if(EXCH_NATIVE)
  add_compile_options(-march=native)
endif()
if(EXCH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_library(exch_naive src/naive_exchange.cc)
target_include_directories(exch_naive PUBLIC include ${CMAKE_SOURCE_DIR})

add_executable(scenario_test tests/scenario_test.cc)
target_link_libraries(scenario_test exch_naive)
add_executable(differential_test tests/differential_test.cc)
target_link_libraries(differential_test exch_naive)

add_executable(bench_engine bench/bench_engine.cc)
target_link_libraries(bench_engine exch_naive)
add_executable(bench_pipeline bench/bench_pipeline.cc)
target_link_libraries(bench_pipeline exch_naive Threads::Threads)

enable_testing()
add_test(NAME scenario COMMAND scenario_test)
add_test(NAME differential COMMAND differential_test)
add_executable(bench_jitter bench/bench_jitter.cc)
target_include_directories(bench_jitter PRIVATE include ${CMAKE_SOURCE_DIR})
