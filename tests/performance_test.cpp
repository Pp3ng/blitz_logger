#include "blitz_logger.hpp"

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
    logger->setModuleName("SingleThread");

    std::cout << "\n=== single thread test (" << messageCount << " messages) ===" << std::endl;

    // warmup
    for (size_t i = 0; i < 1000; ++i)
    {
        LOG_INFO("warmup message #{}", i);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < messageCount; ++i)
    {
        LOG_INFO("test message #{}", i);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double>(end - start).count();

    double throughput = calculateThroughput(messageCount, duration);
    std::cout << "total time: " << duration << " seconds" << std::endl;
    std::cout << "throughput: " << formatNumber(throughput) << " messages/sec" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// multi thread test
void multiThreadTest(size_t messageCount, size_t threadCount)
{
    auto logger = Logger::getInstance();
    logger->setLogLevel(Logger::Level::INFO);

    std::cout << "\n=== multi thread test (" << threadCount << " threads, "
              << messageCount << " messages per thread) ===" << std::endl;

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    // warmup{
    {
        std::thread warmup([logger]()
                           {
            logger->setModuleName("Warmup");
            for (size_t i = 0; i < 1000; ++i) {
                LOG_INFO("warmup message #{}", i);
            } });
        warmup.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto start = std::chrono::high_resolution_clock::now();

    // start multiple threads
    for (size_t t = 0; t < threadCount; ++t)
    {
        threads.emplace_back([t, messageCount, logger]()
                             {
            logger->setModuleName(std::format("Thread-{}", t));
            for (size_t i = 0; i < messageCount; ++i) {
                LOG_INFO("thread {} message #{}", t, i);
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
    std::cout << "average throughput per thread: " << formatNumber(throughput / threadCount)
              << " messages/sec" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

auto main(void) -> int
{
    try
    {
        // configure logger
        Logger::Config cfg;
        cfg.logDir = "test_logs";
        cfg.filePrefix = "performance_test";
        cfg.maxFileSize = 100 * 1024 * 1024; // 100MB
        cfg.maxFiles = 5;
        cfg.consoleOutput = false; // disable console output for performance
        cfg.fileOutput = true;
        cfg.showTimestamp = true;
        cfg.showThreadId = true;
        cfg.showSourceLocation = false; // disable for better performance
        cfg.showModuleName = true;
        cfg.showFullPath = false;
        cfg.minLevel = Logger::Level::INFO;

        Logger::initialize(cfg);

        // run tests
        const size_t SINGLE_THREAD_MESSAGES = 1'000'000;
        const size_t MULTI_THREAD_MESSAGES = 500'000;
        const std::vector<size_t> THREAD_COUNTS = {2, 4, 8, 16};

        // single thread test
        singleThreadTest(SINGLE_THREAD_MESSAGES);

        // multi thread tests
        for (size_t threadCount : THREAD_COUNTS)
        {
            multiThreadTest(MULTI_THREAD_MESSAGES, threadCount);
            // cool down
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        Logger::destroyInstance();
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}