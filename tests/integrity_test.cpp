#include "blitz_logger.hpp"
#include <regex>
#include <set>

// function to verify log integrity
[[nodiscard]]
bool verifyLogIntegrity(const std::string &logPath, int expectedCount)
{
    std::ifstream logFile(logPath);
    if (!logFile)
    {
        std::cerr << "Failed to open log file: " << logPath << std::endl;
        return false;
    }

    std::string line;
    std::set<int> numbers;
    std::regex numberPattern(R"(Number: (\d+))");
    std::smatch matches;

    // extract all numbers from log file
    while (std::getline(logFile, line))
    {
        if (std::regex_search(line, matches, numberPattern))
        {
            numbers.insert(std::stoi(matches[1]));
        }
    }

    // check if all numbers from 1 to expectedCount are present
    bool isComplete = true;
    for (int i = 1; i <= expectedCount; ++i)
    {
        if (numbers.find(i) == numbers.end())
        {
            std::cout << "Missing number: " << i << std::endl;
            isComplete = false;
        }
    }

    // check for any extra numbers
    for (int num : numbers)
    {
        if (num < 1 || num > expectedCount)
        {
            std::cout << "Unexpected number found: " << num << std::endl;
            isComplete = false;
        }
    }

    std::cout << "Numbers found: " << numbers.size() << "/" << expectedCount << std::endl;
    return isComplete;
}

auto main(void) -> int
{
    // configure logger
    Logger::Config cfg;
    cfg.logDir = "test_logs";
    cfg.filePrefix = "integrity_test";
    cfg.maxFileSize = 1024 * 1024 * 1500; // 1.5GB to avoid file rotation during test
    cfg.minLevel = Logger::Level::INFO;
    cfg.consoleOutput = false; // disable console output for better performance
    cfg.fileOutput = true;

    // initialize logger
    Logger::initialize(cfg);

    const int MAX_COUNT = 10000000; // 10 million log messages

    // record start time for writing
    auto write_start = std::chrono::high_resolution_clock::now();

    // write log messages
    for (int i = 1; i <= MAX_COUNT; ++i)
    {
        LOG_INFO("Number: {}", i);

        // print progress every 10000 messages
        if (i % 10000 == 0)
        {
            std::cout << "Progress: " << i << "/" << MAX_COUNT << std::endl;
        }
    }

    // record end time for writing
    auto write_end = std::chrono::high_resolution_clock::now();
    auto write_duration = std::chrono::duration_cast<std::chrono::milliseconds>(write_end - write_start);

    // calculate write performance
    double write_seconds = write_duration.count() / 1000.0;
    double write_msgs_per_sec = MAX_COUNT / write_seconds;

    std::cout << "Write time: " << std::fixed << std::setprecision(2) << write_seconds << " seconds" << std::endl;
    std::cout << "Write speed: " << std::fixed << std::setprecision(2) << write_msgs_per_sec << " msgs/sec" << std::endl;

    // verify log integrity
    std::string logPath = std::format("{}/{}.log", cfg.logDir, cfg.filePrefix);
    std::cout << "\nVerifying log integrity..." << std::endl;
    bool integrityCheck = verifyLogIntegrity(logPath, MAX_COUNT);

    std::cout << "Integrity check: " << (integrityCheck ? "PASSED" : "FAILED") << std::endl;

    // cleanup
    Logger::destroyInstance();

    return integrityCheck ? 0 : 1;
}
