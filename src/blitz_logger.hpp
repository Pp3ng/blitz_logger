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
#include <list>
#include <unordered_map>

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

    // thread-local buffer
    struct alignas(64) ThreadLocalBuffer
    {
        static constexpr size_t BUFFER_SIZE = 1 << 16;

        alignas(64) std::array<LogMessage, BUFFER_SIZE> messages;
        alignas(64) std::atomic<size_t> head{0};
        alignas(64) std::atomic<size_t> tail{0};
        alignas(64) std::atomic<bool> isActive{true};
        std::thread::id ownerThreadId;

        ThreadLocalBuffer() : ownerThreadId(std::this_thread::get_id()) {}

        void push(LogMessage &&msg) noexcept
        {
            while (true)
            {
                auto current_tail = tail.load(std::memory_order_relaxed);
                auto next_tail = (current_tail + 1) & (BUFFER_SIZE - 1);

                if (next_tail != head.load(std::memory_order_acquire))
                {
                    new (&messages[current_tail]) LogMessage(std::move(msg));
                    tail.store(next_tail, std::memory_order_release);
                    return;
                }

                // if buffer is full, yield and retry
                std::this_thread::yield();
            }
        }

        bool pop(LogMessage &msg) noexcept
        {
            auto current_head = head.load(std::memory_order_relaxed);
            auto current_tail = tail.load(std::memory_order_acquire);

            if (current_head == current_tail)
                return false; // buffer empty

            msg = std::move(messages[current_head]);
            head.store((current_head + 1) & (BUFFER_SIZE - 1), std::memory_order_release);
            return true;
        }

        bool isEmpty() const noexcept
        {
            return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_relaxed);
        }

        size_t size() const noexcept
        {
            auto h = head.load(std::memory_order_relaxed);
            auto t = tail.load(std::memory_order_relaxed);
            return (t >= h) ? (t - h) : (BUFFER_SIZE - (h - t));
        }

        // check if buffer is nearly full (90% capacity)
        bool isNearlyFull() const noexcept
        {
            return size() > (BUFFER_SIZE * 0.9);
        }
    };

    struct BufferRegistry
    {
        std::mutex registryMutex;
        std::list<std::shared_ptr<ThreadLocalBuffer>> buffers;

        void registerBuffer(std::shared_ptr<ThreadLocalBuffer> buffer)
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            buffers.push_back(buffer);
        }

        void unregisterBuffer(std::shared_ptr<ThreadLocalBuffer> buffer)
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            buffers.remove(buffer);
        }

        std::vector<std::shared_ptr<ThreadLocalBuffer>> getAllBuffers()
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            return std::vector<std::shared_ptr<ThreadLocalBuffer>>(buffers.begin(), buffers.end());
        }
    };

    static ThreadLocalBuffer &getThreadLocalBuffer();

    static BufferRegistry bufferRegistry;
    struct ThreadStats
    {
        std::atomic<size_t> messagesProduced{0};
        std::thread::id threadId;
    };

    std::unordered_map<std::thread::id, std::shared_ptr<ThreadStats>> threadStatsMap;
    mutable std::mutex statsMapMutex;

    void updateThreadStats();
    void processMessageBatch(const std::vector<LogMessage> &batch,
                             std::vector<char> &fileBuffer,
                             std::vector<char> &consoleBuffer);
    void processAndClearBatch(std::vector<LogMessage> &batchBuffer,
                              std::vector<char> &fileBuffer,
                              std::vector<char> &consoleBuffer);
    void drainAllBuffers(std::vector<LogMessage> &batchBuffer,
                         std::vector<char> &fileBuffer,
                         std::vector<char> &consoleBuffer);

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
    std::thread loggerThread;
    std::atomic<bool> running{true};
    std::atomic<size_t> currentFileSize{0};
    static inline std::unique_ptr<Logger> instance;
    static inline std::once_flag initFlag;

    // private member functions
    Logger();
    void processLogs();
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

            // get thread-local buffer
            auto &buffer = getThreadLocalBuffer();

            // push message to thread-local buffer
            buffer.push(std::move(msg));
            updateThreadStats();
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