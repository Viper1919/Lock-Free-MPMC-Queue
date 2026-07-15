cmake_minimum_required(VERSION 3.20)
project(lockfree_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Sanitizers are first-class configurations, not an afterthought:
#   cmake -B build-tsan -DLFQ_SANITIZE=thread
#   cmake -B build-asan -DLFQ_SANITIZE=address,undefined
set(LFQ_SANITIZE "" CACHE STRING
    "Comma-separated sanitizer list passed to -fsanitize= (e.g. thread or address,undefined)")

find_package(Threads REQUIRED)

add_library(lfq INTERFACE)
target_include_directories(lfq INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lfq INTERFACE Threads::Threads)

add_library(lfq_build_flags INTERFACE)
target_compile_options(lfq_build_flags INTERFACE -Wall -Wextra -Wpedantic)
if(LFQ_SANITIZE)
  target_compile_options(lfq_build_flags INTERFACE
      -fsanitize=${LFQ_SANITIZE} -g -fno-omit-frame-pointer)
  target_link_options(lfq_build_flags INTERFACE -fsanitize=${LFQ_SANITIZE})
endif()

enable_testing()

add_executable(unit_test tests/unit_test.cpp)
target_link_libraries(unit_test PRIVATE lfq lfq_build_flags)
add_test(NAME unit_test COMMAND unit_test)

add_executable(stress_test tests/stress_test.cpp)
target_link_libraries(stress_test PRIVATE lfq lfq_build_flags)
add_test(NAME stress_test COMMAND stress_test)
set_tests_properties(stress_test PROPERTIES TIMEOUT 600)

add_executable(bench_main bench/bench_main.cpp)
target_link_libraries(bench_main PRIVATE lfq lfq_build_flags)
