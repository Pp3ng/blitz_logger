#include "blitz_logger.hpp"
#include <random>

// unified configuration
Logger::Config getTestConfig()
{
    Logger::Config config;
    config.maxFileSize = 5 * 1024 * 1024; // 5mb
    config.maxFiles = 3;
    config.logDir = "test_logs";
    config.filePrefix = "basic_test";
    config.minLevel = Logger::Level::TRACE;
    config.consoleOutput = true;
    config.fileOutput = true;
    config.useColors = true;
    config.showTimestamp = true;
    config.showThreadId = false;
    config.showSourceLocation = true;
    config.showModuleName = true;
    return config;
}

// helper function
void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// test log levels
void testLogLevels()
{
    Logger::getInstance()->setModuleName("LogLevels");
    LOG_INFO("=== Testing Log Levels ===");
    LOG_TRACE("This is a TRACE message");
    LOG_DEBUG("This is a DEBUG message");
    LOG_INFO("This is an INFO message");
    LOG_WARNING("This is a WARNING message");
    LOG_ERROR("This is an ERROR message");
    LOG_FATAL("This is a FATAL message");
}

// test formatting
void testFormatting()
{
    Logger::getInstance()->setModuleName("Formatting");
    LOG_INFO("=== Testing Formatting ===");

    // basic type formatting
    LOG_INFO("Integer: {}", 42);
    LOG_INFO("Float: {:.2f}", 3.14159);
    LOG_INFO("String: {}", "hello");
    LOG_INFO("Multiple args: {}, {}, {}", 1, "two", 3.0);

    // complex formatting
    LOG_INFO("Right aligned: |{:>10}|", "right");
    LOG_INFO("Hexadecimal: 0x{:X}", 255);
    LOG_INFO("Scientific: {:.2e}", 12345.6789);
}

// test error handling
void testErrorHandling()
{
    Logger::getInstance()->setModuleName("ErrorHandling");
    LOG_INFO("=== Testing Error Handling ===");

    try
    {
        throw std::runtime_error("Test exception");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Caught exception: {}", e.what());
    }
}

auto main(void) -> int
{
    try
    {
        // initialize logger
        auto config = getTestConfig();
        Logger::initialize(config);

        Logger::getInstance()->setModuleName("BasicTest");
        // main test module
        LOG_INFO("Starting logger tests...");

        // run all tests
        testLogLevels();
        sleep_ms(100);

        testFormatting();
        sleep_ms(100);

        testErrorHandling();
        sleep_ms(100);

        Logger::getInstance()->setModuleName("Congratulations");
        LOG_INFO("All tests completed successfully");

        // cleanup
        Logger::destroyInstance();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
}