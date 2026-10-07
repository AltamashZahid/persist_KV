# Build with: make            (Linux/macOS)
#             mingw32-make    (Windows / MinGW)

CXX      ?= g++
CXXFLAGS ?= -std=c++14 -O2 -Wall -Wextra
CPPFLAGS += -Iinclude

LIB_SRC  = src/crc32.cpp src/file.cpp src/failpoint.cpp src/pager.cpp src/wal.cpp src/btree.cpp src/db.cpp src/sync.cpp
HEADERS  = $(wildcard include/persistkv/*.h)

ifeq ($(OS),Windows_NT)
  EXE = .exe
endif

BINS = kvcli$(EXE) bench$(EXE) unit_tests$(EXE) crash_test$(EXE)

all: $(BINS)

kvcli$(EXE): tools/kvcli.cpp $(LIB_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ tools/kvcli.cpp $(LIB_SRC)

bench$(EXE): tools/bench.cpp $(LIB_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ tools/bench.cpp $(LIB_SRC)

unit_tests$(EXE): tests/unit_tests.cpp $(LIB_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ tests/unit_tests.cpp $(LIB_SRC)

crash_test$(EXE): tests/crash_test.cpp $(LIB_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ tests/crash_test.cpp $(LIB_SRC)

test: unit_tests$(EXE) crash_test$(EXE)
	./unit_tests$(EXE)
	./crash_test$(EXE)

clean:
	-rm -f $(BINS)
	-rm -rf test_data bench_data

.PHONY: all test clean
