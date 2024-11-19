#include "blitz_logger.hpp"
#include <iostream>
#include <sstream>

// default constructor
Logger::Logger() : config(Config{}), running(true)
{
}

void Logger::processLogs(std::stop_token st)
{
    constexpr size_t BUFFER_SIZE = 10000;
    std::vector<LogMessage> currentBuffer;
    std::vector<LogMessage> processingBuffer;
    currentBuffer.reserve(BUFFER_SIZE);
    processingBuffer.reserve(BUFFER_SIZE);

    std::string fileBuffer;
    std::string consoleBuffer;
    fileBuffer.reserve(1024 * 1024);
    consoleBuffer.reserve(1024 * 1024);

    auto processBuffer = [&](std::vector<LogMessage> &buffer)
    {
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
        queueCV.notify_all();
    };

    while (!st.stop_requested())
    {
        std::vector<LogMessage> tempBuffer;
        {
            std::unique_lock lock(mutex);
            auto pred = [this]
            { return !messageQueue.empty() || !running; };

            if (queueCV.wait_for(lock, std::chrono::milliseconds(1), pred))
            {
                tempBuffer.reserve(messageQueue.size());
                while (!messageQueue.empty())
                {
                    tempBuffer.push_back(std::move(messageQueue.front()));
                    messageQueue.pop();
                }
            }
        }

        if (!tempBuffer.empty())
        {
            processBuffer(tempBuffer);
        }
    }

    // remaining messages
    {
        std::unique_lock lock(mutex);
        std::vector<LogMessage> remainingMessages;
        while (!messageQueue.empty())
        {
            remainingMessages.push_back(std::move(messageQueue.front()));
            messageQueue.pop();
        }
        if (!remainingMessages.empty())
        {
            processBuffer(remainingMessages);
        }
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

std::string Logger::formatLogMessage(const LogMessage &msg)
{
    constexpr size_t ESTIMATED_SIZE = 256;
    std::string result;
    result.reserve(ESTIMATED_SIZE);

    if (config.showTimestamp)
    {
        char timestamp[32];
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&time));

        result.append("[");
        result.append(timestamp);
        result.append(".");
        result.append(std::to_string(ms.count()));
        result.append("] ");
    }

    static constexpr std::string_view LEVEL_PREFIX = "[";
    static constexpr std::string_view LEVEL_SUFFIX = "] ";

    result.append(LEVEL_PREFIX);
    result.append(getLevelString(msg.level));
    result.append(LEVEL_SUFFIX);

    if (config.showThreadId)
    {
        result.append("[T-");
        result.append(std::to_string(std::hash<std::thread::id>{}(msg.context.threadId)));
        result.append("] ");
    }

    if (config.showModuleName && !msg.context.module.empty())
    {
        result.append("[");
        result.append(msg.context.module);
        result.append("] ");
    }

    if (config.showSourceLocation)
    {
        result.append("[");
        if (config.showFullPath)
        {
            result.append(msg.context.file);
        }
        else
        {
            std::string_view file(msg.context.file);
            if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
            {
                file = file.substr(pos + 1);
            }
            result.append(file);
        }
        result.append(":");
        result.append(std::to_string(msg.context.line));
        result.append("] ");
    }

    result.append(msg.message);
    return result;
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

const char *Logger::getLevelColor(Level level) const
{
    switch (level)
    {
    case Level::TRACE:
        return COLORS[7]; // white
    case Level::DEBUG:
        return COLORS[6]; // cyan
    case Level::INFO:
        return COLORS[3]; // green
    case Level::WARNING:
        return COLORS[4]; // yellow
    case Level::ERROR:
        return COLORS[2]; // red
    case Level::FATAL:
        return "\033[1;31m"; // bold red
    default:
        return COLORS[0]; // reset
    }
}

std::string_view Logger::getLevelString(Level level)
{
    switch (level)
    {
    case Level::TRACE:
        return "TRACE";
    case Level::DEBUG:
        return "DEBUG";
    case Level::INFO:
        return "INFO";
    case Level::WARNING:
        return "WARN";
    case Level::ERROR:
        return "ERROR";
    case Level::FATAL:
        return "FATAL";
    default:
        return "UNKNOWN";
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
        instance = std::unique_ptr<Logger>(new Logger());
        instance->configure(cfg); // configure logger
        
        instance->loggerThread = std::jthread([instance = instance.get()](std::stop_token st) {
            instance->processLogs(st);
        });
        
        instance->log(std::source_location::current(), Level::INFO, "Logger initialized"); });
}

void Logger::configure(const Config &cfg)
{
    std::unique_lock lock(mutex);

    if (logFile.is_open())
    {
        logFile.close();
    }

    config = cfg;

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
    std::unique_lock lock(mutex);
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
        queueCV.notify_all();
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
    }
}