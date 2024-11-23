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
    std::vector<LogMessage> batchBuffer;
    batchBuffer.reserve(BATCH_SIZE);

    std::string fileBuffer;
    std::string consoleBuffer;
    fileBuffer.reserve(1024 * 1024);
    consoleBuffer.reserve(1024 * 1024);

    auto processBuffer = [&](std::vector<LogMessage> &buffer)
    {
        if (buffer.empty())
            return;

        fileBuffer.clear();
        consoleBuffer.clear();

        for (const auto &msg : buffer)
        {
            if (config.fileOutput)
            {
                fileBuffer += formatLogMessage(msg);
                fileBuffer += '\n';
            }

            if (config.consoleOutput)
            {
                if (config.useColors)
                {
                    consoleBuffer += getLevelColor(msg.level);
                    consoleBuffer += formatLogMessage(msg);
                    consoleBuffer += COLORS[0];
                    consoleBuffer += '\n';
                }
                else
                {
                    consoleBuffer += formatLogMessage(msg);
                    consoleBuffer += '\n';
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

        buffer.clear();
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
            processBuffer(batchBuffer);
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
            processBuffer(batchBuffer);
            batchBuffer.clear();
        }
    }

    if (!batchBuffer.empty())
    {
        processBuffer(batchBuffer);
    }
}

void Logger::writeLogMessage(const LogMessage &msg)
{
    std::string formattedMessage = formatLogMessage(msg);

    // write to file
    if (config.fileOutput && logFile.is_open())
    {
        logFile << formattedMessage << std::endl;
        currentFileSize += formattedMessage.size() + 1;
        if (msg.level >= Level::ERROR) // flush ERROR and FATAL logs immediately
        {
            logFile.flush();
        }
        else if (currentFileSize % (256 * 1024) == 0) // flush every 256KB
        {
            logFile.flush();
        }
        rotateLogFileIfNeeded();
    }

    // write to console
    if (config.consoleOutput)
    {
        if (config.useColors)
        {
            std::cout << getLevelColor(msg.level)
                      << formattedMessage
                      << COLORS[0] << std::endl;
        }
        else
        {
            std::cout << formattedMessage << std::endl;
        }
    }
}

[[nodiscard]]
std::string Logger::formatLogMessage(const LogMessage &msg) noexcept
{
    alignas(16) char buffer[1024];
    size_t offset = 0;

    // timestamp
    if (config.showTimestamp)
    {
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;

        offset += std::strftime(buffer + offset, sizeof(buffer) - offset,
                                "[%Y-%m-%d %H:%M:%S.", std::localtime(&time));
        offset += std::snprintf(buffer + offset, sizeof(buffer) - offset,
                                "%03d] ", static_cast<int>(ms.count()));
    }

    static constexpr std::array<std::string_view, 7> LEVEL_STRINGS = {
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "STEP"};

    const auto &level_str = LEVEL_STRINGS[static_cast<size_t>(msg.level)];
    offset += std::snprintf(buffer + offset, sizeof(buffer) - offset,
                            "[%.*s] ", static_cast<int>(level_str.size()), level_str.data());

    // thread id
    if (config.showThreadId)
    {
        offset += std::snprintf(buffer + offset, sizeof(buffer) - offset,
                                "[T-%zu] ",
                                std::hash<std::thread::id>{}(msg.context.threadId));
    }

    // module name
    if (config.showModuleName && !msg.context.module.empty())
    {
        offset += std::snprintf(buffer + offset, sizeof(buffer) - offset,
                                "[%s] ", msg.context.module.c_str());
    }

    // source location
    if (config.showSourceLocation)
    {
        std::string_view file(msg.context.file);
        if (!config.showFullPath)
        {
            if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
            {
                file = file.substr(pos + 1);
            }
        }
        offset += std::snprintf(buffer + offset, sizeof(buffer) - offset,
                                "[%.*s:%d] ", static_cast<int>(file.size()), file.data(), msg.context.line);
    }

    // message content
    const size_t remaining = sizeof(buffer) - offset;
    const size_t msg_len = std::min(msg.message.size(), remaining - 1);
    std::memcpy(buffer + offset, msg.message.data(), msg_len);
    offset += msg_len;
    buffer[offset] = '\0';

    return std::string(buffer, offset);
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
        std::set_terminate(terminateHandler);
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