#pragma once
#include "enkiTs/TaskScheduler.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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
  enum class TaskType { IECTASK, FILE_IO, NETWORK_IO };

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
  };

  struct PeriodicTaskInfo {
    bool isPinned = false;
    enki::ITaskSet *taskSet = nullptr;
    enki::IPinnedTask *pinnedTask = nullptr;
    TaskData data;
    std::chrono::steady_clock::time_point nextRun;
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

  // IPC 配置
  IPCConfig ipcConfig;

  std::atomic<int> ioTasksPending = 0;
  std::atomic<int> ioTasksRunning = 0;
  std::atomic<int> ioTasksCompleted = 0;
  std::atomic<int> ioTasksFailed = 0;

  std::vector<PeriodicTaskInfo> periodicTasks;
  std::mutex periodicTasksMutex;
};

using Config = AppConfig;
