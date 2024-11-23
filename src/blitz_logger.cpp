#include "blitz_logger.hpp"
#include <iostream>
#include <sstream>

// default constructor
Logger::Logger() : config(Config{}), running(true)
{
}

void Logger::processLogs(std::stop_token st)
{
    constexpr size_t BATCH_SIZE = 1024;

    // Use std::vector for dynamic memory management, and std::span for views
    std::vector<LogMessage> batchBuffer;
    batchBuffer.reserve(BATCH_SIZE);

    std::vector<char> fileBuffer;
    fileBuffer.reserve(1024 * 1024); // Reserve space for 1MB file buffer
    std::vector<char> consoleBuffer;
    consoleBuffer.reserve(1024 * 1024); // Reserve space for 1MB console buffer

    auto processBuffer = [&](std::span<LogMessage> buffer)
    {
        if (buffer.empty())
            return;

        fileBuffer.clear();
        consoleBuffer.clear();

        for (const auto &msg : buffer)
        {
            if (config.fileOutput)
            {
                std::string formattedMessage = formatLogMessage(msg);
                fileBuffer.insert(fileBuffer.end(), formattedMessage.begin(), formattedMessage.end());
                fileBuffer.push_back('\n');
            }

            if (config.consoleOutput)
            {
                if (config.useColors)
                {
                    std::string levelColor = getLevelColor(msg.level);
                    consoleBuffer.insert(consoleBuffer.end(), levelColor.begin(), levelColor.end());
                    std::string formattedMessage = formatLogMessage(msg);
                    consoleBuffer.insert(consoleBuffer.end(), formattedMessage.begin(), formattedMessage.end());
                    consoleBuffer.insert(consoleBuffer.end(), COLORS[0], COLORS[0] + strlen(COLORS[0]));
                    consoleBuffer.push_back('\n');
                }
                else
                {
                    std::string formattedMessage = formatLogMessage(msg);
                    consoleBuffer.insert(consoleBuffer.end(), formattedMessage.begin(), formattedMessage.end());
                    consoleBuffer.push_back('\n');
                }
            }
        }

        if (config.fileOutput && logFile.is_open() && !fileBuffer.empty())
        {
            logFile.write(fileBuffer.data(), fileBuffer.size());
            logFile.flush();
            currentFileSize += fileBuffer.size();
            rotateLogFileIfNeeded();
        }

        if (config.consoleOutput && !consoleBuffer.empty())
        {
            std::cout.write(consoleBuffer.data(), consoleBuffer.size());
            std::cout.flush();
        }
    };

    while (!st.stop_requested())
    {
        LogMessage msg;
        while (batchBuffer.size() < BATCH_SIZE && buffer.pop(msg))
        {
            batchBuffer.push_back(std::move(msg));
        }

        if (!batchBuffer.empty())
        {
            processBuffer(std::span<LogMessage>(batchBuffer.data(), batchBuffer.size()));
            batchBuffer.clear();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // process remaining logs
    LogMessage msg;
    while (buffer.pop(msg))
    {
        batchBuffer.push_back(std::move(msg));
        if (batchBuffer.size() >= BATCH_SIZE)
        {
            processBuffer(std::span<LogMessage>(batchBuffer.data(), batchBuffer.size()));
            batchBuffer.clear();
        }
    }

    if (!batchBuffer.empty())
    {
        processBuffer(std::span<LogMessage>(batchBuffer.data(), batchBuffer.size()));
    }
}

[[nodiscard]]
std::string Logger::formatLogMessage(const LogMessage &msg) noexcept
{
    // most log messages are short, so stack allocation is more likely to be sufficient
    // using 16-byte alignment for potential SIMD operations
    alignas(16) char stack_buffer[1024];
    size_t offset = 0;
    const size_t max_stack_size = sizeof(stack_buffer);

    // estimate the total size needed for the message
    // base size (256) includes space for timestamp, level, thread id, etc.
    const size_t estimated_size = 256 + msg.message.size();

    // slow path: message too large for stack buffer
    if (estimated_size > max_stack_size) [[unlikely]]
    {
        // allocate heap buffer with exact size estimation
        std::string heap_buffer;
        heap_buffer.reserve(estimated_size);

        // format timestamp if enabled
        if (config.showTimestamp) [[likely]]
        {
            auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          msg.timestamp.time_since_epoch()) %
                      1000;

            // format date and time
            char time_buffer[32];
            size_t time_len = std::strftime(time_buffer, sizeof(time_buffer),
                                            "[%Y-%m-%d %H:%M:%S.", std::localtime(&time));
            heap_buffer.append(time_buffer, time_len);

            // format milliseconds
            char ms_buffer[8];
            int ms_len = std::snprintf(ms_buffer, sizeof(ms_buffer),
                                       "%03d] ", static_cast<int>(ms.count()));
            heap_buffer.append(ms_buffer, ms_len);
        }

        // define log levels array

        // format log level
        const auto &level_str = LEVEL_STRINGS[static_cast<size_t>(msg.level)];
        char level_buffer[32];
        int level_len = std::snprintf(level_buffer, sizeof(level_buffer),
                                      "[%.*s] ", static_cast<int>(level_str.size()),
                                      level_str.data());
        heap_buffer.append(level_buffer, level_len);

        // format thread id if enabled
        if (config.showThreadId) [[likely]]
        {
            char thread_buffer[32];
            int thread_len = std::snprintf(thread_buffer, sizeof(thread_buffer),
                                           "[T-%zu] ",
                                           std::hash<std::thread::id>{}(msg.context.threadId));
            heap_buffer.append(thread_buffer, thread_len);
        }

        // format module name if enabled and not empty
        if (config.showModuleName && !msg.context.module.empty()) [[likely]]
        {
            char module_buffer[256];
            int module_len = std::snprintf(module_buffer, sizeof(module_buffer),
                                           "[%s] ", msg.context.module.c_str());
            heap_buffer.append(module_buffer, module_len);
        }

        // format source location if enabled
        if (config.showSourceLocation) [[likely]]
        {
            std::string_view file(msg.context.file);
            // extract filename from path if full path is not needed
            if (!config.showFullPath) [[likely]]
            {
                if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) [[likely]]
                {
                    file = file.substr(pos + 1);
                }
            }

            char loc_buffer[512];
            int loc_len = std::snprintf(loc_buffer, sizeof(loc_buffer),
                                        "[%.*s:%d] ", static_cast<int>(file.size()),
                                        file.data(), msg.context.line);
            heap_buffer.append(loc_buffer, loc_len);
        }

        // append the actual message content
        heap_buffer.append(msg.message);
        return heap_buffer;
    }

    // fast path: using stack buffer
    // format timestamp if enabled
    if (config.showTimestamp) [[likely]]
    {
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;

        offset += std::strftime(stack_buffer + offset, max_stack_size - offset,
                                "[%Y-%m-%d %H:%M:%S.", std::localtime(&time));
        offset += std::snprintf(stack_buffer + offset, max_stack_size - offset,
                                "%03d] ", static_cast<int>(ms.count()));
    }

    const auto &level_str = LEVEL_STRINGS[static_cast<size_t>(msg.level)];
    offset += std::snprintf(stack_buffer + offset, max_stack_size - offset,
                            "[%.*s] ", static_cast<int>(level_str.size()),
                            level_str.data());

    // format thread id if enabled
    if (config.showThreadId) [[likely]]
    {
        offset += std::snprintf(stack_buffer + offset, max_stack_size - offset,
                                "[T-%zu] ",
                                std::hash<std::thread::id>{}(msg.context.threadId));
    }

    // format module name if enabled and not empty
    if (config.showModuleName && !msg.context.module.empty()) [[likely]]
    {
        offset += std::snprintf(stack_buffer + offset, max_stack_size - offset,
                                "[%s] ", msg.context.module.c_str());
    }

    // format source location if enabled
    if (config.showSourceLocation) [[likely]]
    {
        std::string_view file(msg.context.file);
        if (!config.showFullPath) [[likely]]
        {
            if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) [[likely]]
            {
                file = file.substr(pos + 1);
            }
        }
        offset += std::snprintf(stack_buffer + offset, max_stack_size - offset,
                                "[%.*s:%d] ", static_cast<int>(file.size()),
                                file.data(), msg.context.line);
    }

    // check remaining space for message content
    const size_t remaining = max_stack_size - offset;
    if (msg.message.size() < remaining) [[likely]]
    {
        // fast path: direct copy to stack buffer
        std::memcpy(stack_buffer + offset, msg.message.data(), msg.message.size());
        offset += msg.message.size();
        return std::string(stack_buffer, offset);
    }
    else
    {
        // slow path: combine stack buffer content with remaining message
        std::string result;
        result.reserve(offset + msg.message.size());
        result.append(stack_buffer, offset);
        result.append(msg.message);
        return result;
    }
}

void Logger::rotateLogFileIfNeeded()
{
    if (!config.fileOutput || currentFileSize < config.maxFileSize)
    {
        return;
    }

    logFile.close();

    // generate new filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S",
                  std::localtime(&time));

    std::string oldFile = std::format("{}/{}.log", config.logDir, config.filePrefix);
    std::string newFile = std::format("{}/{}_{}.log",
                                      config.logDir, config.filePrefix, timestamp);

    // rename current log file
    if (std::filesystem::exists(oldFile))
    {
        std::filesystem::rename(oldFile, newFile);
    }

    // open new log file
    logFile.open(oldFile, std::ios::app);
    currentFileSize = 0;

    // clean old logs
    cleanOldLogs();
}

void Logger::cleanOldLogs()
{
    std::vector<std::filesystem::path> logFiles;

    // collect all log files
    for (const auto &entry : std::filesystem::directory_iterator(config.logDir))
    {
        if (entry.path().extension() == ".log" &&
            entry.path().stem().string().starts_with(config.filePrefix))
        {
            logFiles.push_back(entry.path());
        }
    }

    // sort by modification time (newest first)
    std::sort(logFiles.begin(), logFiles.end(),
              [](const auto &a, const auto &b)
              {
                  return std::filesystem::last_write_time(a) >
                         std::filesystem::last_write_time(b);
              });

    // remove old files
    while (logFiles.size() > config.maxFiles)
    {
        std::filesystem::remove(logFiles.back());
        logFiles.pop_back();
    }
}

[[nodiscard]]
const char *Logger::getLevelColor(Level level) const
{
    switch (level)
    {
    case Level::TRACE:
        return COLORS[COLOR_TRACE];
    case Level::DEBUG:
        return COLORS[COLOR_DEBUG];
    case Level::INFO:
        return COLORS[COLOR_INFO];
    case Level::WARNING:
        return COLORS[COLOR_WARNING];
    case Level::ERROR:
        return COLORS[COLOR_ERROR];
    case Level::FATAL:
        return COLORS[COLOR_FATAL];
    case Level::STEP:
        return COLORS[COLOR_STEP];
    default:
        return COLORS[COLOR_RESET];
    }
}

Logger *Logger::getInstance()
{
    if (!instance)
    {
        throw std::runtime_error("Logger not initialized");
    }
    return instance.get();
}
void Logger::initialize(const Config &cfg)
{
    std::call_once(initFlag, [&cfg]()
                   {
        instance = std::make_unique<Logger>();
        instance->configure(cfg); // configure logger
        
        instance->loggerThread = std::jthread([instance = instance.get()](std::stop_token st) {
            instance->processLogs(st);
        });
        
        instance->log(std::source_location::current(), Level::INFO, "Logger initialized"); });
}

void Logger::configure(const Config &cfg)
{
    std::unique_lock lock(configMutex);

    // close the current log file if open
    if (logFile.is_open())
    {
        logFile.close();
    }

    // update the configuration
    config = cfg;

    // reopen the log file with the new configuration
    if (config.fileOutput)
    {
        if (!std::filesystem::exists(config.logDir))
        {
            std::filesystem::create_directories(config.logDir);
        }

        std::string filename = std::format("{}/{}.log",
                                           config.logDir, config.filePrefix);
        logFile.open(filename, std::ios::app);
        if (!logFile)
        {
            throw std::runtime_error(std::format("Failed to open log file: {}", filename));
        }

        currentFileSize = std::filesystem::file_size(filename);
    }
}

void Logger::setLogLevel(Level level)
{
    std::unique_lock lock(configMutex);
    config.minLevel = level;
}

void Logger::setModuleName(std::string_view module)
{
    Context ::getThreadLocalModuleName() = std::string(module);
}

void Logger::destroyInstance()
{
    instance.reset();
}

Logger::~Logger()
{
    try
    {
        running = false;
        if (loggerThread.joinable())
        {
            loggerThread.request_stop();
            loggerThread.join();
        }
        if (logFile.is_open())
        {
            logFile.close();
        }
    }
    catch (...)
    {
        // Ignore exceptions in destructor
    }
}