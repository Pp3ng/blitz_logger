#include "blitz_logger.hpp"
#include <iostream>
#include <sstream>

// default constructor
Logger::Logger() : config(Config{}), running(true)
{
}

void Logger::processLogs(std::stop_token st)
{
    // batch size chosen to balance memory usage and throughput
    constexpr size_t BATCH_SIZE = 4096;

    // pre-allocate buffers to avoid frequent reallocations
    std::vector<LogMessage> batchBuffer;
    batchBuffer.reserve(BATCH_SIZE);

    // allocate 1MB for each output buffer
    std::vector<char> fileBuffer;
    fileBuffer.reserve(1024 * 1024);
    std::vector<char> consoleBuffer;
    consoleBuffer.reserve(1024 * 1024);

    // lambda to process a batch of messages
    auto processBuffer = [&](std::span<LogMessage> buffer)
    {
        if (buffer.empty())
            return;

        // clear buffers for reuse
        fileBuffer.clear();
        consoleBuffer.clear();

        // process each message in the batch
        for (const auto &msg : buffer)
        {
            // handle file output
            if (config.fileOutput)
            {
                formatLogMessage(msg, fileBuffer);
                fileBuffer.push_back('\n');
            }

            // handle console output with optional colors
            if (config.consoleOutput)
            {
                if (config.useColors)
                {
                    // add color prefix
                    std::string_view levelColor = getLevelColor(msg.level);
                    consoleBuffer.insert(consoleBuffer.end(), levelColor.begin(), levelColor.end());

                    // format message
                    formatLogMessage(msg, consoleBuffer);

                    // add color reset and newline
                    consoleBuffer.insert(consoleBuffer.end(), COLORS[0], COLORS[0] + strlen(COLORS[0]));
                    consoleBuffer.push_back('\n');
                }
                else
                {
                    formatLogMessage(msg, consoleBuffer);
                    consoleBuffer.push_back('\n');
                }
            }
        }

        // write file buffer if needed
        if (config.fileOutput && logFile.is_open() && !fileBuffer.empty())
        {
            logFile.write(fileBuffer.data(), fileBuffer.size());
            logFile.flush();
            currentFileSize += fileBuffer.size();
            rotateLogFileIfNeeded();
        }

        // write console buffer if needed
        if (config.consoleOutput && !consoleBuffer.empty())
        {
            std::cout.write(consoleBuffer.data(), consoleBuffer.size());
            std::cout.flush();
        }
    };

    // main processing loop
    while (!st.stop_requested())
    {
        LogMessage msg;
        // collect messages until batch is full or queue is empty
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
            // sleep briefly when no messages are available
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // process remaining messages before shutdown
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

    // process final batch if any messages remain
    if (!batchBuffer.empty())
    {
        processBuffer(std::span<LogMessage>(batchBuffer.data(), batchBuffer.size()));
    }
}

void Logger::formatLogMessage(const LogMessage &msg, std::vector<char> &buffer) noexcept
{
    // estimate required space to minimize reallocations
    // base size (256) accounts for timestamps, level, thread id etc
    const size_t estimated_size = 256 + msg.message.size();
    buffer.reserve(buffer.size() + estimated_size);

    // format timestamp if enabled
    if (config.showTimestamp) [[likely]]
    {
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;

        char time_buffer[32];
        size_t time_len = std::strftime(time_buffer, sizeof(time_buffer),
                                        "[%Y-%m-%d %H:%M:%S.", std::localtime(&time));
        buffer.insert(buffer.end(), time_buffer, time_buffer + time_len);

        char ms_buffer[8];
        int ms_len = std::snprintf(ms_buffer, sizeof(ms_buffer),
                                   "%03d] ", static_cast<int>(ms.count()));
        buffer.insert(buffer.end(), ms_buffer, ms_buffer + ms_len);
    }

    // format log level
    const auto &level_str = LEVEL_STRINGS[static_cast<size_t>(msg.level)];
    buffer.push_back('[');
    buffer.insert(buffer.end(), level_str.begin(), level_str.end());
    buffer.insert(buffer.end(), {']', ' '});

    // format thread id if enabled
    if (config.showThreadId) [[likely]]
    {
        char thread_buffer[32];
        int thread_len = std::snprintf(thread_buffer, sizeof(thread_buffer),
                                       "[T-%zu] ",
                                       std::hash<std::thread::id>{}(msg.context.threadId));
        buffer.insert(buffer.end(), thread_buffer, thread_buffer + thread_len);
    }

    // format module name if enabled and not empty
    if (config.showModuleName && !msg.context.module.empty()) [[likely]]
    {
        buffer.push_back('[');
        buffer.insert(buffer.end(), msg.context.module.begin(), msg.context.module.end());
        buffer.insert(buffer.end(), {']', ' '});
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

        buffer.push_back('[');
        buffer.insert(buffer.end(), file.begin(), file.end());

        char line_buffer[16];
        int line_len = std::snprintf(line_buffer, sizeof(line_buffer),
                                     ":%d] ", msg.context.line);
        buffer.insert(buffer.end(), line_buffer, line_buffer + line_len);
    }

    // append the actual message content
    buffer.insert(buffer.end(), msg.message.begin(), msg.message.end());
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