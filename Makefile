# compiler and flags
CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -pthread
INCLUDES = -Isrc

# source files
LIB_SOURCE = src/blitz_logger.cpp
BASIC_TEST = tests/basic_test.cpp
PERF_TEST = tests/performance_test.cpp
INTEGRITY_TEST = tests/integrity_test.cpp

# targets
BASIC_TARGET = basic_test
PERF_TARGET = perf_test
INTEGRITY_TARGET = integrity_test

# default target
all: basic performance integrity

# build basic test
basic: $(LIB_SOURCE) $(BASIC_TEST)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $(BASIC_TARGET)

# build performance test
performance: $(LIB_SOURCE) $(PERF_TEST)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $(PERF_TARGET)

# build integrity test
integrity: $(LIB_SOURCE) $(INTEGRITY_TEST)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $(INTEGRITY_TARGET)

# run basic test
run_basic: basic
	./$(BASIC_TARGET)

# run performance test
run_perf: performance
	./$(PERF_TARGET)

# run integrity test
run_integrity: integrity
	./$(INTEGRITY_TARGET)

# clean
clean:
	rm -f $(BASIC_TARGET) $(PERF_TARGET) $(INTEGRITY_TARGET)
	rm -rf test_logs

.PHONY: all basic performance integrity run_basic run_perf run_integrity clean