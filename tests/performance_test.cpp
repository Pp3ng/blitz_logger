#include "blitz_logger.hpp"
#include <numeric>
#include <cmath>
#include <random>
#include <iomanip>
#include <thread>
#include <atomic>
#include <barrier>

// test configuration parameters
struct test_config
{
    static constexpr size_t default_message_count = 1'000'000;
    static constexpr int repeat_count = 5;
    static constexpr std::array<size_t, 5> thread_counts = {1, 2, 4, 8, 16};
    static constexpr std::array<size_t, 3> message_sizes = {64, 128, 256};
    static constexpr double latency_threshold_us = 1000.0; // 1ms
};

// performance statistics
struct perf_stats
{
    double avg_throughput{0.0};
    double std_dev_throughput{0.0};
    double avg_latency{0.0};
    double std_dev_latency{0.0};
    double p95_latency{0.0}; // 95th percentile latency
    double p99_latency{0.0}; // 99th percentile latency
};

// test result structure
struct test_result
{
    size_t thread_count;
    size_t message_size;
    perf_stats stats;
};

// calculate statistics
double calculate_mean(const std::vector<double> &values)
{
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double calculate_std_dev(const std::vector<double> &values, double mean)
{
    double sum_squares = 0.0;
    for (double value : values)
    {
        double diff = value - mean;
        sum_squares += diff * diff;
    }
    return std::sqrt(sum_squares / values.size());
}

double calculate_percentile(std::vector<double> values, double percentile)
{
    if (values.empty())
        return 0.0;

    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(values.size() * percentile);
    return values[index];
}

// generate random message
std::string generate_message(size_t size)
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

// warm up logger
void warm_up(Logger *logger)
{
    logger->setModuleName("Warmup");
    for (size_t i = 0; i < 50000; ++i)
    {
        LOG_INFO("warmup message #{}", i);
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

// cool down period
void cool_down()
{
    std::this_thread::sleep_for(std::chrono::seconds(5));
}

// perform thread test
perf_stats perform_test(size_t message_count, size_t thread_count, size_t message_size)
{
    auto logger = Logger::getInstance();
    logger->setLogLevel(Logger::Level::INFO);
    logger->setModuleName(thread_count == 1 ? "SingleThread" : "MultiThread");

    std::cout << "\n=== Thread test ===" << std::endl;
    std::cout << "Thread count: " << thread_count << std::endl;
    std::cout << "Messages per thread: " << message_count << std::endl;
    std::cout << "Message size: " << message_size << " bytes" << std::endl;

    std::vector<double> throughputs;
    std::vector<double> all_latencies;
    std::string test_message = generate_message(message_size);

    for (int repeat = 0; repeat < test_config::repeat_count; ++repeat)
    {
        warm_up(logger);

        std::vector<std::thread> threads;
        std::vector<std::vector<double>> thread_latencies(thread_count);
        std::barrier sync_point(thread_count + 1);
        std::atomic<bool> start_flag{false};

        // create threads
        for (size_t t = 0; t < thread_count; ++t)
        {
            threads.emplace_back([&, t]()
                                 {
                auto& latencies = thread_latencies[t];
                latencies.reserve(message_count);
                
                sync_point.arrive_and_wait();
                
                for (size_t i = 0; i < message_count; ++i) {
                    auto start = std::chrono::steady_clock::now();
                    LOG_INFO("Thread {} - {} - {}", t, test_message, i);
                    auto end = std::chrono::steady_clock::now();
                    
                    auto latency = std::chrono::duration<double, std::micro>(
                        end - start).count();
                    latencies.push_back(latency);
                } });
        }

        // start timing
        auto start_time = std::chrono::steady_clock::now();
        sync_point.arrive_and_wait();

        // wait for completion
        for (auto &thread : threads)
        {
            thread.join();
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration<double>(end_time - start_time).count();

        // calculate throughput
        double throughput = static_cast<double>(message_count * thread_count) / duration;
        throughputs.push_back(throughput);

        // collect latencies
        for (const auto &latencies : thread_latencies)
        {
            all_latencies.insert(all_latencies.end(), latencies.begin(), latencies.end());
        }

        cool_down();
    }

    // calculate statistics
    perf_stats stats;
    stats.avg_throughput = calculate_mean(throughputs);
    stats.std_dev_throughput = calculate_std_dev(throughputs, stats.avg_throughput);

    if (!all_latencies.empty())
    {
        stats.avg_latency = calculate_mean(all_latencies);
        stats.std_dev_latency = calculate_std_dev(all_latencies, stats.avg_latency);
        stats.p95_latency = calculate_percentile(all_latencies, 0.95);
        stats.p99_latency = calculate_percentile(all_latencies, 0.99);
    }

    // print results
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Average throughput: " << stats.avg_throughput
              << " msg/s (±" << stats.std_dev_throughput << ")" << std::endl;
    std::cout << "Average latency: " << stats.avg_latency
              << " μs (±" << stats.std_dev_latency << ")" << std::endl;
    std::cout << "P95 latency: " << stats.p95_latency << " μs" << std::endl;
    std::cout << "P99 latency: " << stats.p99_latency << " μs" << std::endl;

    return stats;
}

// print test results in table format
void print_results(const std::vector<test_result> &results)
{
    std::cout << "\n============= Performance Test Summary =============" << std::endl;

    for (size_t thread_count : test_config::thread_counts)
    {
        std::cout << "\nThread Count: " << thread_count << std::endl;
        std::cout << std::setw(15) << "Message Size"
                  << std::setw(20) << "Throughput (msg/s)"
                  << std::setw(20) << "Latency (μs)"
                  << std::setw(15) << "P95 (μs)"
                  << std::setw(15) << "P99 (μs)" << std::endl;
        std::cout << std::string(85, '-') << std::endl;

        for (const auto &result : results)
        {
            if (result.thread_count == thread_count)
            {
                std::cout << std::fixed << std::setprecision(2)
                          << std::setw(15) << result.message_size
                          << std::setw(20) << result.stats.avg_throughput
                          << std::setw(20) << result.stats.avg_latency
                          << std::setw(15) << result.stats.p95_latency
                          << std::setw(15) << result.stats.p99_latency << std::endl;
            }
        }
    }
}

// run all performance tests
std::vector<test_result> run_performance_tests()
{
    std::vector<test_result> results;

    for (size_t thread_count : test_config::thread_counts)
    {
        for (size_t msg_size : test_config::message_sizes)
        {
            test_result result{
                thread_count,
                msg_size,
                perform_test(test_config::default_message_count / thread_count,
                             thread_count, msg_size)};
            results.push_back(result);
        }
    }

    return results;
}

auto main() -> int
{
    try
    {
        // initialize logger
        Logger::Config cfg;
        cfg.logDir = "test_logs";
        cfg.filePrefix = "perf_test";
        cfg.consoleOutput = false;
        Logger::initialize(cfg);

        // run tests
        auto results = run_performance_tests();
        print_results(results);

        // cleanup
        Logger::destroyInstance();
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}