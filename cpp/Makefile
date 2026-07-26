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

$(BUILD)/obj/tests_main.o: tests/tests_main.cpp | dirs
	$(CXX) $(CXXFLAGS) -Itests -MMD -MP -c $< -o $@

$(BUILD)/obj/bench_main.o: bench/bench_main.cpp | dirs
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(ENGINE_BIN): $(LIB_OBJS) $(BUILD)/obj/main.o
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(LIB_OBJS) $(BUILD)/obj/tests_main.o
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
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/obj/*.d)
