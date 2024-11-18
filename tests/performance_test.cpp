#include "../src/blitz_logger.hpp"

// helper function: calculate messages per second
double calculateThroughput(size_t messageCount, double durationSeconds)
{
    return static_cast<double>(messageCount) / durationSeconds;
}

// helper function: format number display
std::string formatNumber(double number)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << number;
    return oss.str();
}

// single thread test
void singleThreadTest(size_t messageCount)
{
    auto logger = Logger::getInstance();
    logger->setLogLevel(Logger::Level::INFO);

    std::cout << "\n=== single thread test (" << messageCount << " messages) ===" << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < messageCount; ++i)
    {
        LOG_INFO("test message #{}: this is a performance test log message", i);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(end - start).count();

    double throughput = calculateThroughput(messageCount, duration);
    std::cout << "total time: " << duration << " seconds" << std::endl;
    std::cout << "throughput: " << formatNumber(throughput) << " messages/sec" << std::endl;
}

// multi thread test
void multiThreadTest(size_t messageCount, size_t threadCount)
{
    auto logger = Logger::getInstance();
    logger->setLogLevel(Logger::Level::INFO);

    std::cout << "\n=== multi thread test (" << threadCount << " threads, "
              << messageCount << " messages per thread) ===" << std::endl;

    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    // start multiple threads
    for (size_t t = 0; t < threadCount; ++t)
    {
        threads.emplace_back([t, messageCount, logger]()
                             {
            logger->setModuleName(std::format("thread-{}", t));
            for (size_t i = 0; i < messageCount; ++i) {
                LOG_INFO("thread {} test message #{}: this is a performance test log message", t, i);
            } });
    }

    // wait for all threads to complete
    for (auto &thread : threads)
    {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(end - start).count();

    size_t totalMessages = messageCount * threadCount;
    double throughput = calculateThroughput(totalMessages, duration);
    std::cout << "total time: " << duration << " seconds" << std::endl;
    std::cout << "total throughput: " << formatNumber(throughput) << " messages/sec" << std::endl;
    std::cout << "average throughput per thread: " << formatNumber(throughput / threadCount) << " messages/sec" << std::endl;
}

int main()
{
    // configure logger
    Logger::Config cfg;
    cfg.logDir = "benchmark_logs";
    cfg.filePrefix = "benchmark";
    cfg.maxFileSize = 100 * 1024 * 1024; // 100MB
    cfg.maxFiles = 5;
    cfg.consoleOutput = false; // disable console output for more accurate performance testing

    auto logger = Logger::getInstance(cfg);

    // run tests
    const size_t SINGLE_THREAD_MESSAGES = 100000;
    const size_t MULTI_THREAD_MESSAGES = 50000;
    const std::vector<size_t> THREAD_COUNTS = {2, 4, 8, 16};

    // single thread test
    singleThreadTest(SINGLE_THREAD_MESSAGES);

    // multi thread tests (different thread counts)
    for (size_t threadCount : THREAD_COUNTS)
    {
        multiThreadTest(MULTI_THREAD_MESSAGES, threadCount);
    }

    Logger::destroyInstance();
    return 0;
}