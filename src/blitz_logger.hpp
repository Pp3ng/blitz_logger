#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
        FATAL
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
        size_t queueSize{1024};               // log message queue size
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
    struct LogMessage
    {
        std::string message;
        Level level;
        Context context;
        std::chrono::system_clock::time_point timestamp;

        LogMessage(std::string msg, Level lvl, Context ctx)
            : message(std::move(msg)), level(lvl), context(std::move(ctx)), timestamp(std::chrono::system_clock::now()) {}
    };

    // terminal colors
    static constexpr std::array<const char *, 9> COLORS = {
        "\033[0m",  // reset
        "\033[30m", // black
        "\033[31m", // red
        "\033[32m", // green
        "\033[33m", // yellow
        "\033[34m", // blue
        "\033[35m", // magenta
        "\033[36m", // cyan
        "\033[37m"  // white
    };

    // member variables
    std::queue<LogMessage> messageQueue;
    mutable std::shared_mutex mutex;
    std::ofstream logFile;
    std::condition_variable_any queueCV;
    std::jthread loggerThread;
    std::atomic<bool> running{true};
    size_t currentFileSize{0};
    Config config;
    static inline std::unique_ptr<Logger> instance;
    static inline std::once_flag initFlag;

    // private member functions
    Logger();
    explicit Logger(const Config &cfg);
    void initLogger();
    void processLogs(std::stop_token st);
    void writeLogMessage(const LogMessage &msg);
    void rotateLogFileIfNeeded();
    void cleanOldLogs();
    std::string formatLogMessage(const LogMessage &msg);
    const char *getLevelColor(Level level) const;
    static std::string_view getLevelString(Level level);

public:
    static Logger *getInstance();
    static Logger *getInstance(const Config &cfg);
    static void destroyInstance();

    void configure(const Config &cfg);
    void setLogLevel(Level level);
    void setModuleName(std::string_view module);
    template <typename... Args>
    void log(const std::source_location &loc, Level level, std::format_string<Args...> fmt, Args &&...args)
    {
        if (level < config.minLevel)
            return;

        try
        {
            std::string message = std::format(fmt, std::forward<Args>(args)...);
            Context ctx(loc);

            {
                std::unique_lock lock(mutex);
                while (messageQueue.size() >= config.queueSize)
                {
                    queueCV.wait(lock);
                }
                messageQueue.emplace(std::move(message), level, std::move(ctx));
            }
            queueCV.notify_one();
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