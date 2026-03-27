#pragma once
#include "enkiTs/TaskScheduler.h"
#include "global.h"

// 计算任务（消费者）
class ComputeConsumerTask : public enki::ITaskSet
{
public:
    explicit ComputeConsumerTask(const Config::TaskData& data);
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

private:
    Config::TaskData m_data;
};

// IO 线程常驻循环任务
class IOLoopTask : public enki::IPinnedTask
{
public:
    void Execute() override;
};

// 文件 IO 任务
class FileIOConsumerTask : public enki::IPinnedTask
{
public:
    explicit FileIOConsumerTask(const Config::TaskData& data);
    void Execute() override;

private:
    Config::TaskData m_data;
};

// 网络 IO 任务
class NetworkIOConsumerTask : public enki::IPinnedTask
{
public:
    explicit NetworkIOConsumerTask(const Config::TaskData& data);
    void Execute() override;

private:
    Config::TaskData m_data;
};