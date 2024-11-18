# Blitz Logger

A modern, thread-safe, and feature-rich C++ logging library designed for high performance and ease of use. Without any external dependencies, Blitz Logger is a lightweight and efficient solution for logging in C++ applications.

## Features

- **Multiple Log Levels**: TRACE, DEBUG, INFO, WARNING, ERROR, and FATAL
- **Asynchronous Logging**: Non-blocking logging operations with background processing
- **File Management**:
  - Automatic log file rotation based on file size
  - Configurable maximum file size and count
  - Timestamp-based file naming
- **Flexible Output**:
  - Simultaneous console and file output
  - Colored console output support
  - Customizable output format
- **Rich Context Information**:
  - Timestamps
  - Thread IDs
  - Source location (file, line, function)
  - Module names
- **Thread Safety**: Safe for multi-threaded applications
- **Modern C++ Features**: Uses C++20 features including std::format and std::source_location

## Sample

![Sample](sample.png)

## Performance Benchmark

![Performance](performance.png)

## Requirements

- C++20 compatible compiler

## Usage

### Basic Example

```cpp
#include "blitz_logger.hpp"

int main() {
    // Use default configuration
    auto logger = Logger::getInstance();

    // Log messages with different levels
    LOG_INFO("Application started");
    LOG_DEBUG("Debug information: {}", 42);
    LOG_ERROR("Something went wrong: {}", "error message");

    return 0;
}
```

### Custom Configuration

```cpp
Logger::Config config;
config.logDir = "custom_logs";
config.filePrefix = "myapp";
config.maxFileSize = 5 * 1024 * 1024;  // 5MB
config.maxFiles = 3;
config.minLevel = Logger::Level::DEBUG;
config.useColors = true;

auto logger = Logger::getInstance(config);
```

### Module-based Logging

```cpp
void someFunction() {
    Logger::getInstance()->setModuleName("NetworkModule");
    LOG_INFO("Network initialization complete");
}
```

## Configuration Options

| Option             | Description                         | Default |
| ------------------ | ----------------------------------- | ------- |
| logDir             | Log directory path                  | "logs"  |
| filePrefix         | Log file name prefix                | "app"   |
| maxFileSize        | Maximum size per log file           | 10MB    |
| maxFiles           | Maximum number of log files to keep | 5       |
| minLevel           | Minimum log level to process        | INFO    |
| consoleOutput      | Enable console output               | true    |
| fileOutput         | Enable file output                  | true    |
| useColors          | Enable colored console output       | true    |
| showTimestamp      | Show timestamp in logs              | true    |
| showThreadId       | Show thread ID in logs              | true    |
| showSourceLocation | Show source file and line           | true    |
| showModuleName     | Show module name in logs            | true    |
| queueSize          | Maximum size of async queue         | 1024    |

## License

MIT License

## Contributing

Contributions are welcome! Please feel free to submit pull requests.
