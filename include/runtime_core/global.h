#pragma once
#include "enkiTs/TaskScheduler.h"
#include <atomic>
#include <cstdint>

class AppConfig
{
private:
    AppConfig() = default;
    ~AppConfig() = default;

    AppConfig(const AppConfig&) = delete;
    AppConfig& operator=(const AppConfig&) = delete;
    AppConfig(AppConfig&&) = delete;
    AppConfig& operator=(AppConfig&&) = delete;

public:
    static AppConfig* Instance();

    // IO 线程类型
    enum class IOThreadId
    {
        FILE_IO,
        NETWORK_IO_0,
        NETWORK_IO_1,
        NETWORK_IO_2,
        NUM
    };

    // 任务类型
    enum class TaskType
    {
        COMPUTE,
        FILE_IO,
        NETWORK_IO
    };

    // 任务数据
    struct TaskData
    {
        TaskType    type;
        int32_t     runId;
        int32_t     taskId;
    };

public:
    enki::TaskScheduler   ts;
    std::atomic<int32_t>  runCount = 0;
    std::atomic<bool>     stopFlag = false;
    uint32_t              ioThreadStartIndex = 0;
};

using Config = AppConfig;