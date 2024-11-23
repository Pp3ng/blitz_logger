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
    // crash handler function
    static void terminateHandler()
    {
        try
        {
            if (auto logger = Logger::getInstance())
            {
                logger->fatal(std::source_location::current(),
                              "Application is terminating due to fatal error");

                LogMessage msg;
                while (logger->buffer.pop(msg))
                {
                    logger->writeLogMessage(msg);
                }

                if (logger->logFile.is_open())
                {
                    logger->logFile.flush();
                }
            }
        }
        catch (...)
        {
            std::cerr << "Fatal error occurred while handling program termination" << std::endl;
        }
        std::abort();
    }
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

    // ring buffer implementation
    static constexpr size_t BUFFER_SIZE = 1 << 17; // 2^17

    struct alignas(64) RingBuffer
    {
        alignas(64) std::array<LogMessage, BUFFER_SIZE> messages;
        alignas(64) std::atomic<size_t> head{0};
        alignas(64) std::atomic<size_t> tail{0};

        bool full() const
        {
            auto current_tail = tail.load(std::memory_order_relaxed);
            auto next_tail = (current_tail + 1) & (BUFFER_SIZE - 1);
            return next_tail == head.load(std::memory_order_acquire);
        }

        bool empty() const
        {
            return head.load(std::memory_order_relaxed) ==
                   tail.load(std::memory_order_acquire);
        }

        bool push(LogMessage &&msg)
        {
            auto current_tail = tail.load(std::memory_order_relaxed);
            auto next_tail = (current_tail + 1) & (BUFFER_SIZE - 1);

            if (next_tail == head.load(std::memory_order_acquire))
            {
                return false;
            }

            new (&messages[current_tail]) LogMessage(std::move(msg));
            std::atomic_thread_fence(std::memory_order_release);
            tail.store(next_tail, std::memory_order_release);
            return true;
        }

        bool pop(LogMessage &msg)
        {
            auto current_head = head.load(std::memory_order_relaxed);

            if (current_head == tail.load(std::memory_order_acquire))
            {
                return false;
            }

            msg = std::move(messages[current_head]);

            std::atomic_thread_fence(std::memory_order_release);
            head.store((current_head + 1) & (BUFFER_SIZE - 1),
                       std::memory_order_release);
            return true;
        }

        size_t size() const
        {
            auto h = head.load(std::memory_order_acquire);
            auto t = tail.load(std::memory_order_acquire);
            return (t - h) & (BUFFER_SIZE - 1);
        }
    };

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

    // member variables
    Config config;
    RingBuffer buffer;
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
    void writeLogMessage(const LogMessage &msg);
    void rotateLogFileIfNeeded();
    void cleanOldLogs();
    std::string formatLogMessage(const LogMessage &msg) noexcept;
    const char *getLevelColor(Level level) const;

public:
    static Logger *getInstance();
    static void initialize(const Config &cfg);
    static void destroyInstance();

    void configure(const Config &cfg);
    void setLogLevel(Level level);
    void setModuleName(std::string_view module);
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

            constexpr int MAX_RETRIES = 100;
            int retries = 0;
            while (!buffer.push(std::move(msg)))
            {
                if (++retries > MAX_RETRIES)
                {
                    std::this_thread::yield();
                    retries = 0;
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