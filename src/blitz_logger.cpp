#include "blitz_logger.hpp"
#include <iostream>
#include <sstream>
#include <iterator>
#include <algorithm>

thread_local size_t Logger::currentShardId = std::numeric_limits<size_t>::max(); // initialize to max

size_t Logger::getShardId() noexcept
{
    constexpr size_t MAX_RETRY = 3;

    if (currentShardId == std::numeric_limits<size_t>::max()) // never accessed before
    {
        // assign a shard id to thread
        currentShardId = nextShardId.fetch_add(1, std::memory_order_relaxed) % NUM_SHARDS;
        return currentShardId;
    }

    // when the current shard is full, try neighboring shards(spatial locality)
    size_t shardId = currentShardId;
    for (size_t retry = 0; retry < MAX_RETRY; ++retry)
    {
        if (!shards[shardId].isFull())
        {
            return shardId;
        }
        shardId = (shardId + 1) % NUM_SHARDS;
    }

    // if all retries fail, randomly select a new shard
    return nextShardId.fetch_add(1, std::memory_order_relaxed) % NUM_SHARDS;
}

// default constructor
Logger::Logger() : config(Config{}), running(true)
{
}

void Logger::processLogs(std::stop_token st)
{
    constexpr size_t BATCH_SIZE = 16384; // 16KB
    std::vector<LogMessage> batchBuffer;
    batchBuffer.reserve(BATCH_SIZE);

    std::vector<char> fileBuffer;
    fileBuffer.reserve(1024 * 1024);
    std::vector<char> consoleBuffer;
    consoleBuffer.reserve(1024 * 1024);

    size_t currentShard = 0;
    size_t emptyIterations = 0;

    while (!st.stop_requested())
    {
        bool messagesProcessed = false;
        LogMessage msg;

        while (batchBuffer.size() < BATCH_SIZE &&
               shards[currentShard].pop(msg))
        {
            batchBuffer.push_back(std::move(msg));
            messagesProcessed = true;
        }

        if (!messagesProcessed)
        {
            currentShard = (currentShard + 1) % NUM_SHARDS;
            emptyIterations++;

            if (emptyIterations >= NUM_SHARDS)
            {
                emptyIterations = 0;
                if (batchBuffer.empty())
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
            }
        }
        else
        {
            emptyIterations = 0;
        }

        if (!batchBuffer.empty())
        {
            processMessageBatch(batchBuffer, fileBuffer, consoleBuffer);
            batchBuffer.clear();
            fileBuffer.clear();
            consoleBuffer.clear();
        }
    }

    drainRemainingMessages(batchBuffer, fileBuffer, consoleBuffer);
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
        std::cout.flush();
    }
}

void Logger::drainRemainingMessages(
    std::vector<LogMessage> &batchBuffer,
    std::vector<char> &fileBuffer,
    std::vector<char> &consoleBuffer)
{
    for (size_t i = 0; i < NUM_SHARDS; ++i)
    {
        LogMessage msg;
        while (shards[i].pop(msg))
        {
            batchBuffer.push_back(std::move(msg));
            if (batchBuffer.size() >= 4096)
            {
                processMessageBatch(batchBuffer, fileBuffer, consoleBuffer);
                batchBuffer.clear();
                fileBuffer.clear();
                consoleBuffer.clear();
            }
        }
    }

    if (!batchBuffer.empty())
    {
        processMessageBatch(batchBuffer, fileBuffer, consoleBuffer);
    }
}

void Logger::updateShardStats(size_t shardId, bool pushSuccess)
{
    auto &stats = shardStats[shardId];
    stats.pushAttempts.fetch_add(1, std::memory_order_relaxed);

    if (!pushSuccess)
    {
        stats.pushFailures.fetch_add(1, std::memory_order_relaxed);
    }
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

void Logger::printStats() const
{
    std::cout << "\n══════════════ Logger Statistics ══════════════\n\n";

    size_t totalAttempts = 0;
    size_t totalFailures = 0;

    // print header
    std::cout << std::setw(10) << "Shard"
              << std::setw(15) << "Success"
              << std::setw(15) << "Failures"
              << std::setw(15) << "Rate(%)" << "\n";
    std::cout << std::string(55, '-') << "\n";

    // print shard details
    for (size_t i = 0; i < NUM_SHARDS; ++i)
    {
        const auto &stats = shardStats[i];
        auto attempts = stats.pushAttempts.load();
        auto failures = stats.pushFailures.load();
        auto successes = attempts - failures;

        std::cout << std::format("Shard {:<2}{:>15}{:>15}{:>14.1f}\n",
                                 i,
                                 successes,
                                 failures,
                                 attempts > 0 ? (successes * 100.0 / attempts) : 100.0);

        totalAttempts += attempts;
        totalFailures += failures;
    }

    // print summary
    std::cout << std::string(55, '-') << "\n";
    std::cout << std::format("{:>8}{:>15}{:>15}{:>14.1f}\n",
                             "Total",
                             totalAttempts - totalFailures,
                             totalFailures,
                             totalAttempts > 0 ? ((totalAttempts - totalFailures) * 100.0 / totalAttempts) : 100.0);

    std::cout << "\n═══════════════════════════════════════════════\n";
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
        // ignore exceptions in destructor
    }
}