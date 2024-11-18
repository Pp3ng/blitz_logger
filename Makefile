# Compiler settings
CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -I./src

# Source files
SOURCES = src/blitz_logger.cpp test.cpp

# Target executable
TARGET = test
TEMP = logs

# Default target
all: $(TARGET)

# Link the target executable
$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

# Clean
clean:
	rm -f $(TARGET)
	rm -rf $(TEMP)

.PHONY: all clean
