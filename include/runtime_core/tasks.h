#pragma once
#include "enkiTs/TaskScheduler.h"
#include "global.h"
#include "runtime_plugin/plugin_manager.h"

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

// 周期调用插件 I/O 能力的任务。
class PluginIOTask : public enki::ITaskSet
{
public:
    PluginIOTask(const Config::TaskData& data,
                 runtime_plugin::DriverHandle* driver);
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

    Config::TaskState GetState() const { return m_state.load(); }
    runtime_plugin::DriverHandle* GetDriver() const { return m_driver; }

private:
    Config::TaskData m_data;
    runtime_plugin::DriverHandle* m_driver = nullptr;
    std::vector<uint8_t> m_readBuffer;
    std::vector<uint8_t> m_writeBuffer;
    std::vector<uint8_t> m_parameterBuffer;
    RtIoChannelV1 m_readChannel{};
    RtIoChannelV1 m_writeChannel{};
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
    std::atomic<uint64_t> m_executionCount{0};
};

// 周期调用插件 HookFunction 的任务。
class PluginHookTask : public enki::ITaskSet
{
public:
    PluginHookTask(const Config::TaskData& data,
                   runtime_plugin::PluginManager* manager);
    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;

    Config::TaskState GetState() const { return m_state.load(); }

private:
    Config::TaskData m_data;
    runtime_plugin::PluginManager* m_manager = nullptr;
    std::atomic<Config::TaskState> m_state{Config::TaskState::PENDING};
    std::atomic<uint64_t> m_executionCount{0};
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
