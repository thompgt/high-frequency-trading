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
option(HFT_WERROR "Treat warnings as errors (the Makefile build always does)" ON)
set(HFT_SANITIZE "" CACHE STRING
    "Sanitizers to build with, e.g. \"address;undefined\" or \"thread\". Empty \
disables. Needs libasan/libubsan/libtsan, which MinGW does not ship.")

# Warnings belong to every translation unit in the project, not just the ones
# in hft_core. Hanging them off an INTERFACE target that all four consumers
# link is what makes that true: previously these were PRIVATE to hft_core, so
# main.cpp, the seven test TUs and the bench all compiled with no -Wall at all
# and only the Linux Makefile job ever saw a warning.
add_library(hft_warnings INTERFACE)

if(MSVC)
  target_compile_options(hft_warnings INTERFACE /W4)
  if(HFT_WERROR)
    target_compile_options(hft_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(hft_warnings INTERFACE -Wall -Wextra -Wpedantic)
  if(HFT_WERROR)
    target_compile_options(hft_warnings INTERFACE -Werror)
  endif()
  if(HFT_NATIVE_ARCH)
    target_compile_options(hft_warnings INTERFACE -march=native)
  endif()
endif()

# Sanitizers have to reach the same four targets, and have to be on the link
# line as well as the compile line, so they live here too. Configure with e.g.
#   cmake -S cpp -B cpp/build-asan -DHFT_SANITIZE="address;undefined" \
#         -DHFT_STATIC_RUNTIME=OFF
# Static runtime linking and the sanitizer runtimes do not mix, hence the
# second flag; `make asan` in the Makefile is the equivalent for that build.
if(HFT_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "HFT_SANITIZE is only wired up for GCC/Clang")
  endif()
  string(REPLACE ";" "," HFT_SANITIZE_LIST "${HFT_SANITIZE}")
  target_compile_options(hft_warnings INTERFACE
                         -fsanitize=${HFT_SANITIZE_LIST} -fno-omit-frame-pointer -g)
  target_link_options(hft_warnings INTERFACE -fsanitize=${HFT_SANITIZE_LIST})
endif()

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
  src/reconcile.cpp
  src/engine.cpp
)
target_include_directories(hft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(hft_core PRIVATE hft_warnings)

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
target_link_libraries(hft_engine PRIVATE hft_core hft_warnings)

# Tests are split by subsystem; tests_main.cpp carries the runner's main().
add_executable(hft_tests
  tests/tests_main.cpp
  tests/test_oms.cpp
  tests/test_journal.cpp
  tests/test_feed_health.cpp
  tests/test_instrument.cpp
  tests/test_multi_instrument.cpp
  tests/test_reconcile.cpp
  tests/test_order_timeout.cpp
)
target_include_directories(hft_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(hft_tests PRIVATE hft_core hft_warnings)

add_executable(hft_bench bench/bench_main.cpp)
target_link_libraries(hft_bench PRIVATE hft_core hft_warnings)

enable_testing()
add_test(NAME hft_unit_tests COMMAND hft_tests)
# The test binary writes scratch CSVs into ./build, matching the Makefile layout.
set_tests_properties(hft_unit_tests PROPERTIES
                     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
