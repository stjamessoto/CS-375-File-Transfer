# TCP File Transfer lab: builds both versions.
#
#   make            build everything
#   make testfiles  create sample files in server_files/ (10 MB, 100 MB, empty)
#   make test       build and run the automated test suite
#   make bench      run the performance comparison (writes results/benchmark_results.md)
#   make clean      remove binaries, downloads and test output

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

COMMON = common/net_utils.h common/protocol.h common/sha256.h

BINS = version1/server version1/client \
       version2_threadpool/server version2_threadpool/client

all: $(BINS)
	@echo "Build complete:"; for b in $(BINS); do echo "  ./$$b"; done

version1/server: version1/server.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) $< -o $@

version1/client: version1/client.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) $< -o $@

version2_threadpool/server: version2_threadpool/server.cpp version2_threadpool/threadpool.cpp version2_threadpool/threadpool.h $(COMMON)
	$(CXX) $(CXXFLAGS) version2_threadpool/server.cpp version2_threadpool/threadpool.cpp -o $@

version2_threadpool/client: version2_threadpool/client.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) $< -o $@

testfiles:
	./scripts/make_test_files.sh

test: all
	./scripts/run_tests.sh

bench: all
	./scripts/benchmark.sh

clean:
	rm -f $(BINS)
	rm -rf downloads test_output server_files/uploads

.PHONY: all testfiles test bench clean
