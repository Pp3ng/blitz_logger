#include "blitz_logger.hpp"
#include <regex>
#include <set>
#include <ranges>

[[nodiscard]]
bool verifyLogIntegrity(const std::string &logPath, int expectedCount)
{
    std::ifstream logFile(logPath);
    if (!logFile)
    {
        std::cerr << std::format("[ERROR] Failed to open log file: {}\n", logPath);
        return false;
    }

    std::set<int> numbers;
    std::regex numberPattern(R"(Number: (\d+))");

    for (std::string line; std::getline(logFile, line);)
    {
        std::smatch matches;
        if (std::regex_search(line, matches, numberPattern))
            numbers.insert(std::stoi(matches[1]));
    }

    auto missingNumbers =
        std::views::iota(1, expectedCount + 1) | std::views::filter([&numbers](int n)
                                                                    { return !numbers.contains(n); });

    auto extraNumbers = numbers | std::views::filter([expectedCount](int n)
                                                     { return n < 1 || n > expectedCount; });

    for (int n : missingNumbers)
        std::cout << std::format("[WARNING] Missing number: {}\n", n);

    for (int n : extraNumbers)
        std::cout << std::format("[WARNING] Unexpected number: {}\n", n);

    std::cout << std::format("[INFO] Numbers found: {}/{}\n", numbers.size(), expectedCount);

    return std::ranges::empty(missingNumbers) && std::ranges::empty(extraNumbers);
}

// function to log messages with progress
void logMessages(int maxCount)
{
    auto writeStart = std::chrono::high_resolution_clock::now();

    for (int i = 1; i <= maxCount; ++i)
    {
        LOG_INFO("Number: {}", i);

        if (i % 100000 == 0)
            std::cout << std::format("\r[PROGRESS] Writing: {}/{}...", i, maxCount) << std::flush;
    }

    auto writeEnd = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(writeEnd - writeStart);
    double writeSpeed = maxCount / (duration.count() / 1000.0);

    std::cout << std::format("\n[INFO] Write completed in {:.2f} seconds, {:.2f} msgs/sec\n", duration.count() / 1000.0, writeSpeed);
}

auto main(void) -> int
{
    // Logger configuration
    Logger::Config cfg = {
        .logDir = "test_logs",
        .filePrefix = "integrity_test",
        .maxFileSize = 1'500'000'000, // 1.5 GB to ensure no rotation during test
        .minLevel = Logger::Level::INFO,
        .consoleOutput = false,
        .fileOutput = true};

    Logger::initialize(cfg);

    const int MAX_COUNT = 10'000'000;

    // log messages
    logMessages(MAX_COUNT);

    Logger::getInstance()->printStats();

    // destroy logger instance
    Logger::destroyInstance();

    // verify log integrity
    std::string logPath = std::format("{}/{}.log", cfg.logDir, cfg.filePrefix);
    std::cout << "\n[INFO] Verifying log integrity...\n";

    bool integrityCheck = verifyLogIntegrity(logPath, MAX_COUNT);
    std::cout << std::format("[RESULT] Integrity check: {}\n", integrityCheck ? "PASSED" : "FAILED");

    return integrityCheck ? 0 : 1;
}