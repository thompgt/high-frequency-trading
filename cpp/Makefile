# Plain GNU Make build for the HFT C++ engine.
#
# CMake is the primary build (see CMakeLists.txt) but this Makefile exists so
# the project builds with nothing but g++ and make -- which is all some
# environments have. Both produce the same three binaries.
#
#   make            build engine, tests and benchmarks
#   make test       build and run the unit tests
#   make bench      build and run the benchmarks
#   make run        build and run the engine end to end
#   make clean
#
# Static linking note: on MinGW/MSYS2 a dynamically linked binary needs
# libstdc++-6.dll and libgcc_s_seh-1.dll on PATH, and some Windows security
# tooling flags the freshly built exe. Linking the runtime in statically
# produces a single self-contained binary and sidesteps both problems.

CXX      ?= g++
CXXSTD   ?= c++17
OPT      ?= -O2
CXXFLAGS ?= -std=$(CXXSTD) $(OPT) -Wall -Wextra -Wpedantic -Werror -Iinclude
LDFLAGS  ?=
LDLIBS   ?=

UNAME_S := $(shell uname -s 2>/dev/null)
ifneq (,$(findstring MINGW,$(UNAME_S)))
  STATIC_FLAGS := -static -static-libgcc -static-libstdc++
else ifneq (,$(findstring MSYS,$(UNAME_S)))
  STATIC_FLAGS := -static -static-libgcc -static-libstdc++
else ifeq ($(OS),Windows_NT)
  STATIC_FLAGS := -static -static-libgcc -static-libstdc++
else
  STATIC_FLAGS :=
  LDLIBS += -pthread
endif

LDFLAGS += $(STATIC_FLAGS)

BUILD := build
LIB_SRCS := $(wildcard src/*.cpp)
LIB_SRCS := $(filter-out src/main.cpp,$(LIB_SRCS))
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(LIB_SRCS))

# The test suite is split across translation units by subsystem; tests_main.cpp
# holds the runner's main().
TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILD)/obj/test_%.o,$(TEST_SRCS))

ENGINE_BIN := $(BUILD)/hft_engine
TEST_BIN   := $(BUILD)/hft_tests
BENCH_BIN  := $(BUILD)/hft_bench

.PHONY: all test bench run clean dirs

all: $(ENGINE_BIN) $(TEST_BIN) $(BENCH_BIN)

dirs:
	@mkdir -p $(BUILD)/obj

$(BUILD)/obj/%.o: src/%.cpp | dirs
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/main.o: src/main.cpp | dirs
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/test_%.o: tests/%.cpp | dirs
	$(CXX) $(CXXFLAGS) -Itests -MMD -MP -c $< -o $@

$(BUILD)/obj/bench_main.o: bench/bench_main.cpp | dirs
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(ENGINE_BIN): $(LIB_OBJS) $(BUILD)/obj/main.o
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(LIB_OBJS) $(TEST_OBJS)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BENCH_BIN): $(LIB_OBJS) $(BUILD)/obj/bench_main.o
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

run: $(ENGINE_BIN)
	./$(ENGINE_BIN)

clean:
	rm -rf $(BUILD) build-asan build-ubsan build-tsan build-hardened

# ---------------------------------------------------------------- sanitizers
#
# ASan/UBSan/TSan need libasan/libubsan/libtsan, which the MSYS2/MinGW
# toolchain does NOT ship -- `-fsanitize=address` fails at link time with
# "cannot find -lasan" on Windows. These targets are therefore for Linux/CI
# (where CI does run them); `make hardened` below is the equivalent that
# actually works on MinGW.
#
# Each sanitizer builds into its own directory so the objects never mix with
# the optimised build.

SAN_CXXFLAGS := -std=$(CXXSTD) -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests
SAN_SRCS := $(LIB_SRCS) $(TEST_SRCS)

.PHONY: asan ubsan tsan hardened sanitizers

asan:
	@mkdir -p build-asan
	$(CXX) $(SAN_CXXFLAGS) -fsanitize=address -fsanitize=undefined $(SAN_SRCS) -o build-asan/hft_tests $(LDLIBS)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build-asan/hft_tests

ubsan:
	@mkdir -p build-ubsan
	$(CXX) $(SAN_CXXFLAGS) -fsanitize=undefined -fno-sanitize-recover=all $(SAN_SRCS) -o build-ubsan/hft_tests $(LDLIBS)
	./build-ubsan/hft_tests

tsan:
	@mkdir -p build-tsan
	$(CXX) $(SAN_CXXFLAGS) -fsanitize=thread $(SAN_SRCS) -o build-tsan/hft_tests $(LDLIBS)
	TSAN_OPTIONS=halt_on_error=1 ./build-tsan/hft_tests

sanitizers: asan ubsan tsan

# Works everywhere, including MinGW, because none of it needs a sanitizer
# runtime library:
#   -fsanitize-undefined-trap-on-error  turns UB checks into a trap instruction
#   -D_GLIBCXX_DEBUG                    bounds-checks every std:: container
#   -D_GLIBCXX_ASSERTIONS               enables libstdc++ precondition checks
#   -fstack-protector-strong            catches stack smashing
# A failure here aborts the process rather than printing a nice report, so run
# it under a debugger if something trips.
hardened:
	@mkdir -p build-hardened
	$(CXX) -std=$(CXXSTD) -O1 -g -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests \
	  -fsanitize=undefined -fsanitize-undefined-trap-on-error \
	  -D_GLIBCXX_DEBUG -D_GLIBCXX_ASSERTIONS -fstack-protector-strong \
	  $(SAN_SRCS) -o build-hardened/hft_tests $(LDFLAGS) $(LDLIBS)
	./build-hardened/hft_tests

-include $(wildcard $(BUILD)/obj/*.d)
