#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <cstring>

class Logger
{
public:
    // log levels
    enum class Level
    {
        TRACE,
        DEBUG,
        INFO,
        WARNING,
        ERROR,
        FATAL,
        STEP
    };

    // logger configuration
    struct Config
    {
        std::string logDir{"logs"};           // log directory
        std::string filePrefix{"app"};        // log file prefix
        size_t maxFileSize{10 * 1024 * 1024}; // max single file size (10MB)
        size_t maxFiles{5};                   // max number of files to keep
        Level minLevel{Level::INFO};          // minimum log level
        bool consoleOutput{true};             // enable console output
        bool fileOutput{true};                // enable file output
        bool useColors{true};                 // enable colored output
        bool showTimestamp{true};             // show timestamp in logs
        bool showThreadId{true};              // show thread id in logs
        bool showSourceLocation{true};        // show source location in logs
        bool showModuleName{true};            // show module name in logs
        bool showFullPath{false};             // show full file paths in logs
    };

private:
    // log context information
    struct Context
    {
        std::string module;       // module name
        std::string function;     // function name
        std::string file;         // file name
        int line;                 // line number
        std::thread::id threadId; // thread id

        Context(const std::source_location &loc = std::source_location::current())
            : module(getThreadLocalModuleName()),
              function(loc.function_name()),
              file(loc.file_name()),
              line(loc.line()),
              threadId(std::this_thread::get_id())
        {
        }

    private:
        static std::string &getThreadLocalModuleName()
        {
            static thread_local std::string currentModule = "Default Module";
            return currentModule;
        }

        friend class Logger;
    };

    // log message structure
    struct alignas(64) LogMessage
    {
        std::string message;
        Level level;
        Context context;
        std::chrono::system_clock::time_point timestamp;

        LogMessage() = default;

        LogMessage(const LogMessage &) = delete;
        LogMessage &operator=(const LogMessage &) = delete;

        LogMessage(LogMessage &&) noexcept = default;
        LogMessage &operator=(LogMessage &&) noexcept = default;

        LogMessage(std::string msg, Level lvl, Context ctx)
            : message(std::move(msg)), level(lvl), context(std::move(ctx)), timestamp(std::chrono::system_clock::now())
        {
        }

        ~LogMessage() = default;
    };

    static constexpr size_t FULL_THRESHOLD = 0.8f; // 80%
    // shard buffer
    struct alignas(64) BufferShard
    {
        static constexpr size_t SHARD_SIZE = 1 << 18; // 2^18 = 262,144(256KB)
        alignas(64) std::array<LogMessage, SHARD_SIZE> messages;
        alignas(64) std::atomic<size_t> head{0};
        alignas(64) std::atomic<size_t> tail{0};

        bool push(LogMessage &&msg) noexcept
        {
            auto current_tail = tail.load(std::memory_order_relaxed);
            auto next_tail = (current_tail + 1) & (SHARD_SIZE - 1);

            if (next_tail == head.load(std::memory_order_relaxed)) // judge whether the shard is full
            {
                if (next_tail == head.load(std::memory_order_acquire))
                {
                    return false;
                }
            }

            new (&messages[current_tail]) LogMessage(std::move(msg));
            tail.store(next_tail, std::memory_order_release);
            return true;
        }

        bool pop(LogMessage &msg) noexcept
        {
            auto current_head = head.load(std::memory_order_relaxed);

            if (current_head == tail.load(std::memory_order_relaxed))
            {
                if (current_head == tail.load(std::memory_order_acquire))
                {
                    return false;
                }
            }

            msg = std::move(messages[current_head]);
            head.store((current_head + 1) & (SHARD_SIZE - 1), std::memory_order_release);
            return true;
        }

        bool isFull() const noexcept // judge whether the shard is full(based on threshold)
        {
            auto current_tail = tail.load(std::memory_order_relaxed);
            auto current_head = head.load(std::memory_order_relaxed);

            size_t used;
            if (current_tail >= current_head)
            {
                used = current_tail - current_head;
            }
            else
            {
                used = SHARD_SIZE - (current_head - current_tail);
            }

            return (static_cast<float>(used) / SHARD_SIZE) >= FULL_THRESHOLD;
        }
    };

    static constexpr size_t NUM_SHARDS = 32;    // 32 shards (256KB * 32 = 8MB)
    std::array<BufferShard, NUM_SHARDS> shards; // shards array
    std::atomic<size_t> nextShardId{0};
    static thread_local size_t currentShardId;

    struct alignas(64) ShardStats
    {
        std::atomic<size_t> pushAttempts{0};
        std::atomic<size_t> pushFailures{0};
        std::atomic<size_t> maxSize{0};
    };
    std::array<ShardStats, NUM_SHARDS> shardStats;

    size_t getShardId() noexcept;
    void processMessageBatch(const std::vector<LogMessage> &batch,
                             std::vector<char> &fileBuffer,
                             std::vector<char> &consoleBuffer);
    void drainRemainingMessages(std::vector<LogMessage> &batchBuffer,
                                std::vector<char> &fileBuffer,
                                std::vector<char> &consoleBuffer);
    void updateShardStats(size_t shardId, bool pushSuccess);

    // terminal colors
    static constexpr std::array<const char *, 10> COLORS = {
        "\033[0m",   // reset
        "\033[30m",  // black
        "\033[31m",  // red
        "\033[32m",  // green
        "\033[33m",  // yellow
        "\033[34m",  // blue
        "\033[35m",  // magenta
        "\033[36m",  // cyan
        "\033[37m",  // white
        "\033[1;31m" // bright red
    };

    static constexpr size_t COLOR_RESET = 0;   // reset
    static constexpr size_t COLOR_TRACE = 7;   // cyan
    static constexpr size_t COLOR_DEBUG = 6;   // magenta
    static constexpr size_t COLOR_INFO = 3;    // green
    static constexpr size_t COLOR_WARNING = 4; // yellow
    static constexpr size_t COLOR_ERROR = 2;   // red
    static constexpr size_t COLOR_FATAL = 9;   // bright red
    static constexpr size_t COLOR_STEP = 5;    // blue

    static constexpr std::array<std::string_view, 7> LEVEL_STRINGS = {
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "STEP"};

    // member variables
    Config config;
    mutable std::shared_mutex configMutex; // for config changes
    std::ofstream logFile;
    std::jthread loggerThread;
    std::atomic<bool> running{true};
    std::atomic<size_t> currentFileSize{0};
    static inline std::unique_ptr<Logger> instance;
    static inline std::once_flag initFlag;

    // private member functions
    Logger();
    void processLogs(std::stop_token st);
    void rotateLogFileIfNeeded();
    void cleanOldLogs();
    void formatLogMessage(const LogMessage &msg, std::vector<char> &buffer) noexcept;
    const char *getLevelColor(Level level) const;

public:
    static Logger *getInstance();
    static void initialize(const Config &cfg);
    static void destroyInstance();

    void configure(const Config &cfg);
    void setLogLevel(Level level);
    void setModuleName(std::string_view module);
    void printStats() const;
    friend std::unique_ptr<Logger> std::make_unique<Logger>();

    // log method
    template <typename... Args>
    void log(const std::source_location &loc, Level level, std::format_string<Args...> fmt, Args &&...args)
    {
        if (level < config.minLevel)
            return;

        try
        {
            LogMessage msg{
                std::format(fmt, std::forward<Args>(args)...),
                level,
                Context(loc)};

            const size_t originalShardId = getShardId();
            size_t currentShard = originalShardId;
            bool messageLogged = false;

            // until message is logged
            while (!messageLogged)
            {
                for (size_t attempt = 0; attempt < NUM_SHARDS; ++attempt)
                {
                    if (shards[currentShard].push(std::move(msg)))
                    {
                        updateShardStats(currentShard, true);
                        messageLogged = true;
                        break;
                    }

                    updateShardStats(currentShard, false);
                    currentShard = (currentShard + 1) % NUM_SHARDS;
                }

                if (!messageLogged)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Logging error: " << e.what() << std::endl;
        }
    }

    template <typename... Args>
    void trace(const std::source_location &loc, std::format_string<Args...> fmt, Args &&...args)
    {
        log(loc, Level::TRACE, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(const std::source_location &loc, std::format_string<Args...> fmt, Args &&...args)
    {
        log(loc, Level::DEBUG, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(const std::source_location &loc, std::format_string<Args...> fmt, Args &&...args)
    {
        log(loc, Level::INFO, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warning(const std::source_location &loc, std::format_string<Args...> fmt, Args &&...args)
    {
        log(loc, Level::WARNING, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(const std::source_location &loc, std::format_string<Args...> fmt, Args &&...args)
    {
        log(loc, Level::ERROR, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void fatal(const std::source_location &loc, std::format_string<Args...> fmt, Args &&...args)
    {
        log(loc, Level::FATAL, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void step(int stepNum, const std::source_location &loc, std::format_string<Args...> fmt, Args &&...args)
    {
        log(loc, Level::STEP, "[Step {}] {}",
            stepNum,
            std::format(fmt, std::forward<Args>(args)...));
    }

    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;
};

// helper macros for logging
#define LOG_TRACE(...) Logger::getInstance()->trace(std::source_location::current(), __VA_ARGS__)
#define LOG_DEBUG(...) Logger::getInstance()->debug(std::source_location::current(), __VA_ARGS__)
#define LOG_INFO(...) Logger::getInstance()->info(std::source_location::current(), __VA_ARGS__)
#define LOG_WARNING(...) Logger::getInstance()->warning(std::source_location::current(), __VA_ARGS__)
#define LOG_ERROR(...) Logger::getInstance()->error(std::source_location::current(), __VA_ARGS__)
#define LOG_FATAL(...) Logger::getInstance()->fatal(std::source_location::current(), __VA_ARGS__)
#define LOG_STEP(num, ...) Logger::getInstance()->step(num, std::source_location::current(), __VA_ARGS__)