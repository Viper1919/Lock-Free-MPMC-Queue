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

add_executable(reclaim_test tests/reclaim_test.cpp)
target_link_libraries(reclaim_test PRIVATE lfq lfq_build_flags)
add_test(NAME reclaim_test COMMAND reclaim_test)
set_tests_properties(reclaim_test PROPERTIES TIMEOUT 600)

# Adversarial scheduling (phase 3a): the SAME test sources, rebuilt with
# random yields/spins injected into every retry window (inject.hpp) and
# the thread count pushed to 4x oversubscription. Bugs that need a
# preemption inside a two-instruction window fire here or nowhere.
foreach(base stress_test reclaim_test)
  add_executable(adversarial_${base} tests/${base}.cpp)
  target_compile_definitions(adversarial_${base} PRIVATE LFQ_SCHEDULE_FUZZ=1)
  target_link_libraries(adversarial_${base} PRIVATE lfq lfq_build_flags)
  add_test(NAME adversarial_${base} COMMAND adversarial_${base})
  set_tests_properties(adversarial_${base} PROPERTIES TIMEOUT 600)
endforeach()

add_executable(bench_main bench/bench_main.cpp)
target_link_libraries(bench_main PRIVATE lfq lfq_build_flags)
# moodycamel (vendored, bench-only): SYSTEM so -Wall/-Wextra stay ours.
target_include_directories(bench_main SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/concurrentqueue)
