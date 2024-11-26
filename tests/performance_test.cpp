#include "blitz_logger.hpp"
#include <numeric>
#include <cmath>
#include <random>
#include <sys/resource.h>
#include <iomanip>

using namespace std::chrono_literals;

// test configuration parameters
struct TestConfig
{
    static constexpr size_t DEFAULT_MESSAGE_COUNT = 1'000'000;
    static constexpr int REPEAT_COUNT = 5;
    static constexpr std::array<size_t, 4> THREAD_COUNTS = {2, 4, 8, 16};
    static constexpr std::array<size_t, 2> MESSAGE_SIZES = {64, 256};
    static constexpr std::array<size_t, 3> BUFFER_SIZES = {1 << 16, 1 << 17, 1 << 18};
};

// performance statistics structure
struct PerformanceStats
{
    double avgThroughput{0.0};
    double stdDevThroughput{0.0};
    double avgLatency{0.0};
    double stdDevLatency{0.0};
    double cpuUsage{0.0};
    size_t memoryUsage{0};
    size_t lostMessages{0};
};

// store test results
struct TestResult
{
    size_t threadCount;
    size_t messageSize;
    PerformanceStats stats;
};

// cpu usage monitor class
class CpuMonitor
{
    struct timespec startTime
    {
    };
    clock_t startClock{};

public:
    void start()
    {
        clock_gettime(CLOCK_REALTIME, &startTime);
        startClock = clock();
    }

    double getUsage()
    {
        struct timespec endTime
        {
        };
        clock_gettime(CLOCK_REALTIME, &endTime);
        clock_t endClock = clock();

        double realTime = (endTime.tv_sec - startTime.tv_sec) +
                          (endTime.tv_nsec - startTime.tv_nsec) / 1e9;
        double cpuTime = static_cast<double>(endClock - startClock) / CLOCKS_PER_SEC;

        return (cpuTime / realTime) * 100.0;
    }
};

// get current memory usage in KB
size_t getCurrentMemoryUsage()
{
    struct rusage usage
    {
    };
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;
}

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
    for (size_t i = 0; i < 10000; ++i)
    {
        LOG_INFO("warmup message #{}", i);
    }
    std::this_thread::sleep_for(2000ms);
}

// cool down period
void coolDown()
{
    std::this_thread::sleep_for(2000ms);
}

// single thread performance test
PerformanceStats singleThreadTest(size_t messageCount, size_t messageSize)
{
    auto logger = Logger::getInstance();
    logger->setLogLevel(Logger::Level::INFO);
    logger->setModuleName("SingleThread");

    std::cout << "\n=== Single thread test ===" << std::endl;
    std::cout << "Message count: " << messageCount << std::endl;
    std::cout << "Message size: " << messageSize << " bytes" << std::endl;

    std::vector<double> throughputs;
    std::vector<double> latencies;
    CpuMonitor cpuMonitor;
    size_t initialMemory = getCurrentMemoryUsage();
    size_t peakMemory = initialMemory;

    std::string testMessage = generateRandomMessage(messageSize);

    for (int repeat = 0; repeat < TestConfig::REPEAT_COUNT; ++repeat)
    {
        warmUp(logger);

        cpuMonitor.start();
        auto startTime = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < messageCount; ++i)
        {
            auto msgStart = std::chrono::high_resolution_clock::now();
            LOG_INFO("{} - {}", testMessage, i);
            auto msgEnd = std::chrono::high_resolution_clock::now();

            auto latency = std::chrono::duration<double, std::micro>(msgEnd - msgStart).count();
            latencies.push_back(latency);

            size_t currentMemory = getCurrentMemoryUsage();
            peakMemory = std::max(peakMemory, currentMemory);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime).count();

        double throughput = static_cast<double>(messageCount) / duration;
        throughputs.push_back(throughput);

        coolDown();
    }

    // calculate statistics
    PerformanceStats stats;
    stats.avgThroughput = std::accumulate(throughputs.begin(), throughputs.end(), 0.0) / throughputs.size();
    stats.stdDevThroughput = calculateStdDev(throughputs, stats.avgThroughput);
    stats.avgLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    stats.stdDevLatency = calculateStdDev(latencies, stats.avgLatency);
    stats.cpuUsage = cpuMonitor.getUsage();
    stats.memoryUsage = peakMemory - initialMemory;

    // output results
    std::cout << "Average throughput: " << formatNumber(stats.avgThroughput) << " messages/sec (±"
              << formatNumber(stats.stdDevThroughput) << ")" << std::endl;
    std::cout << "Average latency: " << formatNumber(stats.avgLatency) << " μs (±"
              << formatNumber(stats.stdDevLatency) << ")" << std::endl;
    std::cout << "CPU usage: " << formatNumber(stats.cpuUsage) << "%" << std::endl;
    std::cout << "Memory usage: " << stats.memoryUsage << " KB" << std::endl;

    return stats;
}

// multi-thread performance test
PerformanceStats multiThreadTest(size_t messageCount, size_t threadCount, size_t messageSize)
{
    auto logger = Logger::getInstance();
    logger->setLogLevel(Logger::Level::INFO);

    std::cout << "\n=== Multi thread test ===" << std::endl;
    std::cout << "Thread count: " << threadCount << std::endl;
    std::cout << "Messages per thread: " << messageCount << std::endl;
    std::cout << "Message size: " << messageSize << " bytes" << std::endl;

    std::vector<double> throughputs;
    std::vector<double> latencies;
    CpuMonitor cpuMonitor;
    std::atomic<size_t> initialMemory = getCurrentMemoryUsage();
    std::atomic<size_t> peakMemory = initialMemory.load();
    std::atomic<size_t> lostMessages{0};

    std::string testMessage = generateRandomMessage(messageSize);

    for (int repeat = 0; repeat < TestConfig::REPEAT_COUNT; ++repeat)
    {
        warmUp(logger);

        std::vector<std::thread> threads;
        threads.reserve(threadCount);

        std::vector<std::vector<double>> threadLatencies(threadCount);
        std::atomic<bool> startFlag{false};

        cpuMonitor.start();
        auto startTime = std::chrono::high_resolution_clock::now();

        // launch threads
        for (size_t t = 0; t < threadCount; ++t)
        {
            threads.emplace_back([t, messageCount, &testMessage, &threadLatencies,
                                  &startFlag, &lostMessages, &peakMemory, logger]()
                                 {
                logger->setModuleName(std::format("Thread-{}", t));
                
                // wait for start signal
                while (!startFlag.load(std::memory_order_relaxed)) {
                    std::this_thread::yield();
                }

                for (size_t i = 0; i < messageCount; ++i) {
                    auto msgStart = std::chrono::high_resolution_clock::now();
                    
                    try {
                        LOG_INFO("{} - Thread {} message #{}", testMessage, t, i);
                    } catch (...) {
                        lostMessages.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    auto msgEnd = std::chrono::high_resolution_clock::now();
                    auto latency = std::chrono::duration<double, std::micro>(msgEnd - msgStart).count();
                    threadLatencies[t].push_back(latency);

                    size_t currentMemory = getCurrentMemoryUsage();
                    size_t expected = peakMemory.load(std::memory_order_relaxed);
                    while (currentMemory > expected && 
                           !peakMemory.compare_exchange_weak(expected, currentMemory, 
                                                           std::memory_order_relaxed)) {
                        // retry until success
                    }
                } });
        }

        // synchronize thread start
        startFlag.store(true, std::memory_order_release);

        // wait for all threads to complete
        for (auto &thread : threads)
        {
            thread.join();
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double>(endTime - startTime).count();

        // collect latency data from all threads
        for (const auto &threadLatency : threadLatencies)
        {
            latencies.insert(latencies.end(), threadLatency.begin(), threadLatency.end());
        }

        double throughput = static_cast<double>(messageCount * threadCount) / duration;
        throughputs.push_back(throughput);

        coolDown();
    }

    // calculate statistics
    PerformanceStats stats;
    stats.avgThroughput = std::accumulate(throughputs.begin(), throughputs.end(), 0.0) / throughputs.size();
    stats.stdDevThroughput = calculateStdDev(throughputs, stats.avgThroughput);
    stats.avgLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    stats.stdDevLatency = calculateStdDev(latencies, stats.avgLatency);
    stats.cpuUsage = cpuMonitor.getUsage();
    stats.memoryUsage = peakMemory.load() - initialMemory.load();
    stats.lostMessages = lostMessages.load();

    // output results
    std::cout << "Average throughput: " << formatNumber(stats.avgThroughput) << " messages/sec (±"
              << formatNumber(stats.stdDevThroughput) << ")" << std::endl;
    std::cout << "Average latency: " << formatNumber(stats.avgLatency) << " μs (±"
              << formatNumber(stats.stdDevLatency) << ")" << std::endl;
    std::cout << "CPU usage: " << formatNumber(stats.cpuUsage) << "%" << std::endl;
    std::cout << "Memory usage: " << stats.memoryUsage << " KB" << std::endl;
    std::cout << "Lost messages: " << stats.lostMessages << std::endl;

    return stats;
}

// main test function
int main()
{
    try
    {
        // basic logger configuration
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

        Logger::initialize(cfg);

        std::vector<TestResult> results;

        // single thread tests with different message sizes
        for (size_t msgSize : TestConfig::MESSAGE_SIZES)
        {
            TestResult result{1, msgSize,
                              singleThreadTest(TestConfig::DEFAULT_MESSAGE_COUNT, msgSize)};
            results.push_back(result);
        }

        // multi-thread tests with different thread counts and message sizes
        for (size_t threadCount : TestConfig::THREAD_COUNTS)
        {
            for (size_t msgSize : TestConfig::MESSAGE_SIZES)
            {
                TestResult result{threadCount, msgSize,
                                  multiThreadTest(TestConfig::DEFAULT_MESSAGE_COUNT / threadCount, threadCount, msgSize)};
                results.push_back(result);
            }
        }

        std::cout << "\n============= Performance Test Summary =============" << std::endl;

        std::cout << "\nSingle Thread Results:" << std::endl;
        std::cout << std::setw(15) << "Message Size"
                  << std::setw(20) << "Throughput (msg/s)"
                  << std::setw(20) << "Latency (μs)"
                  << std::setw(15) << "CPU (%)"
                  << std::setw(15) << "Memory (KB)"
                  << std::setw(15) << "Lost Msgs" << std::endl;
        std::cout << std::string(100, '-') << std::endl;

        for (const auto &result : results)
        {
            if (result.threadCount == 1)
            {
                std::cout << std::setw(15) << result.messageSize
                          << std::setw(20) << formatNumber(result.stats.avgThroughput)
                          << std::setw(20) << formatNumber(result.stats.avgLatency)
                          << std::setw(15) << formatNumber(result.stats.cpuUsage)
                          << std::setw(15) << result.stats.memoryUsage
                          << std::setw(15) << result.stats.lostMessages << std::endl;
            }
        }

        std::cout << "\nMulti-Thread Results:" << std::endl;
        for (size_t threadCount : TestConfig::THREAD_COUNTS)
        {
            std::cout << "\nThread Count: " << threadCount << std::endl;
            std::cout << std::setw(15) << "Message Size"
                      << std::setw(20) << "Throughput (msg/s)"
                      << std::setw(20) << "Latency (μs)"
                      << std::setw(15) << "CPU (%)"
                      << std::setw(15) << "Memory (KB)"
                      << std::setw(15) << "Lost Msgs" << std::endl;
            std::cout << std::string(100, '-') << std::endl;

            for (const auto &result : results)
            {
                if (result.threadCount == threadCount)
                {
                    std::cout << std::setw(15) << result.messageSize
                              << std::setw(20) << formatNumber(result.stats.avgThroughput)
                              << std::setw(20) << formatNumber(result.stats.avgLatency)
                              << std::setw(15) << formatNumber(result.stats.cpuUsage)
                              << std::setw(15) << result.stats.memoryUsage
                              << std::setw(15) << result.stats.lostMessages << std::endl;
                }
            }
        }

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