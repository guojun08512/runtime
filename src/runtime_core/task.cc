#include "runtime_core/tasks.h"
#include <glog/logging.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------------
void IECLoopTask::Execute() {
  auto *cfg = Config::Instance();
  LOG(INFO) << "[IEC:IO" << threadNum << "] 已启动";

  while (!cfg->ts.GetIsShutdownRequested() && !cfg->stopFlag) {
    bool isScheduler = (threadNum == cfg->ioThreadStartIndex);

    if (isScheduler) {
      // 周期性任务调度：根据 TaskData.intervalMs 来触发执行
      auto now = std::chrono::steady_clock::now();
      std::vector<Config::PeriodicTaskInfo*> dueTasks;
      {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        dueTasks.reserve(cfg->periodicTasks.size());
        for (auto &entry : cfg->periodicTasks) {
          if (entry.data.isPeriodic && now >= entry.nextRun) {
            dueTasks.push_back(&entry);
          }
        }

        // 优先级从高到低调度。持锁提交任务，防止生产线程扩容任务表后
        // dueTasks 中的指针失效。
        std::sort(dueTasks.begin(), dueTasks.end(),
                  [](const Config::PeriodicTaskInfo *a,
                     const Config::PeriodicTaskInfo *b) {
                    return a->data.priority > b->data.priority;
                  });

        for (auto *entry : dueTasks) {
          if (entry->isPinned && entry->pinnedTask) {
            cfg->ts.AddPinnedTask(entry->pinnedTask);
          } else if (!entry->isPinned && entry->taskSet) {
            cfg->ts.AddTaskSetToPipe(entry->taskSet);
          }

          // 基于上次预定时间连续计算，避免调度延迟越积越大
          auto next =
              entry->nextRun +
              std::chrono::milliseconds(entry->data.intervalMs);
          while (next <= now) {
            next += std::chrono::milliseconds(entry->data.intervalMs);
          }
          entry->nextRun = next;
        }
      }

      // 计算下一次唤醒时间
      std::chrono::steady_clock::time_point nextWake = std::chrono::steady_clock::time_point::max();
      {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        for (auto &entry : cfg->periodicTasks) {
          if (entry.data.isPeriodic) {
            nextWake = std::min(nextWake, entry.nextRun);
          }
        }
      }

      auto now2 = std::chrono::steady_clock::now();
      if (nextWake != std::chrono::steady_clock::time_point::max()) {
        auto sleepDuration = std::chrono::duration_cast<std::chrono::milliseconds>(nextWake - now2);
        if (sleepDuration < std::chrono::milliseconds(1)) {
          sleepDuration = std::chrono::milliseconds(1);
        }
        std::this_thread::sleep_for(sleepDuration);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else {
      // 其他 IO 线程只执行已刷入的 pinned 任务，避免重复调度
      cfg->ts.RunPinnedTasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  LOG(INFO) << "[IEC:IO" << threadNum << "] 已退出";
}

// -----------------------------------------------------------------------------

IecTask::IecTask(const Config::TaskData &data) : m_data(data) {
  m_SetSize = 1;
  m_data.state = Config::TaskState::PENDING;
}

void IecTask::ExecuteRange(enki::TaskSetPartition, uint32_t threadNum) {
  auto *cfg = Config::Instance();
  const bool isIECThread = threadNum >= cfg->ts.GetNumTaskThreads() -
                                            (uint32_t)Config::IOThreadId::NUM;

  if (isIECThread) {
    LOG(ERROR) << "计算任务在 IEC 线程 " << threadNum << " 执行";
    return;
  }
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);

  try {
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    LOG(INFO) << "[IEC] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskLabel << " | 状态: COMPLETED";
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    LOG(ERROR) << "[IEC] 线程" << threadNum << " | 轮次" << m_data.runId
               << " | 任务" << m_data.taskLabel << " | 状态: FAILED";
  }
}

// -----------------------------------------------------------------------------

FileIOConsumerTask::FileIOConsumerTask(const Config::TaskData &data)
    : m_data(data) {
  m_data.state = Config::TaskState::PENDING;
}

void FileIOConsumerTask::ExecuteRange(enki::TaskSetPartition range,
                                      uint32_t threadNum) {
  auto *cfg = Config::Instance();

  cfg->ioTasksPending.fetch_sub(1, std::memory_order_relaxed);
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  try {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
    LOG(INFO) << "[文件IO] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskLabel << " | 状态: COMPLETED";
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    LOG(ERROR) << "[文件IO] 线程" << threadNum << " | 轮次" << m_data.runId
               << " | 任务" << m_data.taskId << " | 状态: FAILED";
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------

NetworkIOConsumerTask::NetworkIOConsumerTask(const Config::TaskData &data)
    : m_data(data) {
  m_data.state = Config::TaskState::PENDING;
}

void NetworkIOConsumerTask::ExecuteRange(enki::TaskSetPartition range,
                                         uint32_t threadNum) {
  auto *cfg = Config::Instance();
  cfg->ioTasksPending.fetch_sub(1, std::memory_order_relaxed);
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  try {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
    LOG(INFO) << "[网络IO] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskLabel << " | 状态: COMPLETED";
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    LOG(ERROR) << "[网络IO] 线程" << threadNum << " | 轮次" << m_data.runId
               << " | 任务" << m_data.taskId << " | 状态: FAILED";
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------

PluginIOTask::PluginIOTask(const Config::TaskData &data,
                           runtime_plugin::DriverHandle *driver)
    : m_data(data), m_driver(driver),
      m_readBuffer(data.readBytes), m_writeBuffer(data.writeBytes),
      m_parameterBuffer(data.parameterBytes) {
  m_SetSize = 1;
  m_data.state = Config::TaskState::PENDING;

  m_readChannel.data = m_readBuffer.empty() ? nullptr : m_readBuffer.data();
  m_readChannel.size_bytes = static_cast<uint32_t>(m_readBuffer.size());
  m_writeChannel.data = m_writeBuffer.empty() ? nullptr : m_writeBuffer.data();
  m_writeChannel.size_bytes = static_cast<uint32_t>(m_writeBuffer.size());
  std::fill(m_writeBuffer.begin(), m_writeBuffer.end(), 0xA5);
}

void PluginIOTask::ExecuteRange(enki::TaskSetPartition,
                                uint32_t threadNum) {
  auto *cfg = Config::Instance();
  auto *manager = cfg->plugin_manager;

  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  RtResult result = RT_OK;
  try {
    if (manager == nullptr || m_driver == nullptr) {
      result = RT_ERR_PARAMETER;
    } else {
      result = manager->StartBusCycle(m_driver);
      if (!m_writeBuffer.empty()) {
        if (result == RT_OK) {
          m_writeBuffer[0] =
              static_cast<uint8_t>(m_executionCount.load(
                  std::memory_order_relaxed));
          result = manager->WriteOutputs(m_driver, &m_writeChannel, 1);
        }
      }
      if (result == RT_OK && !m_readBuffer.empty()) {
        result = manager->ReadInputs(m_driver, &m_readChannel, 1);
      }
      if (result == RT_OK && !m_parameterBuffer.empty() &&
          m_data.parameterId != UINT32_MAX) {
        result = manager->ReadParameter(m_driver,
                                        m_data.parameterId,
                                        m_parameterBuffer.data(),
                                        static_cast<uint32_t>(
                                            m_parameterBuffer.size()),
                                        0);
      }
      if (result == RT_OK) {
        result = manager->TriggerWatchdog(m_driver);
      }
    }
  } catch (...) {
    result = RT_ERR_FAILED;
  }

  const uint64_t execution =
      m_executionCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (result == RT_OK) {
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
  } else {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
  }

  if (execution == 1 || execution % 100 == 0 || result != RT_OK) {
    if (result == RT_OK) {
      LOG(INFO) << "[插件IO] 线程" << threadNum << " | 轮次"
                << m_data.runId << " | 任务" << m_data.taskLabel
                << " | 插件" << m_data.pluginId
                << " | 类型" << m_data.driverTypeId
                << " | 状态: COMPLETED"
                << " | 执行次数: " << execution;
    } else {
      LOG(ERROR) << "[插件IO] 线程" << threadNum << " | 轮次"
                 << m_data.runId << " | 任务" << m_data.taskLabel
                 << " | 插件" << m_data.pluginId
                 << " | 类型" << m_data.driverTypeId
                 << " | 状态: FAILED"
                 << " | 执行次数: " << execution;
    }
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------

PluginHookTask::PluginHookTask(const Config::TaskData &data,
                               runtime_plugin::PluginManager *manager)
    : m_data(data), m_manager(manager) {
  m_SetSize = 1;
  m_data.state = Config::TaskState::PENDING;
}

void PluginHookTask::ExecuteRange(enki::TaskSetPartition,
                                  uint32_t threadNum) {
  auto *cfg = Config::Instance();

  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  const uint64_t execution = m_executionCount.load(std::memory_order_relaxed);
  std::string error;
  RtResult result = RT_ERR_PARAMETER;
  if (m_manager != nullptr) {
    result = m_manager->CallHook(m_data.pluginId,
                                 RT_HOOK_COMM_CYCLE,
                                 static_cast<uintptr_t>(execution),
                                 0,
                                 &error);
  }

  const uint64_t completed =
      m_executionCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (result == RT_OK) {
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
  } else {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
  }

  if (completed == 1 || completed % 100 == 0 || result != RT_OK) {
    if (result == RT_OK) {
      LOG(INFO) << "[插件Hook] 线程" << threadNum << " | 轮次"
                << m_data.runId << " | 任务" << m_data.taskLabel
                << " | 插件" << m_data.pluginId
                << " | 状态: COMPLETED"
                << " | 执行次数: " << completed;
    } else {
      LOG(ERROR) << "[插件Hook] 线程" << threadNum << " | 轮次"
                 << m_data.runId << " | 任务" << m_data.taskLabel
                 << " | 插件" << m_data.pluginId
                 << " | 状态: FAILED"
                 << " | 执行次数: " << completed
                 << " | 错误: " << error;
    }
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------

PluginHookCycleTask::PluginHookCycleTask(const Config::TaskData &data)
    : m_data(data) {
  m_SetSize = 1;
  m_data.state = Config::TaskState::PENDING;
}

void PluginHookCycleTask::ExecuteRange(enki::TaskSetPartition,
                                       uint32_t threadNum) {
  auto *cfg = Config::Instance();
  auto *manager = cfg->plugin_manager;

  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  const uint64_t execution = m_executionCount.load(std::memory_order_relaxed);
  std::string error;
  RtResult result =
      manager == nullptr
          ? RT_ERR_PARAMETER
          : manager->CallAllHooks(RT_HOOK_COMM_CYCLE,
                                  static_cast<uintptr_t>(execution),
                                  0,
                                  &error);

  const uint64_t completed =
      m_executionCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (result == RT_OK) {
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
  } else {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
  }

  if (completed == 1 || completed % 1000 == 0 || result != RT_OK) {
    if (result == RT_OK) {
      LOG(INFO) << "[插件HookAll] 线程" << threadNum << " | 任务"
                << m_data.taskLabel
                << " | 状态: COMPLETED"
                << " | 执行次数: " << completed;
    } else {
      LOG(ERROR) << "[插件HookAll] 线程" << threadNum << " | 任务"
                 << m_data.taskLabel
                 << " | 状态: FAILED"
                 << " | 执行次数: " << completed
                 << " | 错误: " << error;
    }
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}
