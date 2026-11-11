#pragma once
#include "enkiTs/TaskScheduler.h"
#include "global.h"
#include "runtime_core/task_callback.h"
#include "runtime_core/triple_buffer.h"

// 计算任务（消费者）
class IecTask : public enki::ITaskSet
{
public:
    explicit IecTask(const Config::TaskData& data);
    ~IecTask() override;
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;
    Config::TaskState GetState() const { return m_state.load(); }
private:
    Config::TaskData m_data;
    RuntimeTaskCallback m_callback = nullptr;
    runtime_core::TaskBuffers m_buffers;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
    std::atomic<uint64_t> m_executionCount{0};
    std::atomic<bool> m_started{false};
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
    ~FileIOConsumerTask() override;
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

    Config::TaskState GetState() const { return m_state.load(); }

private:
    Config::TaskData m_data;
    RuntimeTaskCallback m_callback = nullptr;
    runtime_core::TaskBuffers m_buffers;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
    std::atomic<uint64_t> m_executionCount{0};
    std::atomic<bool> m_pendingConsumed{false};
    std::atomic<bool> m_started{false};
};

// 网络 IO 任务
class NetworkIOConsumerTask : public enki::ITaskSet
{
public:
    explicit NetworkIOConsumerTask(const Config::TaskData& data);
    ~NetworkIOConsumerTask() override;
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

    Config::TaskState GetState() const { return m_state.load(); }

private:
    Config::TaskData m_data;
    RuntimeTaskCallback m_callback = nullptr;
    runtime_core::TaskBuffers m_buffers;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
    std::atomic<uint64_t> m_executionCount{0};
    std::atomic<bool> m_pendingConsumed{false};
    std::atomic<bool> m_started{false};
};

// 周期遍历所有已加载插件并调用 HookFunction。
class PluginHookCycleTask : public enki::ITaskSet
{
public:
    explicit PluginHookCycleTask(const Config::TaskData& data);
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

    Config::TaskState GetState() const { return m_state.load(); }

private:
    Config::TaskData m_data;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
    std::atomic<uint64_t> m_executionCount{0};
};
