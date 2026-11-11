#pragma once
#include "enkiTs/TaskScheduler.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace runtime_plugin {
class PluginManager;
}

class AppConfig {
private:
  AppConfig() = default;
  ~AppConfig() = default;

  AppConfig(const AppConfig &) = delete;
  AppConfig &operator=(const AppConfig &) = delete;
  AppConfig(AppConfig &&) = delete;
  AppConfig &operator=(AppConfig &&) = delete;

public:
  static AppConfig *Instance();

  // IO 线程类型
  enum class IOThreadId {
    FILE_IO,
    NETWORK_IO_0,
    NETWORK_IO_1,
    NETWORK_IO_2,
    NUM
  };

  // 任务类型
  enum class TaskType {
    IECTASK,
    FILE_IO,
    NETWORK_IO,
    PLUGIN_IO,
    PLUGIN_HOOK
  };

  // 任务状态
  enum class TaskState { PENDING, RUNNING, COMPLETED, FAILED };

  // 任务数据
  struct TaskData {
    TaskType type;
    int32_t runId;
    int32_t taskId;
    TaskState state = TaskState::PENDING;
    std::string taskLabel; // 唯一任务标识，比如 "compute-1", "file-0", "net-2"
    bool isPeriodic = false;
    uint32_t intervalMs = 0; // 周期时长（毫秒）
    int priority = 0;        // 优先级，数值越大优先级越高
    std::string pluginId;
    std::string pluginPath;
    std::string driverTypeId;
    uint32_t readBytes = 0;
    uint32_t writeBytes = 0;
    uint32_t parameterId = UINT32_MAX;
    uint32_t parameterBytes = 0;
    std::string taskConfigJson = "{}";
    uint32_t inputBytes = 4096;
    uint32_t outputBytes = 4096;
    std::string executorName = "default";
  };

  struct PeriodicTaskInfo {
    bool isPinned = false;
    enki::ITaskSet *taskSet = nullptr;
    enki::IPinnedTask *pinnedTask = nullptr;
    TaskData data;
    std::chrono::steady_clock::time_point nextRun;
    bool ownsTaskSet = true;
  };

  // IPC 配置结构体
  struct IPCConfig {
    std::string routeName = "runtime_task_route"; // IPC 路由名称
    uint64_t waitTimeoutUs = 1000; // 等待超时时间（微秒），默认 100ms
    bool enabled = true;                // 是否启用 IPC
    uint64_t receiverCount = 1; // 预设接收者数量（用于调度优化）
    IPCConfig() = default;
  };

public:
  enki::TaskScheduler ts;
  std::atomic<int32_t> runCount = 0;
  std::atomic<bool> stopFlag = false;
  uint32_t ioThreadStartIndex = 0;
  uint32_t pluginHookCycleIntervalMs = 1;
  int pluginHookCyclePriority = 0;

  // IPC 配置
  IPCConfig ipcConfig;

  std::atomic<int> ioTasksPending = 0;
  std::atomic<int> ioTasksRunning = 0;
  std::atomic<int> ioTasksCompleted = 0;
  std::atomic<int> ioTasksFailed = 0;

  std::vector<PeriodicTaskInfo> periodicTasks;
  std::mutex periodicTasksMutex;

  // 插件在 main 中创建，生命周期覆盖所有调度器和 IEC 线程。
  runtime_plugin::PluginManager *plugin_manager = nullptr;
};

using Config = AppConfig;
