cmake_minimum_required(VERSION 3.16)
project(hft_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(HFT_STATIC_RUNTIME
       "Link libstdc++/libgcc statically (recommended on MinGW: avoids DLL \
lookup at runtime and produces a single self-contained binary)"
       ON)
option(HFT_NATIVE_ARCH "Build with -march=native (faster, not portable)" OFF)

add_library(hft_core STATIC
  src/order_book.cpp
  src/risk.cpp
  src/log.cpp
  src/config.cpp
  src/metrics.cpp
  src/feed.cpp
  src/strategy.cpp
  src/execution.cpp
  src/latency.cpp
  src/oms.cpp
  src/journal.cpp
  src/feed_health.cpp
  src/instrument.cpp
  src/engine.cpp
)
target_include_directories(hft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(hft_core PRIVATE /W4 /O2)
else()
  target_compile_options(hft_core PRIVATE -Wall -Wextra -Wpedantic)
  if(HFT_NATIVE_ARCH)
    target_compile_options(hft_core PRIVATE -march=native)
  endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(hft_core PUBLIC Threads::Threads)

# On MinGW the freshly linked exe otherwise needs libstdc++-6.dll and
# libgcc_s_seh-1.dll resolvable at runtime. Static linking removes that
# dependency entirely.
if(HFT_STATIC_RUNTIME AND (MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU"))
  if(WIN32)
    target_link_options(hft_core INTERFACE -static -static-libgcc -static-libstdc++)
  endif()
endif()

add_executable(hft_engine src/main.cpp)
target_link_libraries(hft_engine PRIVATE hft_core)

# Tests are split by subsystem; tests_main.cpp carries the runner's main().
add_executable(hft_tests
  tests/tests_main.cpp
  tests/test_oms.cpp
  tests/test_journal.cpp
  tests/test_feed_health.cpp
  tests/test_instrument.cpp
  tests/test_multi_instrument.cpp
)
target_include_directories(hft_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(hft_tests PRIVATE hft_core)

add_executable(hft_bench bench/bench_main.cpp)
target_link_libraries(hft_bench PRIVATE hft_core)

enable_testing()
add_test(NAME hft_unit_tests COMMAND hft_tests)
# The test binary writes scratch CSVs into ./build, matching the Makefile layout.
set_tests_properties(hft_unit_tests PROPERTIES
                     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
