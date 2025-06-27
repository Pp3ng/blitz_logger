#include "blitz_logger.hpp"
#include <iostream>
#include <sstream>
#include <iterator>
#include <algorithm>

Logger::BufferRegistry Logger::bufferRegistry;

// thread local buffer
Logger::ThreadLocalBuffer &Logger::getThreadLocalBuffer()
{
    static thread_local std::shared_ptr<ThreadLocalBuffer> localBuffer = nullptr;

    if (!localBuffer)
    {
        localBuffer = std::make_shared<ThreadLocalBuffer>();
        bufferRegistry.registerBuffer(localBuffer);

        // register cleanup on thread exit
        static thread_local struct ThreadCleanup
        {
            std::shared_ptr<ThreadLocalBuffer> buffer;
            ThreadCleanup(std::shared_ptr<ThreadLocalBuffer> buf) : buffer(buf) {}
            ~ThreadCleanup()
            {
                if (buffer)
                {
                    buffer->isActive.store(false, std::memory_order_release);
                    bufferRegistry.unregisterBuffer(buffer);
                }
            }
        } cleanup(localBuffer);
    }

    return *localBuffer;
}

// default constructor
Logger::Logger() : config(Config{}), running(true)
{
}

void Logger::processLogs()
{
    constexpr size_t BATCH_SIZE = 16384; // 16KB batch size
    std::vector<LogMessage> batchBuffer;
    batchBuffer.reserve(BATCH_SIZE);

    std::vector<char> fileBuffer;
    fileBuffer.reserve(2 * 1024 * 1024); // 2MB file buffer
    std::vector<char> consoleBuffer;
    consoleBuffer.reserve(2 * 1024 * 1024); // 2MB console buffer

    while (running.load(std::memory_order_relaxed))
    {
        bool messagesProcessed = false;
        bool anyBufferNearlyFull = false;

        auto buffers = bufferRegistry.getAllBuffers(); // round-robin access to all buffers
        for (auto &buffer : buffers)
        {
            if (!buffer->isActive.load(std::memory_order_relaxed))
                continue;

            // check if any buffer is nearly full
            if (buffer->isNearlyFull())
            {
                anyBufferNearlyFull = true;
            }

            LogMessage msg;
            size_t messagesFromThisBuffer = 0;
            const size_t maxMessagesPerBuffer = BATCH_SIZE / std::max(buffers.size(), size_t(1));

            // process messages from this buffer
            while (messagesFromThisBuffer < maxMessagesPerBuffer &&
                   batchBuffer.size() < BATCH_SIZE &&
                   buffer->pop(msg))
            {
                batchBuffer.push_back(std::move(msg));
                messagesProcessed = true;
                messagesFromThisBuffer++;
            }
        }
        if (!batchBuffer.empty())
        {
            processAndClearBatch(batchBuffer, fileBuffer, consoleBuffer);
        } // adaptive sleep: short sleep for high pressure, longer for low pressure
        if (!messagesProcessed)
        {
            auto sleep_duration = anyBufferNearlyFull ? std::chrono::microseconds(10) : std::chrono::microseconds(100);
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    // drain remaining messages before shutdown
    drainAllBuffers(batchBuffer, fileBuffer, consoleBuffer);
}

void Logger::processMessageBatch(
    const std::vector<LogMessage> &batch,
    std::vector<char> &fileBuffer,
    std::vector<char> &consoleBuffer)
{
    for (const auto &msg : batch)
    {
        if (config.fileOutput)
        {
            formatLogMessage(msg, fileBuffer);
            std::back_inserter(fileBuffer)++ = '\n';
        }

        if (config.consoleOutput)
        {
            if (config.useColors)
            {
                const char *color = getLevelColor(msg.level);
                size_t color_len = std::strlen(color);
                std::copy_n(color, color_len, std::back_inserter(consoleBuffer));
            }

            formatLogMessage(msg, consoleBuffer);

            if (config.useColors)
            {
                const char *reset = COLORS[COLOR_RESET];
                size_t reset_len = std::strlen(reset);
                std::copy_n(reset, reset_len, std::back_inserter(consoleBuffer));
            }
            std::back_inserter(consoleBuffer)++ = '\n';
        }
    }
    if (config.fileOutput && !fileBuffer.empty())
    {
        logFile.write(fileBuffer.data(), fileBuffer.size());
        currentFileSize += fileBuffer.size();
        rotateLogFileIfNeeded();
    }

    if (config.consoleOutput && !consoleBuffer.empty())
    {
        std::cout.write(consoleBuffer.data(), consoleBuffer.size());
    }
}

void Logger::drainAllBuffers(
    std::vector<LogMessage> &batchBuffer,
    std::vector<char> &fileBuffer,
    std::vector<char> &consoleBuffer)
{
    auto buffers = bufferRegistry.getAllBuffers();
    for (auto &buffer : buffers)
    {
        LogMessage msg;
        while (buffer->pop(msg))
        {
            batchBuffer.push_back(std::move(msg));
            if (batchBuffer.size() >= 4096)
            {
                processAndClearBatch(batchBuffer, fileBuffer, consoleBuffer);
            }
        }
    }
    processAndClearBatch(batchBuffer, fileBuffer, consoleBuffer);

    // final flush to ensure all data is written to disk
    if (config.fileOutput && logFile.is_open())
    {
        logFile.flush();
    }
}

void Logger::updateThreadStats()
{
    auto threadId = std::this_thread::get_id();

    std::lock_guard<std::mutex> lock(statsMapMutex);
    auto [it, inserted] = threadStatsMap.try_emplace(threadId, std::make_shared<ThreadStats>());

    if (inserted)
    {
        it->second->threadId = threadId;
    }
    it->second->messagesProduced.fetch_add(1, std::memory_order_relaxed);
}
void Logger::formatLogMessage(const LogMessage &msg, std::vector<char> &buffer) noexcept
{
    // calculate total required size to avoid reallocation
    size_t required_size = 256 + msg.message.size(); // base size

    if (config.showTimestamp)
        required_size += 32; // timestamp needs about 32 bytes
    if (config.showThreadId)
        required_size += 32; // thread id needs about 32 bytes
    if (config.showModuleName && !msg.context.module.empty())
        required_size += msg.context.module.size() + 3; // module name + "[] "
    if (config.showSourceLocation)
        required_size += msg.context.file.size() + 10; // file name + line number + "[] "

    // reserve space once
    size_t original_size = buffer.size();
    buffer.reserve(original_size + required_size);

    // use back_inserter to avoid repeated insert calls
    auto inserter = std::back_inserter(buffer);

    // format timestamp
    if (config.showTimestamp) [[likely]]
    {
        auto time = std::chrono::system_clock::to_time_t(msg.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      msg.timestamp.time_since_epoch()) %
                  1000;

        char time_buffer[64];
        size_t time_len = std::strftime(time_buffer, sizeof(time_buffer),
                                        "[%Y-%m-%d %H:%M:%S.", std::localtime(&time));

        // format milliseconds and closing bracket
        int ms_len = std::snprintf(time_buffer + time_len, sizeof(time_buffer) - time_len,
                                   "%03d] ", static_cast<int>(ms.count()));

        std::copy_n(time_buffer, time_len + ms_len, inserter);
    }

    // format log level
    const auto &level_str = LEVEL_STRINGS[static_cast<size_t>(msg.level)];
    *inserter++ = '[';
    std::copy(level_str.begin(), level_str.end(), inserter);
    *inserter++ = ']';
    *inserter++ = ' ';

    // format thread id
    if (config.showThreadId) [[likely]]
    {
        char thread_buffer[32];
        int thread_len = std::snprintf(thread_buffer, sizeof(thread_buffer),
                                       "[T-%zu] ",
                                       std::hash<std::thread::id>{}(msg.context.threadId));
        std::copy_n(thread_buffer, thread_len, inserter);
    }

    // format module name
    if (config.showModuleName && !msg.context.module.empty()) [[likely]]
    {
        *inserter++ = '[';
        std::copy(msg.context.module.begin(), msg.context.module.end(), inserter);
        *inserter++ = ']';
        *inserter++ = ' ';
    }

    // format source location
    if (config.showSourceLocation) [[likely]]
    {
        std::string_view file(msg.context.file);
        if (!config.showFullPath) [[likely]]
        {
            if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
                file = file.substr(pos + 1);
        }

        *inserter++ = '[';
        std::copy(file.begin(), file.end(), inserter);

        char line_buffer[16];
        int line_len = std::snprintf(line_buffer, sizeof(line_buffer),
                                     ":%d] ", msg.context.line);
        std::copy_n(line_buffer, line_len, inserter);
    }

    // append message content
    std::copy(msg.message.begin(), msg.message.end(), inserter);
}

void Logger::rotateLogFileIfNeeded()
{
    if (!config.fileOutput || currentFileSize < config.maxFileSize)
        return;

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
        
        instance->loggerThread = std::thread([instance = instance.get()]() {
            instance->processLogs();
        });
        
        instance->log(std::source_location::current(), Level::INFO, "Logger initialized with thread-local buffers"); });
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
    Context::getThreadLocalModuleName() = std::string(module);
}

void Logger::destroyInstance()
{
    instance.reset();
}

void Logger::printStats() const
{
    std::cout << "\n══════════════ Logger Statistics ══════════════\n\n";

    size_t totalProduced = 0;
    size_t activeThreads = 0;
    std::cout << std::setw(15) << "Thread ID"
              << std::setw(15) << "Produced" << "\n";
    std::cout << std::string(30, '-') << "\n";

    {
        std::lock_guard<std::mutex> lock(statsMapMutex);
        for (const auto &[threadId, stats] : threadStatsMap)
        {
            auto produced = stats->messagesProduced.load();

            std::cout << std::format("{:>15x}{:>15}\n",
                                     std::hash<std::thread::id>{}(threadId),
                                     produced);

            totalProduced += produced;
            activeThreads++;
        }
    }
    auto buffers = bufferRegistry.getAllBuffers();
    std::cout << std::string(30, '-') << "\n";
    std::cout << std::format("Active Threads: {}\n", activeThreads);
    std::cout << std::format("Active Buffers: {}\n", buffers.size());

    std::cout << std::string(30, '-') << "\n";
    std::cout << std::format("{:>15}{:>15}\n",
                             "Total",
                             totalProduced);
}

void Logger::processAndClearBatch(
    std::vector<LogMessage> &batchBuffer,
    std::vector<char> &fileBuffer,
    std::vector<char> &consoleBuffer)
{
    if (!batchBuffer.empty())
    {
        processMessageBatch(batchBuffer, fileBuffer, consoleBuffer);
        batchBuffer.clear();
        fileBuffer.clear();
        consoleBuffer.clear();
    }
}

Logger::~Logger()
{
    try
    {
        running.store(false, std::memory_order_release);

        if (loggerThread.joinable())
        {
            loggerThread.join();
        }
        if (logFile.is_open())
        {
            logFile.flush(); // final flush before closing
            logFile.close();
        }

        std::lock_guard<std::mutex> lock(statsMapMutex);
        threadStatsMap.clear();
    }
    catch (...)
    {
        // ignore exceptions in destructor
    }
}