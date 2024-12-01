#include "blitz_logger.hpp"
#include <numeric>
#include <cmath>
#include <random>
#include <iomanip>

using namespace std::chrono_literals;

// test configuration parameters
struct TestConfig
{
    static constexpr size_t DEFAULT_MESSAGE_COUNT = 1'000'000;
    static constexpr int REPEAT_COUNT = 3;
    static constexpr std::array<size_t, 5> THREAD_COUNTS = {1, 2, 4, 8, 16};
    static constexpr std::array<size_t, 3> MESSAGE_SIZES = {64, 128, 256};
};

// performance statistics structure
struct PerformanceStats
{
    double avgThroughput{0.0};
    double stdDevThroughput{0.0};
    double avgLatency{0.0};
    double stdDevLatency{0.0};
};

// test result structure
struct TestResult
{
    size_t threadCount;
    size_t messageSize;
    PerformanceStats stats;
};

// calculate standard deviation
double calculateStdDev(const std::vector<double> &values, double mean)
{
    double sumSquares = 0.0;
    for (double value : values)
    {
        double diff = value - mean;
        sumSquares += diff * diff;
    }
    return std::sqrt(sumSquares / values.size());
}

// generate random message of specified size
std::string generateRandomMessage(size_t size)
{
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    std::string str(size - 1, '\0');
    for (size_t i = 0; i < size - 1; ++i)
    {
        str[i] = charset[dis(gen)];
    }
    return str;
}

// format number with fixed precision
std::string formatNumber(double number)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << number;
    return oss.str();
}

// warm up the logger
void warmUp(Logger *logger)
{
    logger->setModuleName("Warmup");
    for (size_t i = 0; i < 50000; ++i)
    {
        LOG_INFO("warmup message #{}", i);
    }

    std::this_thread::sleep_for(3000ms);
}

// cool down period
void coolDown()
{
    std::this_thread::sleep_for(5000ms);
}

// perform thread test
PerformanceStats performTest(size_t messageCount, size_t threadCount, size_t messageSize)
{
    auto logger = Logger::getInstance();
    logger->setLogLevel(Logger::Level::INFO);
    logger->setModuleName(threadCount == 1 ? "SingleThread" : "MultiThread");

    std::cout << "\n=== Thread test ===" << std::endl;
    std::cout << "Thread count: " << threadCount << std::endl;
    std::cout << "Messages per thread: " << messageCount << std::endl;
    std::cout << "Message size: " << messageSize << " bytes" << std::endl;

    std::vector<double> throughputs;
    std::string testMessage = generateRandomMessage(messageSize);
    std::vector<std::vector<double>> allThreadLatencies;

    // batch size for latency sampling
    const size_t SAMPLE_INTERVAL = 100;

    for (int repeat = 0; repeat < TestConfig::REPEAT_COUNT; ++repeat)
    {
        warmUp(logger);

        std::vector<std::thread> threads;
        std::vector<std::vector<double>> threadLatencies(threadCount);
        std::atomic<bool> startFlag{false};
        std::atomic<size_t> completedThreads{0};

        // pre-reserve space for latencies with sampling
        for (auto &latencies : threadLatencies)
        {
            latencies.reserve(messageCount / SAMPLE_INTERVAL + 1);
        }

        // create threads but don't start logging yet
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([&, t]()
                                 {
                std::vector<double>& latencies = threadLatencies[t];
                
                // wait for start signal
                while (!startFlag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                for (size_t i = 0; i < messageCount; ++i) {
                    if (i % SAMPLE_INTERVAL == 0) {
                        // measure latency only for sampled messages
                        auto msgStart = std::chrono::steady_clock::now();
                        LOG_INFO("Thread {} - {} - {}", t, testMessage, i);
                        auto msgEnd = std::chrono::steady_clock::now();

                        auto latency = std::chrono::duration<double, std::micro>(
                            msgEnd - msgStart).count();
                        if (latency < 1000.0) { // filter out outliers above 1ms
                            latencies.push_back(latency);
                        }
                    } else {
                        // regular logging without latency measurement
                        LOG_INFO("Thread {} - {} - {}", t, testMessage, i);
                    }
                }
                ++completedThreads; });
        }

        // start all threads simultaneously
        auto startTime = std::chrono::steady_clock::now();
        startFlag.store(true, std::memory_order_release);

        for (auto &thread : threads)
        {
            thread.join();
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime).count();

        double throughput = static_cast<double>(messageCount * threadCount) / duration;
        throughputs.push_back(throughput);

        // store latencies from this repeat
        for (auto &latencies : threadLatencies)
        {
            allThreadLatencies.push_back(std::move(latencies));
        }

        coolDown();
    }

    // calculate statistics
    PerformanceStats stats;

    // calculate throughput statistics
    stats.avgThroughput = std::accumulate(throughputs.begin(), throughputs.end(), 0.0) / throughputs.size();
    stats.stdDevThroughput = calculateStdDev(throughputs, stats.avgThroughput);

    // combine all latencies for overall statistics
    std::vector<double> allLatencies;
    size_t totalSamples = 0;

    for (const auto &latencies : allThreadLatencies)
    {
        if (!latencies.empty())
        {
            std::vector<double> sortedLatencies = latencies;
            std::sort(sortedLatencies.begin(), sortedLatencies.end());

            // use middle 90% of samples
            size_t start = sortedLatencies.size() * 0.05;
            size_t end = sortedLatencies.size() * 0.95;

            allLatencies.insert(allLatencies.end(),
                                sortedLatencies.begin() + start,
                                sortedLatencies.begin() + end);
            totalSamples += end - start;
        }
    }

    if (!allLatencies.empty())
    {
        stats.avgLatency = std::accumulate(allLatencies.begin(), allLatencies.end(), 0.0) / allLatencies.size();
        stats.stdDevLatency = calculateStdDev(allLatencies, stats.avgLatency);
    }

    std::cout << "Average throughput: " << formatNumber(stats.avgThroughput)
              << " messages/sec (±" << formatNumber(stats.stdDevThroughput) << ")" << std::endl;
    std::cout << "Average latency: " << formatNumber(stats.avgLatency)
              << " μs (±" << formatNumber(stats.stdDevLatency) << ")" << std::endl;

    return stats;
}

// initialize logger configuration
Logger::Config initializeLoggerConfig()
{
    Logger::Config cfg;
    cfg.logDir = "test_logs";
    cfg.filePrefix = "performance_test";
    cfg.maxFileSize = 100 * 1024 * 1024; // 100MB
    cfg.maxFiles = 5;
    cfg.consoleOutput = false;
    cfg.fileOutput = true;
    cfg.showTimestamp = true;
    cfg.showThreadId = true;
    cfg.showSourceLocation = false;
    cfg.showModuleName = true;
    cfg.showFullPath = false;
    cfg.minLevel = Logger::Level::INFO;
    return cfg;
}

// print test results
void printResults(const std::vector<TestResult> &results)
{
    std::cout << "\n============= Performance Test Summary =============" << std::endl;

    for (size_t threadCount : TestConfig::THREAD_COUNTS)
    {
        std::cout << "\nThread Count: " << threadCount << std::endl;
        std::cout << std::setw(15) << "Message Size"
                  << std::setw(20) << "Throughput (msg/s)"
                  << std::setw(20) << "Latency (μs)" << std::endl;
        std::cout << std::string(55, '-') << std::endl;

        for (const auto &result : results)
        {
            if (result.threadCount == threadCount)
            {
                std::cout << std::setw(15) << result.messageSize
                          << std::setw(20) << formatNumber(result.stats.avgThroughput)
                          << std::setw(20) << formatNumber(result.stats.avgLatency) << std::endl;
            }
        }
    }
}

// run performance tests
std::vector<TestResult> runPerformanceTests()
{
    std::vector<TestResult> results;

    for (size_t threadCount : TestConfig::THREAD_COUNTS)
    {
        for (size_t msgSize : TestConfig::MESSAGE_SIZES)
        {
            TestResult result{threadCount, msgSize,
                              performTest(TestConfig::DEFAULT_MESSAGE_COUNT / threadCount,
                                          threadCount, msgSize)};
            results.push_back(result);
        }
    }

    return results;
}

auto main() -> int
{
    try
    {
        Logger::initialize(initializeLoggerConfig());

        auto results = runPerformanceTests();
        printResults(results);

        std::this_thread::sleep_for(500ms);
        Logger::destroyInstance();
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}