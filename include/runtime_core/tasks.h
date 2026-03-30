#pragma once
#include "enkiTs/TaskScheduler.h"
#include "global.h"

// 计算任务（消费者）
class IecTask : public enki::ITaskSet
{
public:
    explicit IecTask(const Config::TaskData& data);
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;
    Config::TaskState GetState() const { return m_state.load(); }
private:
    Config::TaskData m_data;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
};

// IEC 线程常驻循环任务
class IECLoopTask : public enki::IPinnedTask
{
public:
    void Execute() override;
};

// 文件 IO 任务
class FileIOConsumerTask : public enki::ITaskSet
{
public:
    explicit FileIOConsumerTask(const Config::TaskData& data);
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

    Config::TaskState GetState() const { return m_state.load(); }

private:
    Config::TaskData m_data;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
};

// 网络 IO 任务
class NetworkIOConsumerTask : public enki::ITaskSet
{
public:
    explicit NetworkIOConsumerTask(const Config::TaskData& data);
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

    Config::TaskState GetState() const { return m_state.load(); }

private:
    Config::TaskData m_data;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
};